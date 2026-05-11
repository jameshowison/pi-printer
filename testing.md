# HL-5170DN Printer App — Test Plan

Covers Phase 2 exit criteria (§2.3), Phase 3 exit criteria, Phase 4 observability,
Phase 5 supply level, and Phase 6A APT tests T1–T4, T6–T7 (T5 passed offline
2026-05-09).

All `lp` commands use `-h localhost:8000` to talk directly to PAPPL's IPP server
without requiring a CUPS daemon. Run them from the Pi. `journalctl` commands show
the last 200 log lines; run them immediately after the job completes.

---

## Setup

Confirm `text-test.pdf` is the multi-page file used for duplex and APT multi-page
tests. No generated file needed.

```
pdfinfo /home/tuttle/pi-printer/text-test.pdf | grep Pages
```

Expected output: `Pages:          5`

---

## Preflight

### PRE-1 — Service is running

```
systemctl is-active hl5170dn-printer-app
```

Expected output: `active`

### PRE-2 — Printer is reachable via IPP

```
lpstat -h localhost:8000 -p hl5170dn
```

Expected output: line containing `hl5170dn` and `idle` or `ready`. If this command
fails, verify `systemctl status hl5170dn-printer-app` and that port 8000 is
listening (`ss -tlnp | grep 8000`).

### PRE-3 — Web UI is reachable

Open `http://tuttle-pi.local:8000/` in a browser. Pass: printer admin page loads,
printer listed as idle.

---

## Phase 2 — Output quality and robustness

### P2-T1a — Text at 300 dpi

```
lp -h localhost:8000 -d hl5170dn -o printer-resolution=300dpi /home/tuttle/pi-printer/text-test.pdf
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "start job:|pdf_filter: ok"
```

Expected log: `start job: 300dpi duplex=OFF paper=LETTER` … `pdf_filter: ok — 1 page(s)`

Physical pass: page prints, text readable. Keep for side-by-side comparison with T1b.

### P2-T1b — Text at 600 dpi

```
lp -h localhost:8000 -d hl5170dn -o printer-resolution=600dpi /home/tuttle/pi-printer/text-test.pdf
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "start job:|gs cmd:"
```

Expected log: `start job: 600dpi` and `gs cmd: gs … -r600 …`

Physical pass: sharper than T1a. Keep for baseline comparison.

### P2-T1c — Image at 300 dpi

```
lp -h localhost:8000 -d hl5170dn -o printer-resolution=300dpi /home/tuttle/pi-printer/image-test.pdf
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "start job:|pdf_filter: ok"
```

Expected log: `start job: 300dpi` … `pdf_filter: ok — 1 page(s)`

Physical pass: image prints. Keep for comparison with T1d and T1c-baseline output.

### P2-T1d — Image at 600 dpi

```
lp -h localhost:8000 -d hl5170dn -o printer-resolution=600dpi /home/tuttle/pi-printer/image-test.pdf
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "start job:|gs cmd:"
```

Expected log: `start job: 600dpi` and `gs cmd: gs … -r600 …`

Physical pass: visibly better halftone than T1c. Keep for APT comparison (P6A-T1).

### P2-T2 — AirPrint photo from iPhone (manual)

On the iPhone: open a photo in Photos → Share → Print → select "hl5170dn" (or
"Brother HL-5170DN") from AirPrint printers → Print.

Log check immediately after job completes:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "pdf_filter:|start job:|end job"
```

Expected log:
- `pdf_filter: '<filename>' via gs pgmraw (streaming)` — confirms the PDF filter
  handled the job, not PAPPL's internal BLACK_1 path
- `start job: … paper=LETTER …`
- `pdf_filter: ok — 1 page(s)`
- `end job (elapsed …s)` — elapsed time should be reasonable (< 60 s at 300 dpi)

Physical pass: photo prints, no stall, printer not left in a waiting state.

### P2-T3a — Multi-page duplex, long-edge binding

```
lp -h localhost:8000 -d hl5170dn -o sides=two-sided-long-edge /home/tuttle/pi-printer/text-test.pdf
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "start job:|start page|end job"
```

Expected log: `start job: … duplex=LONGEDGE …`, five `start page` entries, `end job`

Physical pass: 5 pages on 3 sheets (sheet 3 front only, back blank — correct for an
odd page count), long-edge (book-style) orientation.

### P2-T3b — Multi-page duplex, short-edge binding

```
lp -h localhost:8000 -d hl5170dn -o sides=two-sided-short-edge /home/tuttle/pi-printer/text-test.pdf
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep "start job:"
```

Expected log: `start job: … duplex=SHORTEDGE …`

Physical pass: 5 pages on 3 sheets, short-edge (calendar-style flip) orientation.

### P2-T4 — Cancel mid-print job

Submit a slow job (image at 600 dpi takes ~45 s on Pi 3B+):

```
lp -h localhost:8000 -d hl5170dn -o printer-resolution=600dpi /home/tuttle/pi-printer/image-test.pdf
```

While the printer is actively printing (LED active, paper moving), cancel all jobs:

```
cancel -h localhost:8000 -a
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "pdf_filter:|gs exited|end job"
```

Expected log: either `pdf_filter: FAILED — 0 page(s)` or `pdf_filter: gs exited
with status` (non-zero), followed by `end job`.

Physical pass: printer is not stuck (no blinking error LEDs). Submit and complete a
simple follow-up job to confirm:

```
lp -h localhost:8000 -d hl5170dn /home/tuttle/pi-printer/text-test.pdf
```

Pass: follow-up job prints cleanly.

---

## Phase 3 — Media substitution

The printer has Letter loaded. Default `loaded-paper` is `na_letter_8.5x11in`.

### P3-T1 — A4 job substituted to Letter (substitute mode)

```
lp -h localhost:8000 -d hl5170dn -o media=iso_a4_210x297mm /home/tuttle/pi-printer/text-test.pdf
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "substituted|gs cmd:|start job:"
```

Expected log:
- `substituted LETTER for A4 (loaded paper)` — substitution fired
- `gs cmd: gs … -sPAPERSIZE=letter …` — GS used letter, not a4
- `start job: … paper=LETTER …` — PJL used LETTER

Physical pass: page prints on Letter, A4 content scaled to fit (no clipping at
edges). The content area is smaller than a full Letter print because A4 is narrower
and taller — this is correct fitpage scaling behaviour.

### P3-T2 — Envelope not coerced

```
lp -h localhost:8000 -d hl5170dn -o media=iso_dl_110x220mm /home/tuttle/pi-printer/text-test.pdf
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "substituted|start job:"
```

Expected log: `start job: … paper=DL …` and NO `substituted` line. The DL envelope
must pass through unchanged even though Letter is loaded.

Physical pass: job sent with DL paper; printer may pause for a media mismatch if no
envelope is loaded — that is correct behaviour (the app is not incorrectly
substituting Letter for the envelope). Cancel from the web UI if the printer waits.

### P3-T3 — Reject mode: A4 job rejected before printing

```
lp -h localhost:8000 -d hl5170dn -o media=iso_a4_210x297mm -o media-mismatch-action=reject /home/tuttle/pi-printer/text-test.pdf
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "rejecting job|pdf_filter:|start job:"
```

Expected log: `rejecting job: loaded paper is LETTER, requested A4`. The job must
appear in the log but NO `start job:` or `pdf_filter:` line should follow it —
the job was aborted before the PJL header was sent.

Physical pass: nothing prints. The printer remains idle.

---

## Phase 4 — Observability

### P4-T1 — Control characters in job name are sanitised

Submit a job whose name contains a newline and a bell character:

```
lp -h localhost:8000 -d hl5170dn -t "$(printf 'test\007bell\nand\ttab')" /home/tuttle/pi-printer/text-test.pdf
```

Log check — confirm no raw control characters appear in the start job line:

```
journalctl -u hl5170dn-printer-app -n 100 --no-pager | grep "start job:" | grep -cP "[\x01-\x08\x0b-\x1f\x7f]"
```

Expected output: `0` (zero lines with control characters).

Also confirm the start job line is human-readable:

```
journalctl -u hl5170dn-printer-app -n 100 --no-pager | grep "start job:"
```

Expected: the job context prefix in the line shows something like `test bell and
tab from <user>` — spaces replacing the control characters, no garbled output.

---

## Phase 5 — Supply level

### P5-T1 — Supply level visible in web UI

Open `http://tuttle-pi.local:8000/` in a browser → click the printer name →
look for the toner / supply section.

Expected: a supply entry named "Black Toner Cartridge" is shown. Level may display
as "Unknown" (`marker-levels=-2`) — that is the correct fallback per Phase 0
findings (`INFO SUPPLIES` returns `"?"` on this printer). The page must not crash.

Log check (confirm status polling ran without error):

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep "status:"
```

Expected log: `status: CODE=10001 ONLINE=TRUE` (ready) or `CODE=40000` (sleep).
No `papplDeviceRead` error should appear (a "device busy" debug line is acceptable).

---

## Phase 6A — APT photo path (Mode 1024)

T5 passed offline 2026-05-09 (TIFF header verified with `tiffinfo`). These are the
physical printer tests.

### P6A-T1 — APT path taken, image input

```
lp -h localhost:8000 -d hl5170dn -o print-quality=5 /home/tuttle/pi-printer/image-test.pdf
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "APT Mode|apt_render:"
```

Expected log:
- `pdf_filter: using APT Mode 1024 (150 dpi input)` — APT branch taken
- `apt_render: gs cmd: gs … -r150 …` — GS at 150 dpi
- `apt_render: page 0: 1275x1650 px, TIFF=2278674 B` — Letter at 150 dpi
- `apt_render: ok — 1 page(s)` — completed without error

Physical pass: image prints. Compare to T1d (600 dpi ordered dither). Note which
looks better — this is the APT decision gate. If APT output is visually superior,
note it. If inferior or blank, record that here and see the decision gate below.

### P6A-T2 — APT path taken, text input

```
lp -h localhost:8000 -d hl5170dn -o print-quality=5 /home/tuttle/pi-printer/text-test.pdf
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "APT Mode|apt_render: ok"
```

Expected log: `using APT Mode 1024` … `apt_render: ok — 1 page(s)`

Physical pass: text prints. At 150 dpi input, text is expected to look noticeably
softer than T1b (600 dpi dither). Record the quality difference — this informs
whether APT should be limited to photo-only jobs in future.

### P6A-T3 — APT NOT taken at normal quality

```
lp -h localhost:8000 -d hl5170dn -o print-quality=4 /home/tuttle/pi-printer/image-test.pdf
```

Log check — confirm APT was not used:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -c "using APT Mode"
```

Expected output: `0`

Also confirm the normal 600 dpi dither path ran:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "gs cmd:|pdf_filter: ok"
```

Expected log: `gs cmd: gs … -r600 …` (not -r150) and `pdf_filter: ok — 1 page(s)`

Physical pass: same output as T1d (600 dpi dither), no APT.

### P6A-T4 — APT multi-page

```
lp -h localhost:8000 -d hl5170dn -o print-quality=5 /home/tuttle/pi-printer/text-test.pdf
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "apt_render: page|apt_render: ok"
```

Expected log: five `apt_render: page N:` lines (pages 0–4) followed by `apt_render:
ok — 5 page(s)`.

Physical pass: all 4 pages print, each with printer-halftoned output.

### P6A-T6 — APT with duplex

```
lp -h localhost:8000 -d hl5170dn -o print-quality=5 -o sides=two-sided-long-edge /home/tuttle/pi-printer/text-test.pdf
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "start job:|apt_render: ok"
```

Expected log: `start job: 600dpi duplex=LONGEDGE paper=LETTER …` and `apt_render: ok — 5 page(s)`

Physical pass: 5 pages on 3 sheets, both sides used (sheet 3 front only), APT halftoning visible.

### P6A-T7 — Cancel mid APT job

APT at 150 dpi is faster than 600 dpi dither, but a 4-page job still takes several
seconds. Submit and cancel during rendering:

```
lp -h localhost:8000 -d hl5170dn -o print-quality=5 /home/tuttle/pi-printer/text-test.pdf
```

While the printer LED is active, cancel all jobs:

```
cancel -h localhost:8000 -a
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 200 --no-pager | grep -E "apt_render:|gs exited"
```

Expected log: `apt_render: FAILED` or `apt_render: gs exited with status` (non-zero),
followed by `end job`.

Physical pass: printer is not stuck. Confirm with a clean follow-up job:

```
lp -h localhost:8000 -d hl5170dn /home/tuttle/pi-printer/text-test.pdf
```

Pass: follow-up job prints cleanly.

---

## Phase 6A decision gate

After T1 and T2:

| Outcome | Action |
|---------|--------|
| APT image output visually superior to 600 dpi dither (T1 vs T1d) | APT is the right default for `print-quality=5`. Document in plan §6A. |
| Text at APT (T2) looks unacceptably soft | Restrict APT to photo-only jobs in a future phase. Note in plan. |
| Printer prints blank or garbled page at T1 | Disable APT. Stay on 600 dpi dither. Record attempt in plan §6A. Check GS error log: `cat /tmp/hl5170dn-gs-apt.log` |
| Printer prints blank or garbled at T1 but GS log is clean | PCL framing or PJL issue. Compare byte stream against `cups-filter-baseline` tag (`git diff 4602cfc`). |

---

## GS error log

If any job fails unexpectedly, GS stderr is captured to:

- Normal path: `/tmp/hl5170dn-gs.log`
- APT path: `/tmp/hl5170dn-gs-apt.log`

```
cat /tmp/hl5170dn-gs.log
```

```
cat /tmp/hl5170dn-gs-apt.log
```

---

## Phase completion checklist

| Test | Result | Notes |
|------|--------|-------|
| PRE-1 service active | | |
| PRE-2 lpstat reachable | | |
| PRE-3 web UI loads | | |
| P2-T1a text 300 dpi | | |
| P2-T1b text 600 dpi | | |
| P2-T1c image 300 dpi | | |
| P2-T1d image 600 dpi | | |
| P2-T2 iPhone AirPrint | | |
| P2-T3a duplex long-edge | | |
| P2-T3b duplex short-edge | | |
| P2-T4 cancel mid-job | | |
| P3-T1 A4→Letter substitute | | |
| P3-T2 envelope not coerced | | |
| P3-T3 reject mode | | |
| P4-T1 control chars sanitised | | |
| P5-T1 supply level in web UI | | |
| P6A-T1 APT image | | |
| P6A-T2 APT text | | |
| P6A-T3 APT not taken at quality=4 | | |
| P6A-T4 APT multi-page | | |
| P6A-T6 APT + duplex | | |
| P6A-T7 APT cancel | | |
