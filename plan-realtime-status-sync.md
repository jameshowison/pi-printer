# Plan: Real-time printer status sync

## Context

Commit d3107ee moved PJL polling to job-boundary only to stop waking the printer's fuser
on every macOS Get-Printer-Attributes. That fixed the wakeup problem but introduced a
3-minute lag before paper-out or stall conditions reach macOS or the web UI. Subsequent
work (USTATUS DEVICE=VERBOSE + TIMED=5) gave us mid-job status arriving every page
boundary, but exposed deeper structural sync problems:

1. **GS races ahead of the printer.** Raster data pipes into the printer's input buffer
   as fast as Ghostscript can render, so cancellation and stall detection are 3 minutes
   late.
2. **No physical-page tracking.** We count pages we *sent*, never pages that physically
   came out.
3. **macOS shows "completed" when nothing printed.** `pdf_filter_cb` returns true if any
   page started; PAPPL marks the job complete; user thinks job succeeded.
4. **Idle status goes stale.** We send `@PJL USTATUSOFF` at EOJ; USB device closes
   between jobs; no more push.
5. **Supplies page is static** — no auto-refresh; user must hit reload.

## Research summary

- **Manual §7.6.3**: `@PJL USTATUS PAGE = ON` makes the printer push `@PJL USTATUS PAGE\r\n<N>\r\n\f`
  on *physical* page completion (used for job recovery, per the manual).
- **Manual §534**: USTATUS settings persist across EOJ; only the page counter resets.
- **PAPPL** rate-limits `status_cb` to 1/sec and skips it during jobs (`printer-ipp.c:1636`).
- **`papplDeviceRead`** uses a 10-second libusb timeout (`device-usb.c:734`) — explains the
  partial-response problem seen in job 8 (39 bytes, CODE=-1).
- **macOS Print Center** renders `media-empty` → "Out of paper", `cover-open` → "Cover
  open", `offline` → "Printer offline", `IPP_PSTATE_STOPPED` → "Paused".
- **PAPPL web UI** supports `<meta http-equiv="refresh">` via `papplClientHTMLPrinterHeader(..., refresh, ...)`.
- **Other PAPPL drivers** (hp-printer-app, lprint) use SNMP for status — no precedent
  for USB+PJL real-time. We're inventing the pattern.
- **DISPLAY field**: The HL-5170DN has no LCD, but the firmware still populates DISPLAY
  with canonical status strings like `"00 IDLE 001P LT"`, `"12 COVER OPEN"`,
  `"PRINTING"`. The printer is the authoritative source of its own state in plain text.

## Plan — phased

### Phase 1: Supplies page auto-refresh

`hl5170dn_web_supplies` already calls PAPPL's web helpers. Pass `refresh=5` so the
page emits `<meta http-equiv="refresh" content="5">`. Zero JS, standard idiom.

### Phase 7 (revised): Show DISPLAY prominently, mark staleness

DISPLAY *is* the printer's self-reported state in plain text. Surface it:

- Display `Printer reports: "<DISPLAY value>"` near the top of the status panel.
- When `pjl_online == 0`, render header red and override LED status to red-blink (done).
- Show "Last polled: N seconds ago" — when fresh (< 30s), green dot; otherwise grey.
- Keep the Code number and LED diagram, but when offline + code disagrees with
  online state, render the code in muted color (it's stale).

### Phase 2: Track physical pages via USTATUS PAGE

- `pjl.c`: add `@PJL USTATUS PAGE = ON\r\n` to the job header alongside DEVICE/TIMED.
- `driver.c`: extend response handling to detect `@PJL USTATUS PAGE\r\n<N>` packets
  (distinct from DEVICE/TIMED). Increment `jd->pages_printed`.
- Call `papplJobSetImpressionsCompleted(job, jd->pages_printed)` after each PAGE event
  → macOS shows real "X of N" based on physical output.

### Phase 5 (depends on Phase 2): Truthful IPP job state

- When a blocking USTATUS DEVICE code arrives mid-job (40021, 40023, 41xxx),
  call `papplPrinterSetState(printer, IPP_PSTATE_STOPPED)` → macOS shows "Paused".
- At end of `pdf_filter_cb`: if `pages_printed < pages_sent`, set
  `papplJobSetState(job, IPP_JSTATE_PROCESSING_STOPPED)` with `media-empty` reason
  so PAPPL doesn't mark the job completed when it wasn't.

### Phase 3 (depends on Phase 2): GS throttling

Add backpressure so GS pace tracks printer pace. After sending page N to printer, before
reading page N+1 from GS pipe, block on a short-timeout USB IN read until
`pages_sent - pages_printed < 2`. Requires a non-`papplDeviceRead` path (10s timeout
too long); use libusb directly via `papplDeviceGetData` if accessible, or implement
short-timeout wrapper.

### Phase 4 (depends on Phase 3): Cancel propagation

- Replace `popen()` with `pipe()`/`fork()`/`execvp()` so we have the GS PID.
- On `papplJobIsCanceled(job)` in the streaming loop, SIGTERM GS, waitpid.
- PCL data already in the printer's buffer can't be aborted (manual confirms UEL only
  flushes PJL), but we stop adding to it.

### Phase 6: Idle status without waking the engine

Two-tier:
- **Tier 2** (timed refresh, ~60s): `status_cb` does cache replay normally; if last
  real poll was >60s ago, do a single PJL INFO STATUS over a freshly opened device,
  then close. Bounded ≤ 60 wakeups/hour vs the pre-d3107ee rate.
- **Tier 3** (experimental): drop USTATUSOFF from trailer, leave the USB handle open
  between jobs, drain USB IN in status_cb. Only if testing shows read-only USB doesn't
  wake the fuser.

## Recommended order

1. Phase 1 (auto-refresh) — ship now
2. Phase 7 (DISPLAY prominence + freshness) — ship now
3. Phase 2 (USTATUS PAGE → impressions-completed)
4. Phase 5 (IPP job state truth)
5. Phase 3 (GS throttle)
6. Phase 4 (cancel propagation)
7. Phase 6 Tier 2 (timed idle refresh)
8. Phase 6 Tier 3 (event-driven idle — gated on USB-wake measurement)

## Files touched

- `src/pjl.c` — Phase 2 (USTATUS PAGE in header)
- `src/driver.c`:
  - `hl5170dn_web_supplies` — Phase 1 (refresh)
  - `render_status_panel` — Phase 7
  - `apply_pjl_status_response` + new PAGE parser — Phase 2
  - `hl5170dn_job_t` — Phase 2 (`pages_printed` field)
  - `pdf_filter_cb` — Phase 2 USTATUS PAGE counting + later phases
  - `hl5170dn_status` — Phase 6 (later)
