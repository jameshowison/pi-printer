# Brother HL-5170DN — Native PAPPL Driver: Plan

## What this plan covers

This plan operationalises
[`PRD-printer-applicance-rewrite-hl5170dn-pappl-driver.md`](PRD-printer-applicance-rewrite-hl5170dn-pappl-driver.md):
the rewrite of the existing CUPS + shell-script-filter + Ghostscript `ljet4`
pipeline into a single PAPPL-based printer-app binary
(`hl5170dn-printer-app`) running as a systemd service. The PRD is the
spec; this file is the sequence of work and the decisions we expect to
hit along the way.

The previous plan (covering the CUPS-filter implementation that's
shipping today) is preserved in git at tag **`cups-filter-baseline`**
(`4602cfc`). Diff against that tag if you need to recover any decision
or operational note from the prior architecture.

## Why a rewrite

The CUPS-filter baseline works for the 95% case (Mac/text PDFs) but has
three structural problems the filter architecture can't fix from inside:

1. **AirPrint photo prints from iOS stall.** Job 10 sat for 1.5 h with
   the CUPS USB backend looping `Waiting for printer to become available`
   while the filter blocked on a full pipe. Diagnosed but not fixed in
   the baseline; root cause is the filter/backend split itself.
2. **A4-from-iPhone tray-mismatch pauses.** The PRD's media-substitution
   feature has to live above the filter layer (at the IPP attribute
   resolution step) to work, and CUPS doesn't give a filter that hook.
3. **No web admin UI, no live log viewer, no per-printer log level.**
   PAPPL provides all of these for free; CUPS does not.

A native PAPPL driver fixes all three by collapsing the pipeline into
one process that owns the IPP attributes, the raster generation, the
USB device, and the web UI together.

## Constraints carried over from the baseline plan

- **Keep the Brother HL-5170DN.** 2003-vintage USB+Ethernet mono laser.
  Mechanically excellent, toner is cheap.
- **Pi 3B+ as the print server.** Day-zero investigation 2 confirms
  whether GS render is fast enough; if not, the Pi 5 / 300 dpi default
  / Ethernet-instead-of-USB / new-printer escape hatches from the
  baseline plan still apply.
- **USB only.** The PRD explicitly drops Ethernet; using
  `socket://...:9100` would route around the interesting hard parts
  (USB back-channel, libusb plumbing, sleep-mode quirks).

## Goals (mirroring the PRD; tracked here for status)

1. Print PDF, PWG Raster, Apple Raster, JPEG, PNG from any
   IPP-Everywhere client to the USB-attached HL-5170DN.
2. Match or beat baseline output quality at 300, 600, and HQ1200.
3. Brother options via standard IPP attributes: duplex (long/short),
   toner save, tray, media type, resolution, copies.
4. Best-effort supply level via PJL `INFO` over the USB back-channel.
5. PAPPL web UI at `http://pi.local:8000/` with live log viewer and
   per-printer log level.
6. Run as systemd. No snap. No CUPS at runtime.
7. Silent Letter-substitution for incoming A4/A5/A6/Legal/Executive,
   notified through three channels. Loaded paper configurable; strict
   `reject` mode also available.
8. Clean source build on Raspberry Pi OS 64-bit stable.

## Phases

The phasing is deliberately "skeleton first, features second, polish
third." Each phase ends in something printable so we can reality-check
against the physical printer rather than against our model of it.

### Phase 0 — Day-zero investigations (do before writing driver code)

These are the PRD's "open questions." Each one shapes the architecture
of later phases, so all four happen first. Time-box: one day total,
parallelise where possible.

**Run on the Pi**, not in the Cowork sandbox — they all need physical
access to the printer or to Pi 3B+ silicon. The runbook lives at
[`phase-0-investigations.md`](phase-0-investigations.md) with concrete
commands, expected output, and decision rules. In short:

1. **Capture the baseline filter's USB byte stream at HQ1200.** Use
   `usbmon` while a `cupsPrintQuality=High` job runs through the
   CUPS filter (now in `legacy/`). Look for `<ESC>*r1027`, mode 1024,
   mode 1152. Outcome determines Phase 6 strategy.
2. **Time `gs -sDEVICE=ljet4 -r600` on the Pi 3B+** for `text-test.pdf`
   and `image-test.pdf`. If a photo PDF takes >10 s, plan on `300dpi`
   as the default for Phase 1.
3. **Verify `papplDeviceRead()` returns back-channel bytes** from
   this printer. Probe binary in the runbook. Outcome shapes Phase 5:
   if it works, supply polling is feasible; if not, Phase 5 ships as
   "unknown" and we move on.
4. **Build PAPPL from source on Raspberry Pi OS 64-bit** if
   `libpappl-dev` from apt is too old. Pin the version.

Deliverable: findings recorded in `phase-0-investigations.md`,
summary lifted into this file under Phase 1 once they're in.

**Phase 0 complete (2026-05-07). Summary of findings:**

1. **HQ1200 byte stream** — baseline uses genuine 1200 dpi raster with
   standard PCL5e mode-2 (packbits) and mode-3 (delta row) compression;
   no Brother proprietary mode 1027/1024/1152. Phase 6 = PRD option 2:
   declare HQ1200, set `@PJL SET RESOLUTION=1200`, run GS at 1200 dpi,
   emit mode-2 packbits. No proprietary encoding work required.

2. **GS render timing** — text PDF @ 600 dpi: ~0.8 s (fine). Image PDF
   @ 600 dpi: ~45 s; @ 300 dpi: ~21 s. Both exceed the safe USB-keepalive
   window for photo input. **Phase 1 defaults to 300 dpi.** Pi 5 is the
   upgrade path for 600 dpi photo performance.

3. **papplDeviceRead() back-channel** — works. Response in ~400 ms from
   sleep. STATUS codes: `CODE=10001` = READY, `CODE=40000` = SLEEP.
   `INFO SUPPLIES` returns `"?"` — toner level unavailable. Phase 5 can
   poll ready/sleep state; `marker-levels` will be `-2` (unknown).
   USB URI: `usb://Brother/HL-5170DN%20series?serial=L4J624176`.

4. **libpappl-dev version** — apt 1.3.1 is sufficient for Phases 1–5.
   All required callbacks and device APIs present and compiling.
   Re-check for Phase 3 (vendor options / job-creation hooks).

**Open question from Phase 0 — USB keepalive during rendering.**
GS render takes 21–45 s for image input, which exceeds the printer's
USB idle-sleep threshold. PAPPL owns the USB device, but the render
runs before `rstartjob_cb` is called (or inside it via GS pipe); the
printer sees silence and may enter sleep, requiring a ~400 ms re-wake
that can stall or wedge the job. Investigate before or during Phase 1:

- **Option A — PJL ping thread**: spawn a background thread in
  `rstartjob_cb` that sends `@PJL INFO STATUS\r\n` every ~8 s over
  the USB back-channel and reads (drains) the reply, keeping the link
  active. Thread is cancelled and joined before the first PCL byte
  goes out. Risk: overlapping PJL/PCL traffic if thread and main
  disagree on timing.
- **Option B — disable printer sleep via PJL**: send
  `@PJL SET POWERSAVE=OFF\r\n` in `rstartjob_cb`'s PJL header (before
  `ENTER LANGUAGE=PCL`), re-enable with `@PJL SET POWERSAVE=ON` at
  `rendjob_cb`. Check `Tech_Manual_Ch5_PJL` §POWERSAVE for whether the
  HL-5170DN honours this command. Cleanest solution if it works;
  downside is the printer stays on when idle between jobs.
- **Option C — stream GS output**: instead of buffering full raster
  before sending, pipe GS stdout directly into the PCL send path so
  bytes start flowing to the printer immediately after the PJL header.
  Eliminates the silent gap entirely at the cost of a more complex
  `rstartjob_cb` / `rwriteline_cb` wiring (GS must be launched as a
  child process with a pipe, and `rwriteline_cb` reads from that pipe).
  This is architecturally cleanest and also removes the intermediate
  buffer allocation for large photo pages.

Recommendation: try Option B first (single PJL line, no threading);
if the HL-5170DN doesn't honour `POWERSAVE=OFF`, fall back to Option A.
Reserve Option C for Phase 2 if A/B still produce stalls on real photo
input, because it requires restructuring the job-start path. Record the
outcome in this section before Phase 2 begins.

### Phase 1 — Skeleton driver: one page of text at 600 dpi

**Status: complete (2026-05-07).** `text-test.pdf` prints cleanly on
the physical HL-5170DN at 300 dpi (commit `2594fe8`). PAPPL's web admin
UI is reachable at `http://tuttle-pi.local:8000/` after enabling the
`_WEB_INTERFACE | _WEB_LOG | _WEB_REMOTE` SOPTIONS flags and supplying
a non-NULL `footer_html` to `papplMainloop`. Final default resolution
is 300 dpi (Investigation 2 finding); 600 dpi and HQ1200 deferred to
Phases 2 and 6 respectively.

Phase 1 surfaced more PAPPL bring-up gotchas than expected — the
1.3.1 → 1.4.10 source-build upgrade, the `papplLocGetString` /
`cupsArrayFind` / `strcmp` NULL-deref in PAPPL's default footer,
the `_WEB_LOG` flag not registering a `/logs` route in 1.4.10, and a
last-decimal-digit truncation bug in PAPPL's logger present in both
1.3.1 and 1.4.10. All recorded in
[`bring-up-notes.md`](bring-up-notes.md) so the next person hitting
them has a head start.

Smallest thing that prints. Resolves the PAPPL API shape before we
commit to feature work. All files live in a new top-level `src/`
directory; the repo root stays clean.

#### 1.1 — Source layout

```
src/
  main.c          # papplMainloop() entry point, system/printer setup
  driver.c        # driver_cb, all raster callbacks, identify_cb, status_cb
  pjl.c           # PJL header construction helpers (extracted so Phase 2 can extend)
  pjl.h
  packbits.c      # packbits encoder (used in rwriteline_cb)
  packbits.h
Makefile
hl5170dn-printer-app.service   # systemd unit template
```

No single-file approach — keeping `pjl.c` and `packbits.c` separate
means Phase 2 can extend PJL without touching raster code.

#### 1.2 — Makefile

`pkg-config`-driven; no autotools. Targets:

- `all` → `hl5170dn-printer-app`
- `install` → binary to `/usr/local/bin`, unit to
  `/etc/systemd/system/`, then `systemctl daemon-reload`
- `clean`

Flags: `-Wall -Wextra -g` for Phase 1 (strip `-g` in Phase 7).
Dependencies: `pappl`, `cupsfilters` (for `cfOneBitLine()`). No direct
`libusb` link — PAPPL's USB backend owns the device; the driver never
calls libusb directly.

#### 1.3 — `main.c`

```c
papplMainloop(argc, argv,
    "1.0",                      // version string
    NULL,                       // footer HTML
    1, drivers, driver_cb,      // one driver entry
    NULL,                       // setup_cb (unused)
    NULL);                      // subcmd_name / subcmd_cb (unused)
```

`drivers[]` is a single `pappl_pr_driver_t` entry:
`{ "hl5170dn", "Brother HL-5170DN", NULL, NULL }`.

`driver_cb` signature: `pappl_pr_driver_data_t *data, ipp_t **attrs,
const char *driver_name, void *cbdata` — fill `data` in place.

System creation via `papplSystemCreate()` with:
- `PAPPL_SOPTIONS_MULTI_QUEUE` off (single printer),
- hostname `NULL` (use system default / Bonjour),
- port 8000,
- `NULL` TLS options (plain HTTP for Phase 1).

Printer added via `papplSystemAddPrinter()` after system create, with
device URI from `argv` or hardcoded
`usb://Brother/HL-5170DN%20series?serial=L4J624176` for Phase 1 (make
it a `#define` so Phase 7 can replace it with a CLI flag).

#### 1.4 — `driver_cb` (in `driver.c`)

Fills `pappl_pr_driver_data_t`:

| Field | Phase 1 value |
|---|---|
| `input_face_up` | `false` |
| `has_copies` | `false` (Phase 2) |
| `identify_supported` | `PAPPL_IDENTIFY_ACTIONS_SOUND` (no-op stub) |
| `num_resolution` | 1 |
| `x_resolution[0]`, `y_resolution[0]` | 300 (Phase 0 finding 2: default 300 dpi) |
| `x_default`, `y_default` | 300 |
| `raster_types` | `PAPPL_PWG_RASTER_TYPE_BLACK_1` |
| `force_raster_type` | same |
| `num_media` | 1 — `"na_letter_8.5x11in"` |
| `media_default` | Letter, 300 dpi, `"tray-1"`, `"stationery"` |
| `num_source` | 2 — `"tray-1"`, `"by-pass-tray"` |
| `num_type` | 1 — `"stationery"` |
| `duplex` | `PAPPL_DUPLEX_NONE` |
| Callbacks | set all five: `rstartjob`, `rstartpage`, `rwriteline`, `rendpage`, `rendjob` |
| `status_cb` | stub returning immediately |
| `identify_cb` | stub returning immediately |

Note: `x_resolution` / `y_resolution` are in dots-per-inch as `int`.
`raster_types` must include `PAPPL_PWG_RASTER_TYPE_BLACK_1` so PAPPL
knows to give us 1-bit-per-pixel lines.

#### 1.5 — `rstartjob_cb`

Emits the PJL job header via `papplDeviceWrite()`. Exact byte sequence
(from Investigation 1 capture, verbatim field order):

```
<ESC>%-12345X
@PJL SET RESOLUTION=300\r\n
@PJL SET ECONOMODE=OFF\r\n
@PJL SET DUPLEX=OFF\r\n
@PJL SET SOURCETRAY=AUTO\r\n
@PJL SET MEDIATYPE=REGULAR\r\n
@PJL SET COPIES=1\r\n
@PJL SET LPARM : PCL PAPER=LETTER\r\n
@PJL ENTER LANGUAGE=PCL\r\n
```

Follow immediately with `papplDeviceFlush()`. Store the job-start
timestamp in the `job_data` pointer (allocated with `calloc`, freed in
`rendjob_cb`) — Phase 4 uses this for the log prefix. For Phase 1 the
struct can be a single `time_t`.

USB keepalive experiment lives here: before the PJL header, try
`@PJL SET POWERSAVE=OFF\r\n` and note whether it generates an error
response on the back-channel at `rendpage_cb` teardown time.

#### 1.6 — `rstartpage_cb`

Called once per page. Emits PCL raster setup:

```
<ESC>E                 (printer reset — clears any leftover state)
<ESC>*t300R            (raster resolution = 300 dpi)
<ESC>*r0F              (presentation mode: portrait, don't rotate)
<ESC>*b2M              (compression method: TIFF packbits)
<ESC>*r1A              (start raster at current cursor, 1 = top-left origin)
```

No paper size / source PCL commands — PJL owns those. `<ESC>E` resets
any previous raster state cleanly; safe to emit every page.

#### 1.7 — `rwriteline_cb`

Receives a `pappl_pr_line_t` with pre-halftoned 1-bit data from PAPPL
(forced by `force_raster_type = PAPPL_PWG_RASTER_TYPE_BLACK_1`).
PAPPL handles the halftoning; `cfOneBitLine()` is **not** needed in
Phase 1 — simplifies the dependency chain.

Per line:

1. Skip trailing blank lines at the bottom of the page by tracking
   the last non-blank line (defer the emit until we know it's not the
   final skip; or emit all lines — blank lines compress to near zero,
   so skip the optimisation for Phase 1).
2. Packbits-encode the line bytes using `packbits_encode()` from
   `packbits.c`.
3. Emit `<ESC>*b<n>W<encoded-bytes>` where `<n>` is the encoded length
   as a decimal ASCII integer.

`packbits.c` implements standard TIFF packbits (identical to what GS
ljet4 emits, confirmed by Investigation 1). The encoder fits in ~40
lines:

- Scan forward for runs of identical bytes → emit run-length token
  `(1 - runlen)` followed by the repeated byte.
- Scan forward for literal sequences → emit count-1 followed by the
  bytes verbatim.
- Output buffer is at most `ceil(n * 1.5) + 1` bytes; allocate once
  per job (store in `job_data`), reuse per line.

#### 1.8 — `rendpage_cb`

End of page:

```
<ESC>*rC               (end raster)
0x0C                   (form feed — eject the page)
<ESC>E                 (printer reset — clears raster mode cleanly)
```

#### 1.9 — `rendjob_cb`

End of job (UEL bookend):

```
<ESC>%-12345X
@PJL EOJ\r\n
<ESC>%-12345X
```

If Option B keepalive was used, re-enable sleep here:
`@PJL SET POWERSAVE=ON\r\n` before `EOJ`. Free `job_data`.

#### 1.10 — Systemd unit (`hl5170dn-printer-app.service`)

```ini
[Unit]
Description=Brother HL-5170DN Printer App
After=network.target

[Service]
ExecStart=/usr/local/bin/hl5170dn-printer-app server
User=printapp
Group=lp
Restart=on-failure
StandardError=journal

[Install]
WantedBy=multi-user.target
```

`printapp` is a dedicated system account (no login shell, no home dir)
in the `lp` group.  The `lp` group is the canonical Linux printer-access
group; the udev rule grants `lp` read/write on the Brother USB device
node so `printapp` can open it without running as root.

**Ubuntu install pattern for USB device permissions:**

1. **udev rule** (`99-brother-hl5170dn.rules`) — sets `GROUP=lp
   MODE=0660` on the device node matched by vendor/product ID.
   Installed by `make install` to `/etc/udev/rules.d/`.
2. **Dedicated system user** — `useradd -r -M -G lp -s /usr/sbin/nologin
   printapp` — no home directory, below UID 1000, in the lp group.
   `make install` creates this user idempotently (guards with `id -u`).
3. **Systemd unit** sets `User=printapp Group=lp`.  systemd applies
   supplementary group membership on service start so the process has
   the `lp` GID without needing `newgrp`.

`plugdev` is an alternative Ubuntu group for libusb devices, but `lp`
is semantically correct for a printer and more portable across distros.
Do NOT add the real login user to `lp` permanently — use the dedicated
service account and udev instead.

#### 1.11 — Exit criterion and verification

1. Build without warnings on the Pi: `make 2>&1 | grep -c warning`
   should be 0.
2. Run under `sudo` (or after adding `printapp` to `lp` group):
   `sudo ./hl5170dn-printer-app`.
3. `lp -d hl5170dn text-test.pdf` from the Pi.
4. Physical page comes out. No garbled header, no blank page.
5. Byte-diff: capture `papplDeviceWrite()` output by temporarily
   wrapping it (or use `usbmon`) and diff the PCL section against the
   baseline `cups-filter-baseline` stream for `text-test.pdf` at 300
   dpi. The PJL header should match field-for-field; the raster section
   should have the same compression mode and similar line counts.

If the page is blank or garbled, attach `usbmon` output and compare
against the Investigation 1 stream byte-by-byte before changing logic.

### Phase 2 — Feature parity with the baseline filter

#### 2.0 — Start here: unblock iPhone AirPrint, then add features

Three small changes go first because they unblock everything else and
two of them are one-liners:

1. **Wire PDF via `papplSystemAddMIMEFilter()`** — the headline fix.
   PAPPL 1.3.1's bring-up log said `JPEG is supported, PDF is not
   supported`; iPhone AirPrint sends `application/pdf` for photos, so
   this is why the iPhone test prints a blank page. PAPPL 1.4 added
   `papplSystemAddMIMEFilter()` for exactly this case. The Ghostscript
   Printer Application
   ([`OpenPrinting/ghostscript-printer-app`](https://github.com/OpenPrinting/ghostscript-printer-app))
   is the cleanest reference for the call signature and the GS
   invocation pattern — read its filter registration first, graft into
   our `system_cb`. Verification: reprint the iPhone photo that
   produced a blank page in Phase 1; expect a recognisable photo on
   paper. This also satisfies Phase 2 exit-criterion 2 (iPhone AirPrint
   of a photo).
2. **Drop `data->force_raster_type`** in `driver.c`. Comment out or
   remove the `force_raster_type = PAPPL_PWG_RASTER_TYPE_BLACK_1`
   line — `raster_types` alone declares what we accept, and the
   `force_raster_type` hint's behaviour in 1.4 is unclear. If the
   iPhone print still misbehaves after wiring PDF, this is the next
   variable to remove.
3. **Add a per-line trace** in `rwriteline_cb`: `if ((y % 256) == 0)
   papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "rwriteline y=%u bytes=%u",
   y, encoded_len);`. Two lines of code; gives us "are rasterlines
   actually being called, and at what dimensions" visibility from
   `runtime.log` without flooding it. Useful for the rest of Phase 2
   and beyond.

Once iPhone AirPrint of a photo prints something recognisable,
Phase 2 feature work proper:

#### 2.1 — Feature expansion

Adds every IPP attribute the baseline filter respects. The PJL
mapping table in the PRD (§"PJL command mapping") is copied verbatim
from `legacy/brother-hl5170dn-pjl`; it's known-good against this
printer.

- Resolutions: 300 + 600. (HQ1200 deferred to Phase 6.)
- Media sizes: A4, A5, A6, Legal, Executive, plus envelope variants
  (DL, C5, Com10, Monarch, ISOB5).
- Sources: `tray-1`, `by-pass-tray`, `auto`.
- Media types: per `legacy/brother-hl5170dn-pjl`'s mapping
  (`REGULAR`, `THICK`, `THICK2`, etc.).
- Duplex: `one-sided`, `two-sided-long-edge`, `two-sided-short-edge`.
  `BINDING` only emitted when `DUPLEX=ON`.
- Print quality: 3/4/5 → econo+300 / normal+600 / 1200
  (1200 nominal-only until Phase 6 confirms).
- Copies.

#### 2.2 — Phase 2 exit criteria

Manual test pass against:

1. `text-test.pdf` and `image-test.pdf` at 300 and 600 dpi, compared
   side-by-side with baseline output.
2. iPhone AirPrint of a photo: confirm a single rasterisation
   (PWG → PCL, no PDF detour) by reading the per-line trace added in
   §2.0 step 3. This is the test that reproduces the iOS stall
   scenario from the baseline; the new architecture (Option B
   keepalive in PJL header + 300 dpi default) should not exhibit it.
3. Multi-page PDF, duplex long-edge and short-edge.
4. Mid-print job cancel: PAPPL aborts cleanly, printer is not stuck
   in a bad PJL state on the next job.

#### 2.3 — Phase 3 prerequisite check

Before starting Phase 3 (media substitution), confirm the job-creation
hooks needed are present in PAPPL 1.4.10. Investigation 4's outcome
flagged this as a re-check item: `papplJobSetState()` and the early
job-attribute inspection point both need to exist for the
media-coercion logic to work. Quick verification — grep
`/usr/local/include/pappl/job.h` for `papplJobSetState` and any
`papplPrinterSetCreateCB` / job-creation callback registrations
before writing Phase 3 code.

### Phase 3 — Media substitution

The headline new feature, and the part most likely to need iteration
because the IPP-attribute hook is PAPPL-version-specific.

- Hook the job-creation path. Inspect IPP `media`. If it matches the
  PRD coercion table (Letter loaded → A4/A5/A6/Legal/Executive get
  rewritten to Letter; envelopes pass through unchanged), rewrite
  the attribute and mark the job substituted.
- Letter-loaded coercion table from PRD §"Media substitution".
  Inverse table when loaded paper is configured as A4.
- PAPPL vendor option `media-mismatch-action` with values `substitute`
  (default) and `reject`. In `reject` mode, fail the job at creation
  with `job-state-message` "Loaded paper is Letter; requested A4.
  Change client setting or reload printer."
- PAPPL vendor option for the loaded-paper size itself
  (default `Letter`, accepts `A4`).
- Notification through all three channels:
  - **Log line** at Info: `Job N: substituted Letter for A4 (loaded paper)`.
  - **`job-state-message`**: `Substituted Letter for requested A4`.
  - **`job-state-reasons`**: custom `media-substituted-warning` value.

Verification — PRD test items 6 and 7:

- Default `substitute` mode: A4 PDF from Mac (or `lp -o media=a4`)
  prints on Letter, content scaled to fit (no clipping), web UI log
  shows the substitution, `lpstat -W` / `ipptool` shows the
  `job-state-reasons` value. Envelope at envelope size *not* coerced.
- `reject` mode: A4 PDF fails before printing, client surfaces the
  error, no paper.

Risk: if the IPP-attribute rewrite isn't hooked at the right point,
PAPPL may set up its raster pipeline for A4 *before* the coercion
takes effect, producing an A4-sized raster with PJL claiming Letter
— the exact tray-mismatch the feature is meant to prevent. Mitigation
per PRD: capture `papplDeviceWrite()` output and verify the raster
header bytes show Letter dimensions before declaring the feature
working.

### Phase 4 — Observability

Mechanical work, but matters for debugging the rest of the project.

- Construct rich job-prefix per PRD §"Logging and observability":
  `Job N: <doc-or-job-name> from <host-or-user>`, ≤60 chars,
  control-char-stripped, newlines-replaced, source attributes are
  `document-name` / `job-name` / `document-format` / `job-originating-host-name`
  / `requesting-user-name`.
- Apply prefix to every `papplLogJob()` call in driver code:
  substitution events, PJL command summary at job start, supply
  polling, USB errors, page completions.
- Confirm PAPPL's web UI at `http://pi.local:8000/` shows logs and
  exposes per-printer log level.
- Sanitisation tests: submit a job with embedded newlines / control
  chars in `job-name` and confirm logs render cleanly.

### Phase 5 — Supply level polling

**Time-box: one focused day.** Per PRD: if it doesn't work, ship
without it and document the attempt.

- In `status_cb` (and once at `rendjob_cb` end), send the PJL
  `INFO STATUS` + `INFO PAGECOUNT` block, read with `papplDeviceRead()`
  on a 500 ms timeout.
- Parse per Brother's `Tech_Manual_Ch5_PJL` documentation. Map
  ready / low / out responses to `marker-levels` 75 / 10 / 0 if the
  printer reports state rather than a percentage.
- Fall back to `marker-levels=-2` (unknown) on no response.

Exit criterion: PRD test item 5 — web UI shows *something* sensible
(a level, or "unknown"). Not a crash.

### Phase 6 — HQ1200 (stretch)

Strategy depends on Phase 0 investigation 1.

- **If pi-printer's "HQ1200" is nominal-only** (no Mode 1027 in the
  capture): match it. Declare `HQ1200`, set `@PJL SET RESOLUTION=1200`,
  emit standard 600 dpi mode-2 raster. Document the limitation.
  This is PRD option 2.
- **If pi-printer is actually emitting Mode 1027 / 1024 / 1152**, or
  if we choose to chase actual 2400×600 output regardless: implement
  Brother's mode 1024 + 1152 raster compression from `Tech_Manual_Ch2_PCL`
  (Brother manual §6.3.8 + §6.3 "Horizontal 1200-dpi image format
  mode," roughly pages 89–101 of the PCL chapter). This is PRD
  option 3 and is the most interesting outcome for the meta-goal of
  evaluating agentic coding on undocumented vendor protocols. **Time-box
  this** — if focused effort doesn't yield working output, fall back
  to the nominal approach with documentation.

Either way: publish the byte-level investigation. It's useful
reference material for anyone else looking at this printer.

### Phase 7 — Build, packaging, deployment

- Finalise `Makefile` and `make install` targets:
  binary → `/usr/local/bin`, unit file → `/etc/systemd/system`,
  udev rule → `/etc/udev/rules.d/99-brother-hl5170dn.rules`.
- `make install` creates the `printapp` system user (idempotent via
  `id -u` guard) and reloads udev rules.  See §1.10 for rationale.
- README rewrite. Cover:
  - Build dependencies (`libpappl-dev`, `libcupsfilters-dev`,
    `ghostscript`, `libusb-1.0-0-dev`) with pinned versions.
  - Manual PAPPL build steps if the apt version is too old.
  - First-run setup, web UI URL, where the systemd unit logs to,
    how to change the loaded-paper vendor option, how to enable
    `reject` mode.
  - Comparison-to-baseline section: what the rewrite improved
    (iOS stall, media substitution, web UI), what it didn't
    (USB-only constraint, Pi 3B+ render speed), what regressed
    (if anything — re-evaluate after Phase 2).

### Phase 8 — Ship

- Manual smoke-test pass through the full PRD §Testing matrix
  (items 1–7). Document the pass in the release commit message.
- Tag the release.
- Write the agent-coding journal: which parts the agent got right
  first try, which needed iteration, which had to be done by hand.
  Publish alongside the README.

## What "done" looks like (PRD §"What 'done' looks like")

- `make && sudo make install && sudo systemctl enable --now hl5170dn-printer-app`
  on a fresh Pi OS install produces a working IPP-Everywhere printer.
- Mac Preview prints a PDF via Bonjour discovery.
- iPhone AirPrint prints a photo.
- Web UI at `http://pi.local:8000` shows logs, log level adjustable.
- Supply level field is populated (with whatever fidelity Phase 5
  achieved) and doesn't crash.
- README documents setup, design decisions, limitations, and the
  baseline comparison.
- Agent-coding journal documents the iteration story.

## Risks (from PRD §Risks; restated as project tracking items)

| Risk | Symptom | Mitigation | Phase |
|---|---|---|---|
| PCL 5e encoding bugs | Blank/garbled pages, printer errors | Tee `papplDeviceWrite()` output, byte-diff against `cups-filter-baseline` | 1, 2 |
| PAPPL API mismatch | Build/runtime failures | Pin a known-good PAPPL version | 0, 7 |
| USB back-channel doesn't work | Supply polling hangs | Aggressive timeout, fall back to "unknown" | 0, 5 |
| HQ1200 raster encoding undocumented | HQ1200 prints blank or at 600 dpi | Skip for v1 (Phase 1–5); attempt Mode 1027 only as Phase 6 stretch | 6 |
| Media coercion at wrong layer | Tray-mismatch pause despite substitution log entry | Verify raster header bytes show Letter before declaring done | 3 |
| Halftoning quality | Photo prints look poor | Start with libcupsfilters ordered dither (option 1); revisit if complaints | 1 |
| Pi 3B+ too slow on photo PDFs | Long render, possibly stalls | Phase 0 timing measurement; fall back to 300 dpi default or recommend Pi 5 | 0 |

## Open questions parked for the implementation phase

From PRD §"Open questions for the implementation phase":

- What does the baseline filter actually emit at HQ1200? — answered
  in Phase 0.
- Is GS render fast enough on Pi 3B+? — answered in Phase 0.
- Does `papplDeviceRead()` deliver back-channel data reliably? —
  answered in Phase 0.
- Default port 8000 vs something distinctive? — go with 8000, match
  `gutenprint-printer-app`. Override available in the systemd unit.

## Reference material in this repo

| File | Purpose |
|------|---------|
| `PRD-printer-applicance-rewrite-hl5170dn-pappl-driver.md` | The spec this plan operationalises |
| `phase-0-investigations.md` | Pi-side runbook for the four day-zero investigations. Findings get recorded inline. |
| `bring-up-notes.md` | PAPPL 1.3/1.4 quirks and other things learned during Phase 1 implementation that aren't in the PRD or vendor manuals. |
| `legacy/brother-hl5170dn-pjl` | Baseline CUPS filter — source of truth for the PJL mapping table |
| `legacy/Brother-HL5170DN-PCL.ppd` | Baseline PPD — historical reference for media/source/type names |
| `legacy/install.sh` | Installs the legacy filter; usable from any CWD via `$(dirname "$0")` resolution |
| `legacy/README.md` | Why the legacy directory exists and how to install from it |
| `Tech_Manual_AD.pdf` | Full Brother PCL/PJL Technical Reference Guide |
| `Tech_Manual_Ch2_PCL.pdf` / `.md` | Chapter 2: PCL. **§6.3.8 + §6.3 are the only authoritative source for HQ1200 mode 1024/1152.** |
| `Tech_Manual_Ch5_PJL.pdf` / `.md` | Chapter 5: PJL. Source for `INFO STATUS` response format used in Phase 5. |
| `text-test.pdf`, `image-test.pdf` | Manual-test inputs, reused from the baseline |
| Git tag `cups-filter-baseline` (`4602cfc`) | Frozen state of the previous CUPS-filter implementation, for diff/reference |

## What this project is testing about agent coding

(Per PRD's closing section — copied here so the meta-goal stays
visible during implementation.) The interesting question is not
"can a working HL-5170DN driver be written" — the baseline already
does that adequately. It's:

- Can an agentic coding loop navigate an unfamiliar C API (PAPPL)
  with thin documentation, learning from comparable drivers
  (`hp-printer-app`, `lprint`)?
- Can it correctly synthesise hardware-specific knowledge from a
  vendor PDF (Brother's PJL manual) without hallucinating commands?
- Can it produce a PCL byte stream that matches a known-good
  reference (the `cups-filter-baseline` Ghostscript output) closely
  enough to print?
- Where does it need human intervention — debugging actual printer
  output, USB protocol quirks, performance tuning?

The PRD, plan, code, and agent-interaction journal are published
together so others can calibrate their own expectations.
