# pi-printer — Project Guide for Claude

## Current Job

Job state and plan: `/Users/jlh5498/jobs/how-jobs/working/pi-printer/`

## CRITICAL: All commands run via SSH

All commands (ipptool, journalctl, etc.) must be run on the Pi via SSH:

```
ssh tuttle@tuttle-pi.local "COMMAND"
```

Never ask the user to run commands manually — Claude runs them via SSH.

## Deploying changes to the Pi

Use git, not rsync. The full sequence is:

```bash
# 1. Commit locally (on Mac)
git add <files>
git commit -m "message"

# 2. Push to GitHub
git push

# 3. Pull, build, and install on the Pi
ssh tuttle@tuttle-pi.local "cd /home/tuttle/pi-printer && git pull && make && sudo make install"
```

`sudo make install` builds the binary and restarts the service. Always use this
form — never `sudo systemctl restart` directly.

`sudo make install` needs an interactive terminal for the password. If it fails
with "a terminal is required to read the password", ask the user to run it once
manually in their own terminal (this caches the sudo credential for ~15 min):

```bash
ssh tuttle@tuttle-pi.local "cd /home/tuttle/pi-printer && sudo make install"
```

## Known PAPPL API gaps

`PAPPL_SUPPLY_TYPE_DRUM_IMAGING` is not defined in PAPPL — use
`PAPPL_SUPPLY_TYPE_OPC` (optical photoconductor) for drum units.

## What this project is

A PAPPL Printer Application for the Brother HL-5170DN, running as a systemd
service on a Raspberry Pi. It exposes an IPP endpoint directly — it is NOT a
CUPS driver.

Service: `hl5170dn-printer-app`
IPP endpoint: `ipp://localhost:8000/ipp/print`
Web UI: `http://tuttle-pi.local:8000/`

## CRITICAL: Job submission must use ipptool — never lp/lpr/CUPS

**Do not use `lp`, `lpr`, `lpadmin`, or any CUPS client tool to submit test jobs.**

Reasons this will fail or give wrong results:
1. `lp -h localhost:8000 -d hl5170dn` fails with "printer-uri not found" — CUPS
   constructs `/printers/hl5170dn` but PAPPL serves at `/ipp/print`.
2. Even when CUPS is configured as a proxy (`lpadmin -p ... -v ipp://...`), CUPS
   rasterizes PDFs to `image/urf` (URF) before forwarding. PAPPL receives URF and
   takes the raster path, bypassing `pdf_filter_cb`. This means:
   - APT (Mode 1024, `print-quality=5`) never triggers — APT is initiated only in `pdf_filter_cb`.
   - GS is not invoked; `gs cmd:` log lines do not appear.
   - Resolution is fixed at 600 dpi regardless of the `printer-resolution` IPP attribute.
3. `hl5170dn-printer-app submit` is a standalone PAPPL process; it is not an IPP
   client to the running service. Its logs go to its own stderr, not journalctl.

**The only correct tool for test job submission is `ipptool`.**

### ipptool invocation pattern

```
ipptool -tv \
  -f /path/to/input.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/TESTFILE.test
```

Variables:
- `-f FILE` sets `$filename` in the test file
- `-d filetype=TYPE` sets `$filetype` (use `application/pdf` for PDFs)
- Additional `-d KEY=VALUE` pairs set other `$KEY` variables used in the test file
- The printer URI becomes `$uri` automatically

### Test files

Pre-built test files live in `/home/tuttle/pi-printer/tests/`:

| File | Sides | Resolution | Quality | Media |
|------|-------|------------|---------|-------|
| `print-duplex-long-300.test` | long-edge | 300 dpi | — | Letter |
| `print-duplex-long-600.test` | long-edge | 600 dpi | — | Letter |
| `print-duplex-short.test` | short-edge | — | — | Letter |
| `print-simplex.test` | one-sided | — | — | Letter |
| `print-apt.test` | long-edge | — | 5 (APT) | Letter |
| `print-quality4-duplex-long-600.test` | long-edge | 600 dpi | 4 | Letter |
| `print-a4-duplex-long.test` | long-edge | — | — | A4 |
| `print-envelope-simplex.test` | one-sided | — | — | DL |
| `print-a4-reject.test` | long-edge | — | — | A4 + reject |
| `print-jobname.test` | long-edge | — | — | Letter (needs -d jobname=) |

System-provided test files (read-only queries, no job creation):

```
/usr/share/cups/ipptool/get-printer-attributes.test
/usr/share/cups/ipptool/get-jobs.test
```

### IPP attribute types for new test files

```
ATTR keyword  sides                  two-sided-long-edge   # or one-sided, two-sided-short-edge
ATTR resolution printer-resolution  300dpi                 # or 600dpi
ATTR enum     print-quality         5                      # 3=draft 4=normal 5=high
ATTR keyword  media                 iso_a4_210x297mm       # or na_letter_8.5x11in, iso_dl_110x220mm
ATTR keyword  media-mismatch-action reject                 # PAPPL custom attribute
ATTR name     job-name              $jobname               # set via -d jobname=VALUE
```

### Cancellation

Cancellation (not job submission) uses the PAPPL CLI:

```
hl5170dn-printer-app cancel -d hl5170dn
```

## Log inspection

`-o cat` strips journalctl's metadata prefix and shows only the PAPPL log line
(`I [timestamp] message`), eliminating double-timestamp noise.

```
# Interactive reading (paged, follow recent output)
SYSTEMD_LESS=FRXMK journalctl -u hl5170dn-printer-app -o cat --since "5 minutes ago"

# Grep pipeline (standard form used in test log checks)
journalctl -u hl5170dn-printer-app -o cat --since "5 minutes ago" --no-pager | grep "..."

# Follow live
journalctl -u hl5170dn-printer-app -o cat -f
```

GS error logs:
- Normal path: `/tmp/hl5170dn-gs.log`
- APT path: `/tmp/hl5170dn-gs-apt.log`

## Source layout

```
src/main.c      — PAPPL system setup, mainloop
src/driver.c    — driver callbacks: pdf_filter_cb, apt_render, rwriteline_cb, pjl helpers
src/pjl.c/.h    — PJL command generation
src/packbits.c  — PackBits RLE encoder for PCL raster
Makefile        — build; `make` produces hl5170dn-printer-app
```

## Key log markers

| Log fragment | Meaning |
|---|---|
| `start job: NNNdpi duplex=X paper=Y` | Job started, PJL sent |
| `pdf_filter: ok — N page(s)` | GS completed normally |
| `pdf_filter: cancelled after N complete page(s)` | Job cancelled mid-render |
| `gs cmd: gs … -rNNN …` | GS invoked at NNN dpi |
| `pdf_filter: using APT Mode 1024` | APT triggered (print-quality=5) |
| `apt_render: ok` | APT rendering completed |
| `apt_render: cancelled after N complete page(s)` | APT job cancelled mid-render |
| `substituted LETTER for A4` | Media substitution fired |
| `rejecting job: loaded paper is …` | Reject mode fired |
| `status: CODE=10001 ONLINE=TRUE` | Printer ready |
