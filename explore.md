# Exploration notes

Running discoveries about the system that are useful for debugging and test design.

---

## Job submission routing (2026-05-10)

### How jobs reach the PAPPL service

`hl5170dn-printer-app submit` is a standalone PAPPL process, not an IPP client
to the running service. It logs to its own stderr (terminal), not journalctl.

`lp -h localhost:8000 -d hl5170dn` fails with "printer-uri not found" because
CUPS constructs the destination URI as `/printers/hl5170dn` but PAPPL serves at
`/ipp/print`.

**Working approaches:**

1. **CUPS as IPP proxy (preferred for manual testing)**

   Register PAPPL as a CUPS destination once:
   ```
   sudo lpadmin -p hl5170dn -v ipp://localhost:8000/ipp/print -E -m everywhere
   ```
   Then use `lp -d hl5170dn [-o option=value] file`. CUPS forwards to PAPPL;
   all job execution logs appear in journalctl.

2. **ipptool (direct IPP, no CUPS)**

   ```
   ipptool -tv -f file.pdf -d filetype=application/pdf \
     ipp://localhost:8000/ipp/print /usr/share/cups/ipptool/print-job.test
   ```
   Sends the job directly to PAPPL. Useful for PDF path (bypasses CUPS
   rasterization). Confirmed working 2026-05-10.

### CUPS converts PDF → URF before forwarding

When a PDF is submitted via `lp -d hl5170dn`, CUPS (IPP Everywhere / `-m
everywhere` mode) rasterizes the PDF to `image/urf` before forwarding to PAPPL.
PAPPL receives URF and processes it via the raster path (`rwriteline_cb`), NOT
via `pdf_filter_cb`. As a result:

- `pdf_filter:` and `gs cmd:` log lines do NOT appear for `lp`-submitted jobs.
- `start job:` and `end job` lines DO appear (raster path goes through
  `rstartjob_cb`).
- CUPS renders at 600 dpi regardless of `printer-resolution` option.
- `print-quality=5` (APT) is NOT triggered via `lp` because APT is initiated in
  `pdf_filter_cb`, not in `rwriteline_cb`.

For tests that need the PDF/GS path (APT, resolution-specific GS rendering), use
`ipptool` to submit the PDF directly.

### journalctl window

`journalctl -n 200` is often too small when the web UI has been recently browsed
(each page load generates ~15–20 log lines). Use `-n 500` or
`--since "5 minutes ago"`.

### Cancellation

CUPS-submitted jobs: `cancel hl5170dn` or `cancel -a hl5170dn`  
ipptool/PAPPL-submitted jobs: `hl5170dn-printer-app cancel -d hl5170dn`

### Confirmed working (2026-05-10)

```
lp -d hl5170dn -o sides=two-sided-long-edge /tmp/2page-test.pdf
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep "start job:"
# → start job: 600dpi duplex=LONGEDGE paper=LETTER source=TRAY1 type=REGULAR econo=OFF copies=1
# → Completed, job-impressions-completed=2
```
