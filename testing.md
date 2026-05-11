# HL-5170DN Printer App — Test Plan

Covers Phase 2 exit criteria (§2.3), Phase 3 exit criteria, Phase 4 observability,
Phase 5 supply level, and Phase 6A APT tests T1–T4, T6–T7 (T5 passed offline
2026-05-09).

**Job submission protocol:** all jobs are sent directly to PAPPL via `ipptool`
at `ipp://localhost:8000/ipp/print`. Do NOT use `lp`, `lpr`, or CUPS — CUPS
rasterizes PDFs to URF before forwarding, which bypasses the PDF/GS path and
prevents APT from triggering. Do NOT use `hl5170dn-printer-app submit` — it is
a standalone process that does not route through the running service. The test
files live in `/home/tuttle/pi-printer/tests/`.

Standard submission form:

```
ipptool -tv \
  -f /path/to/input.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/TESTFILE.test
```

Cancellation (not job submission) still uses the PAPPL CLI:

```
hl5170dn-printer-app cancel -d hl5170dn
```

Default throughout: **2-page input, duplex long-edge** (1 sheet, both sides).
Exceptions are noted per test.

---

## Setup

Create the 2-page test PDF used in most tests (pages 1–2 of text-test.pdf):

```
gs -q -dBATCH -dNOPAUSE -sDEVICE=pdfwrite -dFirstPage=1 -dLastPage=2 \
  -sOutputFile=/tmp/2page-test.pdf /home/tuttle/pi-printer/text-test.pdf
```

Confirm:

```
pdfinfo /tmp/2page-test.pdf | grep Pages
```

Expected output: `Pages:          2`

---

## Preflight

### PRE-1 — Service is running

```
systemctl is-active hl5170dn-printer-app
```

Expected output: `active`

### PRE-2 — Printer is reachable via IPP

```
ipptool -tv ipp://localhost:8000/ipp/print \
  /usr/share/cups/ipptool/get-printer-attributes.test
```

Expected: request succeeds (status `successful-ok`), attributes returned include
`printer-state`. If this fails, verify `systemctl status hl5170dn-printer-app`
and that port 8000 is listening (`ss -tlnp | grep 8000`).

### PRE-3 — Web UI is reachable

Open `http://tuttle-pi.local:8000/` in a browser. Pass: printer admin page loads,
printer listed as idle.

---

## Phase 2 — Output quality and robustness

### P2-T1a — Text at 300 dpi

```
ipptool -tv \
  -f /tmp/2page-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-duplex-long-300.test
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "start job:|pdf_filter: ok"
```

Expected log: `start job: 300dpi duplex=LONGEDGE paper=LETTER` … `pdf_filter: ok — 2 page(s)`

Physical pass: both sides of one sheet printed, text readable. Keep for comparison with T1b.

### P2-T1b — Text at 600 dpi

```
ipptool -tv \
  -f /tmp/2page-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-duplex-long-600.test
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "start job:|gs cmd:"
```

Expected log: `start job: 600dpi duplex=LONGEDGE` and `gs cmd: gs … -r600 …`

Physical pass: sharper than T1a. Keep for baseline comparison.

### P2-T1c — Image at 300 dpi

```
ipptool -tv \
  -f /home/tuttle/pi-printer/image-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-duplex-long-300.test
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "start job:|pdf_filter: ok"
```

Expected log: `start job: 300dpi duplex=LONGEDGE` … `pdf_filter: ok`

Physical pass: image prints. Keep for comparison with T1d.

### P2-T1d — Image at 600 dpi

```
ipptool -tv \
  -f /home/tuttle/pi-printer/image-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-duplex-long-600.test
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "start job:|gs cmd:"
```

Expected log: `start job: 600dpi duplex=LONGEDGE` and `gs cmd: gs … -r600 …`

Physical pass: visibly better halftone than T1c. Keep for APT comparison (P6A-T1).

### P2-T2 — AirPrint photo from iPhone (manual)

On the iPhone: open a photo in Photos → Share → Print → select "hl5170dn" (or
"Brother HL-5170DN") from AirPrint printers → Print.

Log check immediately after job completes:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "pdf_filter:|start job:|end job"
```

Expected log:
- `pdf_filter: '<filename>' via gs pgmraw (streaming)` — PDF filter used, not PAPPL's BLACK_1 path
- `start job: … paper=LETTER …`
- `pdf_filter: ok — 1 page(s)`
- `end job (elapsed …s)` — should complete without stalling

Physical pass: photo prints, no stall, printer not left in a waiting state.

### P2-T3a — Duplex long-edge

```
ipptool -tv \
  -f /tmp/2page-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-duplex-long-600.test
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "start job:|start page"
```

Expected log: `start job: … duplex=LONGEDGE …`, two `start page` entries.

Physical pass: both sides of one sheet printed, long-edge (book-style) orientation.

### P2-T3b — Duplex short-edge

```
ipptool -tv \
  -f /tmp/2page-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-duplex-short.test
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep "start job:"
```

Expected log: `start job: … duplex=SHORTEDGE …`

Physical pass: both sides of one sheet, short-edge (calendar-style flip) orientation.

### P2-T4 — Cancel mid-print job

Image at 600 dpi takes ~45 s on the Pi 3B+, giving time to cancel.

```
ipptool -tv \
  -f /home/tuttle/pi-printer/image-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-duplex-long-600.test
```

While the printer is actively printing (LED active, paper moving), cancel all jobs:

```
hl5170dn-printer-app cancel -d hl5170dn
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "pdf_filter:|gs exited|end job"
```

Expected log: `pdf_filter: FAILED` or `pdf_filter: gs exited with status` (non-zero), then `end job`.

Physical pass: printer not stuck. Confirm with a clean follow-up job:

```
ipptool -tv \
  -f /tmp/2page-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-duplex-long-600.test
```

Pass: follow-up job prints cleanly.

---

## Phase 3 — Media substitution

The printer has Letter loaded. Default `loaded-paper` is `na_letter_8.5x11in`.

### P3-T1 — A4 job substituted to Letter (substitute mode)

```
ipptool -tv \
  -f /tmp/2page-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-a4-duplex-long.test
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "substituted|gs cmd:|start job:"
```

Expected log:
- `substituted LETTER for A4 (loaded paper)` — substitution fired
- `gs cmd: gs … -sPAPERSIZE=letter …` — GS used letter, not a4
- `start job: … paper=LETTER …` — PJL used LETTER

Physical pass: both pages print on Letter, A4 content scaled to fit (no clipping).
A4 is narrower and taller than Letter — fitpage scaling means small margins on the
sides; this is correct.

### P3-T2 — Envelope not coerced

Envelopes must not be substituted. Use single-sided (no duplex on envelopes).

```
ipptool -tv \
  -f /tmp/2page-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-envelope-simplex.test
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "substituted|start job:"
```

Expected log: `start job: … paper=DL …` and NO `substituted` line.

Physical pass: job sent with DL paper. Printer may pause for a media mismatch if no
envelope is loaded — that is correct (the app must not silently substitute Letter for
the envelope). Cancel from the web UI if the printer waits.

### P3-T3 — Reject mode

```
ipptool -tv \
  -f /tmp/2page-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-a4-reject.test
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "rejecting job|pdf_filter:|start job:"
```

Expected log: `rejecting job: loaded paper is LETTER, requested A4`. No `start job:`
or `pdf_filter:` line should follow — the job was aborted before any PJL was sent.

Physical pass: nothing prints.

---

## Phase 4 — Observability

### P4-T1 — Control characters in job name are sanitised

```
ipptool -tv \
  -f /tmp/2page-test.pdf \
  -d filetype=application/pdf \
  -d "jobname=$(printf 'test\007bell\nand\ttab')" \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-jobname.test
```

Log check — confirm no raw control characters in the start job line:

```
journalctl -u hl5170dn-printer-app -n 100 --no-pager | grep "start job:" | grep -cP "[\x01-\x08\x0b-\x1f\x7f]"
```

Expected output: `0`

Confirm the line is human-readable:

```
journalctl -u hl5170dn-printer-app -n 100 --no-pager | grep "start job:"
```

Expected: job context prefix shows something like `test bell and tab from <user>` —
spaces replacing control characters, no garbled output.

---

## Phase 5 — Supply level

### P5-T1 — Supply level visible in web UI

Open `http://tuttle-pi.local:8000/` in a browser → click the printer name →
look for the toner / supply section.

Expected: "Black Toner Cartridge" shown. Level may display as "Unknown"
(`marker-levels=-2`) — correct fallback per Phase 0 (`INFO SUPPLIES` returns `"?"`
on this printer). Page must not crash.

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep "status:"
```

Expected log: `status: CODE=10001 ONLINE=TRUE` (ready) or `CODE=40000` (sleep).
No `papplDeviceRead` error (a "device busy" debug line is acceptable).

---

## Phase 6A — APT photo path (Mode 1024)

T5 passed offline 2026-05-09. These are the physical printer tests.

### P6A-T1 — APT path taken, image input

```
ipptool -tv \
  -f /home/tuttle/pi-printer/image-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-apt.test
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "APT Mode|apt_render:"
```

Expected log:
- `pdf_filter: using APT Mode 1024 (150 dpi input)`
- `apt_render: gs cmd: gs … -r150 …`
- `apt_render: page 0: 1275x1650 px, TIFF=2278674 B` (Letter at 150 dpi)
- `apt_render: ok`

Physical pass: image prints. Compare to T1d (600 dpi ordered dither) — this is the
APT decision gate. Record which looks better.

### P6A-T2 — APT path taken, text input

```
ipptool -tv \
  -f /tmp/2page-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-apt.test
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "APT Mode|apt_render: ok"
```

Expected log: `using APT Mode 1024` … `apt_render: ok — 2 page(s)`

Physical pass: both pages print. At 150 dpi, text is expected to look noticeably
softer than T1b (600 dpi dither). Record quality difference — informs whether APT
should be restricted to photo-only jobs.

### P6A-T3 — APT NOT taken at normal quality

```
ipptool -tv \
  -f /home/tuttle/pi-printer/image-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-quality4-duplex-long-600.test
```

Log check — confirm APT was not used:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -c "using APT Mode"
```

Expected output: `0`

Confirm normal 600 dpi path:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "gs cmd:|pdf_filter: ok"
```

Expected log: `gs cmd: gs … -r600 …` (not -r150) and `pdf_filter: ok`

Physical pass: same output as T1d.

### P6A-T4 — APT multi-page

```
ipptool -tv \
  -f /tmp/2page-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-apt.test
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "apt_render: page|apt_render: ok"
```

Expected log: two `apt_render: page N:` lines (pages 0–1) then `apt_render: ok — 2 page(s)`.

Physical pass: both pages print with printer-halftoned output.

### P6A-T6 — APT with duplex

```
ipptool -tv \
  -f /tmp/2page-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-apt.test
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "start job:|apt_render: ok"
```

Expected log: `start job: 600dpi duplex=LONGEDGE paper=LETTER` and `apt_render: ok — 2 page(s)`

Physical pass: both sides of one sheet printed with APT halftoning.

### P6A-T7 — Cancel mid APT job

APT at 150 dpi is fast. Use image-test.pdf to ensure enough rendering time to cancel.

```
ipptool -tv \
  -f /home/tuttle/pi-printer/image-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-apt.test
```

While the printer LED is active, cancel:

```
hl5170dn-printer-app cancel -d hl5170dn
```

Log check:

```
journalctl -u hl5170dn-printer-app -n 500 --no-pager | grep -E "apt_render:|gs exited"
```

Expected log: `apt_render: FAILED` or `gs exited with status` (non-zero), then `end job`.

Physical pass: printer not stuck. Confirm with a clean follow-up:

```
ipptool -tv \
  -f /tmp/2page-test.pdf \
  -d filetype=application/pdf \
  ipp://localhost:8000/ipp/print \
  /home/tuttle/pi-printer/tests/print-duplex-long-600.test
```

Pass: follow-up job prints cleanly.

---

## Phase 6A decision gate

After T1 and T2:

| Outcome | Action |
|---------|--------|
| APT image output visually superior to 600 dpi dither (T1 vs T1d) | APT is the right default for `print-quality=5`. Document in plan §6A. |
| Text at APT (T2) looks unacceptably soft | Restrict APT to photo-only jobs in a future phase. Note in plan. |
| Printer prints blank or garbled at T1 | Disable APT. Stay on 600 dpi dither. Check GS error log: `cat /tmp/hl5170dn-gs-apt.log` |
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
| PRE-2 IPP reachable | | |
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
