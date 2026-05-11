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

The document `hl5170dn_revised_plan.md` records the architectural
decision made after Phase 1 to adopt driver-owned streaming raster
generation. That decision is incorporated here; the revised plan file
is kept as a historical record.

## Why a rewrite

The CUPS-filter baseline works for the 95% case (Mac/text PDFs) but has
structural problems the filter architecture can't fix from inside:

1. **AirPrint photo prints from iOS stall.** Job 10 sat for 1.5 h with
   the CUPS USB backend looping `Waiting for printer to become available`
   while the filter blocked on a full pipe. Diagnosed but not fixed in
   the baseline; root cause is the filter/backend split itself.
2. **A4-from-iPhone tray-mismatch pauses.** The PRD's media-substitution
   feature has to live above the filter layer (at the IPP attribute
   resolution step) to work, and CUPS doesn't give a filter that hook.
3. **No web admin UI, no live log viewer, no per-printer log level.**
   PAPPL provides all of these for free; CUPS does not.
4. **Generic upstream halftoning quality is poor.** Framework-owned
   `BLACK_1` raster throws away grayscale information before the driver
   can act on it.

A native PAPPL driver fixes all four by collapsing the pipeline into
one process that owns the IPP attributes, the raster generation, the
USB device, and the web UI together.

## Architectural decision (post-Phase-1)

Phase 1 validated PAPPL integration, USB transport ownership, PJL/PCL
correctness, and minimal raster output. It also revealed a fundamental
limitation of the `BLACK_1` bring-up path:

```
PDF → PAPPL raster → BLACK_1 → PCL
```

throws away grayscale information too early, preventing printer-aware
halftoning, adaptive photo handling, and APT experimentation.

**The Phase 1 `BLACK_1` architecture is a bring-up simplification, not
the final architecture.**

### Division of responsibility from Phase 2 onward

**PAPPL owns:** IPP, AirPrint, printer discovery, web UI, logging, USB
device ownership, job lifecycle, queue management.

**The driver owns:** Ghostscript invocation, grayscale raster generation,
halftoning, PCL encoding, raster streaming, print-quality strategy.

`papplSystemAddMIMEFilter()` is used only to accept document formats and
route them into the driver's rendering pipeline. PAPPL is not the owner
of final raster generation.

### Why the driver owns halftoning

Gutenprint evolved specifically because generic upstream dithering was
visually inferior, lacked printer-specific tuning, and could not adapt
to printer characteristics. The same considerations apply here. The
HL-5170DN is simpler than a photo inkjet — monochrome, fixed dot size —
but toner/dot gain behaviour and visible halftone structure still matter.
Grayscale data should remain intact as long as possible.

### Internal raster representation

The driver's canonical internal representation from Phase 2 onward is
**8-bit grayscale**, not 1-bit. Ghostscript produces grayscale pages
(`pgmraw`); the driver then decides threshold vs. ordered dither,
resolution, compression, and whether printer firmware should halftone.

### Streaming architecture

The original Phase 1 model buffered full raster before transmission.
That model increases memory usage, delays first-page output, complicates
cancellation, and creates long USB idle periods that risk printer
sleep-mode wedging.

Phase 2 adopts streaming:

```
Ghostscript stdout
    ↓
driver-side halftone
    ↓
PackBits encoder
    ↓
papplDeviceWrite()
```

Rows are processed incrementally. Benefits: minimal USB silence, low RAM
usage, responsive cancellation, immediate first-page output, and simpler
handling of large photo jobs. Streaming is the primary mitigation for
both Pi render latency and USB keepalive risk.

## Development discipline

### Build and install discipline

`make install` must always stop the running service before replacing the
binary, then start it again. Replacing a binary under a running process
has no effect on the live service — the old binary keeps running until
the process is restarted. Forgetting this causes the most common
service-development mistake: debugging behavior from a stale binary.

The install target follows this sequence:

```
systemctl stop hl5170dn-printer-app || true
install binary
install service unit / udev rules (if changed)
systemctl daemon-reload
systemctl start hl5170dn-printer-app
```

### Version verification

The binary embeds the git hash at build time via `-DGIT_HASH=…` in
`CFLAGS`. `DRIVER_VERSION` is defined as `"0.1-<hash>"` and logged at
startup. Before every debugging session, confirm the expected hash is
running:

```
journalctl -u hl5170dn-printer-app -n 5   # shows version string at startup
```

This makes stale-binary confusion immediately visible.

---

## Constraints carried over from the baseline plan

- **Keep the Brother HL-5170DN.** 2003-vintage USB+Ethernet mono laser.
  Mechanically excellent, toner is cheap.
- **Pi 3B+ as the print server.** Phase 0 investigation 2 confirms
  render times; streaming architecture mitigates the keepalive risk.
- **USB only.** Using `socket://...:9100` would route around the
  interesting hard parts (USB back-channel, libusb plumbing,
  sleep-mode quirks).

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

---

### Phase 0 — Day-zero investigations

**Status: complete (2026-05-07).**

**Phase 0 findings:**

1. **HQ1200 byte stream** — baseline uses genuine 1200 dpi raster with
   standard PCL5e mode-2 (packbits) and mode-3 (delta row) compression;
   no Brother proprietary mode 1027/1024/1152. Phase 6 = PRD option 2:
   declare HQ1200, set `@PJL SET RESOLUTION=1200`, run GS at 1200 dpi,
   emit mode-2 packbits. No proprietary encoding work required.

2. **GS render timing** — text PDF @ 600 dpi: ~0.8 s (fine). Image PDF
   @ 600 dpi: ~45 s; @ 300 dpi: ~21 s. Both exceed the safe USB
   keepalive window for photo input at 300 and 600 dpi.
   Streaming (Option C below) is the primary mitigation; Phase 1 defaults
   to 300 dpi as a conservative fallback. Pi 5 is the upgrade path for
   600 dpi photo performance without streaming.

3. **`papplDeviceRead()` back-channel** — works. Response in ~400 ms
   from sleep. STATUS codes: `CODE=10001` = READY, `CODE=40000` = SLEEP.
   `INFO SUPPLIES` returns `"?"` — toner level unavailable. Phase 5 can
   poll ready/sleep state; `marker-levels` will be `-2` (unknown).
   USB URI: `usb://Brother/HL-5170DN%20series?serial=L4J624176`.

4. **libpappl-dev version** — apt 1.3.1 is present on the Pi but
   source-built PAPPL 1.4.10 is the pinned baseline (required for
   `papplSystemAddMIMEFilter()` and related APIs). All required
   callbacks and device APIs present and compiling.

**Deployment note — rendering host.** The printer-app may run directly on
a Mac or Linux laptop with the Pi as a pure USB pass-through, or on the Pi
itself acting as an IPP server. Render times above are Pi 3B+ baselines;
a modern laptop renders 5–10× faster.

**Rendering and halftoning paths:**

| Path | GS device | Input res | Text quality | Photo quality | Pi render (text/photo) |
|------|-----------|-----------|-------------|---------------|------------------------|
| 300 dpi threshold | `pgmraw` | 300 dpi | Marginal | Poor | 0.8 s / 21 s |
| 600 dpi driver-side ordered dither | `pgmraw` | 600 dpi | Excellent | Very good | ~3 s / ~45 s |
| APT Mode 1024 (printer halftones) | `pgmraw` | ≤150 dpi | Poor† | Potentially best | ~0.5 s / ~5 s |

† APT at 150 dpi rasterises text poorly; APT is right for photo pages
only. For mixed content, 600 dpi ordered dither is the safe single path.

**Content-type strategy:**

| Content type | Planned render path |
|---|---|
| Text / vector-heavy | 600 or 1200 dpi driver-side dither |
| Mixed office documents | 600 dpi driver-side dither |
| Full-page photo jobs | APT Mode 1024 (if validated in Phase 6) |

**USB keepalive options (documented for reference; streaming is primary):**

- **Option A — PJL ping thread**: spawn a background thread in
  `rstartjob_cb` that sends `@PJL INFO STATUS\r\n` every ~8 s over
  the USB back-channel and drains the reply. Thread cancelled and joined
  before the first PCL byte goes out. Risk: overlapping PJL/PCL traffic.
- **Option B — disable printer sleep via PJL**: send
  `@PJL SET POWERSAVE=OFF\r\n` in `rstartjob_cb`'s PJL header, re-enable
  with `@PJL SET POWERSAVE=ON` at `rendjob_cb`. Cleanest if the
  HL-5170DN honours it; downside is the printer stays on between jobs.
  Check `Tech_Manual_Ch5_PJL` §POWERSAVE before relying on this.
- **Option C — streaming (adopted)**: pipe GS stdout directly into the
  PCL send path so bytes start flowing to the printer immediately after
  the PJL header. Eliminates the silent gap entirely. This is the
  Phase 2 architecture; A/B become fallbacks if streaming reveals
  edge-case stalls.

---

### Phase 1 — Skeleton driver: one page of text at 300 dpi

**Status: complete (2026-05-07).** `text-test.pdf` prints cleanly on
the physical HL-5170DN at 300 dpi (commit `2594fe8`). PAPPL web admin
UI reachable at `http://tuttle-pi.local:8000/` after enabling
`_WEB_INTERFACE | _WEB_LOG | _WEB_REMOTE` SOPTIONS flags and supplying
a non-NULL `footer_html` to `papplMainloop`. Final default resolution
is 300 dpi (Investigation 2 finding); 600 dpi and HQ1200 deferred to
Phases 2 and 6.

Phase 1 surfaced more PAPPL bring-up gotchas than expected — the
1.3.1 → 1.4.10 source-build upgrade, the `papplLocGetString` /
`cupsArrayFind` / `strcmp` NULL-deref in PAPPL's default footer,
the `_WEB_LOG` flag not registering a `/logs` route in 1.4.10, and a
last-decimal-digit truncation bug in PAPPL's logger present in both
1.3.1 and 1.4.10. All recorded in
[`bring-up-notes.md`](bring-up-notes.md) so the next person hitting
them has a head start.

Phase 1 intentionally prioritised protocol correctness over image
quality architecture. The `BLACK_1` path is now retired.

#### Source layout (established in Phase 1)

```
src/
  main.c          # papplMainloop() entry point, system/printer setup
  driver.c        # driver_cb, all raster callbacks, identify_cb, status_cb
  pjl.c           # PJL header construction helpers
  pjl.h
  packbits.c      # packbits encoder (used in rwriteline_cb)
  packbits.h
Makefile
hl5170dn-printer-app.service   # systemd unit template
```

#### Systemd unit and USB permissions

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
in the `lp` group. The udev rule (`99-brother-hl5170dn.rules`) sets
`GROUP=lp MODE=0660` on the device node matched by vendor/product ID.
`make install` creates the user idempotently (guards with `id -u`) and
installs the udev rule. Do NOT add the real login user to `lp`
permanently — use the dedicated service account and udev instead.

---

### Phase 2 — Driver-owned raster pipeline

**Status: implementation complete (2026-05-08); pending physical print
verification (§2.3 exit criteria).** §2.0–§2.2 implemented in one
pass: streaming pgmraw pipeline, ordered halftoning, full feature
expansion, and PJL parameterisation.

**Key implementation notes:**
- `force_raster_type` removed; `raster_types = SGRAY_8` so PAPPL
  delivers 8-bit grayscale to `rwriteline_cb` for the PWG/JPEG/PNG path.
- `pdf_filter_cb` now uses `popen()` with `pgmraw` — GS stdout is read
  incrementally, no temp file. PCL bytes reach the printer as soon as
  GS produces the first row.
- `pjl_write_job_header` refactored to accept `pjl_job_params_t`; all
  job params (duplex/paper/source/type/economode) derived from IPP
  options in `pjl_params_from_options()`.
- Per-page buffer allocation moved from `rstartjob_cb` to
  `rstartpage_cb` (we don't know page width at job start for the PDF path).
- `rendjob_cb` guards against `jd == NULL` (OOM in rstartjob) and
  `pdf_filter_cb` uses a `job_started` flag to avoid calling
  `rendjob_cb` if rstartjob never succeeded.

#### 2.0 — Start here: finish unblocking, then add features

1. **Wire PDF via `papplSystemAddMIMEFilter()`** — **done** (commit
   `b4f955a`).
2. **Drop `data->force_raster_type`** — **done**. Changed
   `raster_types` to `SGRAY_8`.
3. **Add a per-line trace** in `rwriteline_cb` — **done**:
   logs at y=0 and every 256 lines.

#### 2.1 — Establish streaming grayscale pipeline — **done**

Streaming architecture implemented:

```
Ghostscript stdout (pgmraw, 8-bit)
    ↓
driver-side halftone (threshold @ 300 dpi; ordered dither @ 600 dpi)
    ↓
PackBits encoder
    ↓
papplDeviceWrite()
```

- Launch GS as a child process; read stdout pipe incrementally.
- No full-page raster buffering.
- At 300 dpi: simple 50% threshold acceptable initially.
- At 600 dpi: clustered-dot ordered dither (deterministic, low CPU).
  Error diffusion is deferred.
- Add timing log entries: render start, first byte out, first page
  complete, job complete, periodic rasterline counters. Goal: distinguish
  render bottlenecks from USB bottlenecks.

#### 2.2 — Feature expansion — **done**

All IPP attributes from the baseline filter are now wired: The PJL mapping
table in `legacy/brother-hl5170dn-pjl` is known-good against this
printer and is the authoritative source for all PJL command values.

- **Resolutions:** 300 + 600 dpi. (HQ1200 deferred to Phase 6.)
- **Media sizes:** `na_letter_8.5x11in`, `iso_a4_210x297mm`,
  `iso_a5_148x210mm`, `iso_a6_105x148mm`, `na_legal_8.5x14in`,
  `na_executive_7.25x10.5in`, plus envelope variants (DL, C5, Com10,
  Monarch, ISOB5).
- **Sources:** `tray-1`, `by-pass-tray`, `auto`.
- **Media types:** per `legacy/brother-hl5170dn-pjl` mapping
  (`REGULAR`, `THICK`, `THICK2`, etc.).
- **Duplex:** `one-sided`, `two-sided-long-edge`,
  `two-sided-short-edge`. `BINDING` only emitted when `DUPLEX=ON`.
- **Print quality:** IPP values 3/4/5 → econo+300 / normal+600 /
  1200 (1200 nominal-only until Phase 6 confirms).
- **Copies.**

#### 2.3 — Exit criteria

See `testing.md §Phase 2` (P2-T1a through P2-T4).

#### 2.4 — Phase 3 prerequisite check

Before starting Phase 3, confirm the job-creation hooks needed are
present in PAPPL 1.4.10. Grep
`/usr/local/include/pappl/job.h` for `papplJobSetState` and any
`papplPrinterSetCreateCB` / job-creation callback registrations before
writing Phase 3 code.

---

### Phase 3 — Media substitution

**Status: implementation complete (2026-05-08); pending physical print
verification (§exit criteria).**

**Key implementation notes:**
- `apply_media_substitution()` in `driver.c` runs at the top of
  `rstartjob_cb`, before `pjl_params_from_options()`. Rewriting
  `options->media.size_name` there propagates to both the PJL PAPER=
  command AND the GS `-sPAPERSIZE=` argument (read from
  `options->media.size_name` in `pdf_filter_cb` after `rstartjob_cb`
  returns).
- PAPPL 1.4.10 has no public `papplJobSetState()` or per-job IPP
  attribute setter, so `job-state-reasons` uses the standard
  `PAPPL_JREASON_WARNINGS_DETECTED` / `PAPPL_JREASON_DOCUMENT_UNPRINTABLE_ERROR`
  bits rather than a custom string value. The substitute/reject distinction
  is visible in the log and `job-state-message`.
- Vendor options `loaded-paper` and `media-mismatch-action` are declared
  in `driver_cb` and settable via web UI or `lp -o loaded-paper=iso_a4_210x297mm`.
- PWG raster path (JPEG/PNG from PAPPL's internal filter): substitution
  changes the PJL PAPER= claim and `options->media` but cannot rescale
  raster data that PAPPL already dimensioned from the original media.
  Correct behaviour is limited to the PDF path (the primary use case).

- Hook the job-creation path. Inspect IPP `media`. If it matches the
  PRD coercion table (Letter loaded → A4/A5/A6/Legal/Executive get
  rewritten to Letter; envelopes pass through unchanged), rewrite
  the attribute **and** the raster dimensions to match the substituted
  media. The raster dimensions themselves must match the substituted
  media — not just the PJL paper claim — or the tray-mismatch pause
  persists despite the substitution log entry.
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

See `testing.md §Phase 3` (P3-T1 through P3-T3) for verification procedure.

---

### Phase 4 — Observability

Mechanical work, but matters for debugging the rest of the project.

- Construct rich job-prefix per PRD §"Logging and observability":
  `Job N: <doc-or-job-name> from <host-or-user>`, ≤60 chars,
  control-char-stripped, newlines-replaced. Source attributes:
  `document-name` / `job-name` / `document-format` /
  `job-originating-host-name` / `requesting-user-name`.
- Apply prefix to every `papplLogJob()` call in driver code:
  substitution events, PJL command summary at job start, supply
  polling, USB errors, page completions.

See `testing.md §Phase 4` (P4-T1) for verification.

---

### Phase 5 — Supply level polling

**Time-box: one focused day.** Per PRD: if it doesn't work, ship
without it and document the attempt.

- In `status_cb` (and once at `rendjob_cb` end), send the PJL
  `INFO STATUS` + `INFO PAGECOUNT` block, read with `papplDeviceRead()`
  on a 500 ms timeout.
- Parse per `Tech_Manual_Ch5_PJL` documentation. Map ready / low / out
  responses to `marker-levels` 75 / 10 / 0 if the printer reports
  state rather than a percentage. Phase 0 confirmed `INFO SUPPLIES`
  returns `"?"` — toner level unavailable; `marker-levels=-2` (unknown)
  is the expected fallback.
- Fall back to `marker-levels=-2` on no response. Ready/sleep state
  (`CODE=10001` / `CODE=40000`) can still be surfaced even if toner
  level cannot.

Exit criterion: see `testing.md §Phase 5` (P5-T1).

---

### Phase 6 — HQ1200 + APT photo quality (stretch)

Two investigations in one phase, either of which is a worthwhile
standalone result.

#### 6A — APT photo path (Mode 1024)

**Status: T5 offline gate passed (2026-05-09); pending physical print verification (T1–T4, T6–T7).**

##### What the manual actually says (§6.3.8)

`ESC*b1024M` sends the **entire page as one TIFF file** in a single
`ESC*b<N>W` command — not row-by-row.  Key constraints from the manual:

- "Valid only for 600 dpi data" — printer must be at 600 dpi via PJL
  and `ESC*t600R`, even though GS input is 150 dpi (the printer upscales).
- APT activates when: `BitsPerSample=8` + `Compression=1` (no compression)
  + printer at 600 dpi.  Setting `BitsPerSample=1` gives APT=OFF.
- Recommended input resolution: ≤150 dpi to keep data small.
- `SamplesPerPixel` must be 1 (monochrome TIFF only).
- Tags must be in ascending tag-ID order and must precede pixel data.
- PJL: `APT` and `IMAGEADAPT` are both settable for HL-5170DN (PJL manual
  line 859, line 735).  Both set ON in the job header when APT is active.

##### TIFF byte layout (exact)

Little-endian ("II") format, 12 tags, single strip, no libtiff needed.

```
Offset  Size  Content
 0       2    49 49  ("II" little-endian)
 2       2    2A 00  (TIFF magic 42)
 4       4    08 00 00 00  (IFD offset = 8)

IFD at offset 8:
 8       2    0C 00  (12 entries)

Tag entries (12 bytes each, ascending tag ID):
  256 ImageWidth        SHORT  1  w
  257 ImageLength       SHORT  1  h
  258 BitsPerSample     SHORT  1  8   ← triggers APT
  259 Compression       SHORT  1  1   (no compression)
  262 PhotometricInterp SHORT  1  1   (min-is-black; matches pgmraw 0=black)
  273 StripOffsets      LONG   1  174 (pixel data offset)
  277 SamplesPerPixel   SHORT  1  1
  278 RowsPerStrip      LONG   1  h   (single strip)
  279 StripByteCounts   LONG   1  w*h
  282 XResolution       RATIONAL 1  158 (offset to rational)
  283 YResolution       RATIONAL 1  166 (offset to rational)
  296 ResolutionUnit    SHORT  1  2   (inch)

After IFD:
  4 bytes: 00 00 00 00  (no next IFD)

Rational data at offset 158:
  158   8   150, 1  (XResolution = 150/1 dpi)
  166   8   150, 1  (YResolution = 150/1 dpi)

Pixel data at offset 174:
  w*h bytes  raw 8-bit grayscale (pgmraw order, 0=black, 255=white)
────────────────────────────────────────────────
Total header = 174 bytes.  For 150 dpi Letter (1275×1650): ≈ 2.0 MB/page.
```

##### PCL page sequence

```
ESC E              reset (clears prior raster state)
ESC *t 600 R       set resolution 600 dpi
ESC *b 1024 M      compression mode: TIFF/APT
ESC *r 1 A         start raster at cursor position
ESC *b <N> W <TIFF>  N = 174 + w*h; entire TIFF in one command
ESC *r C           end raster
\x0c               form feed
ESC E              reset for next page
```

##### PJL additions

`pjl_job_params_t` gains `bool apt`.  When true, `pjl_write_job_header()`
adds before `ENTER LANGUAGE=PCL`:

```
@PJL SET APT=ON
@PJL SET IMAGEADAPT=ON
```

Resolution is forced to 600 in the params when APT is active.

##### Driver integration

APT bypasses `rstartpage_cb` / `rwriteline_cb` / `rendpage_cb`.
`pdf_filter_cb` branches on `options->print_quality == IPP_QUALITY_HIGH`:

- HIGH → `apt_render_pdf()`: GS at 150 dpi, per-page TIFF-in-W-command
- else → existing pgmraw streaming path (unchanged)

`apt_render_pdf()` streams pixels: reads PGM header (gets w, h), emits
PCL framing + 174-byte TIFF header, then forwards GS pixel rows directly
to `papplDeviceWrite()`.  No full-page buffer needed.

New helper `apt_build_tiff_header(buf, w, h)` in `driver.c` builds the
174-byte header by direct byte writes (no libtiff dependency).

##### Test cases

T5 (offline TIFF validation) passed 2026-05-09. Physical printer tests T1–T4, T6–T7:
see `testing.md §Phase 6A` (P6A-T1 through P6A-T7).

**T5 passed (2026-05-09).** `test_apt_tiff.c` (standalone, no PAPPL) builds the
same 174-byte header via the same byte-write helpers.  `tiffinfo` output:

```
Image Width: 1275 Image Length: 1650
Resolution: 150, 150 pixels/inch
Bits/Sample: 8
Compression Scheme: None
Photometric Interpretation: min-is-black
Samples/Pixel: 1
Rows/Strip: 1650
Planar Configuration: single image plane
```

All correct.  Header arithmetic is verified.  If the printer fails on T1–T4,
the problem is PCL framing or PJL settings, not the TIFF structure.

**Next session starts here: T1 on the physical printer.**
Send `image-test.pdf` with `print-quality=high` via `ipptool` using
`tests/print-apt.test` (see `testing.md §P6A-T1`), watch the log for
`apt_render`, and compare physical output to the 600 dpi dither baseline
(P2-T1d). Then run T2–T4 in order.

##### Decision gate

See `testing.md §Phase 6A decision gate` for the outcome table and
next-action rules.

#### 6B — HQ1200

Phase 0 investigation 1 found the baseline uses standard 1200 dpi
packbits — no Mode 1027. Strategy is therefore PRD option 2:

Declare `HQ1200`, set `@PJL SET RESOLUTION=1200`, run GS at 1200 dpi,
emit mode-2 packbits. Text quality approaches typeset at 1200 dpi;
photos at 1200 dpi are impractical on Pi (render ~180 s) but fine on a
laptop. Recommend `HQ1200` only for text-dominant jobs on Pi, or
unrestricted on laptop.

If there is appetite for true 2400×600 output via Mode 1027, that is
PRD option 3: implement Brother's Mode 1027 raster format from
`Tech_Manual_Ch2_PCL` §6.3.13 (pages 98–100). **Time-box strictly** —
if a day of effort doesn't yield working output, fall back to option 2
and document the attempt.

---

### Phase 7 — Build, packaging, deployment

- Finalise `Makefile` and `make install` targets:
  binary → `/usr/local/bin`, unit file → `/etc/systemd/system`,
  udev rule → `/etc/udev/rules.d/99-brother-hl5170dn.rules`.
- `make install` creates the `printapp` system user (idempotent via
  `id -u` guard) and reloads udev rules.
- README rewrite. Cover:
  - Build dependencies (`libpappl-dev`, `libcupsfilters-dev`,
    `ghostscript`, `libusb-1.0-0-dev`) with pinned versions.
    Manual PAPPL 1.4.10 build steps (apt ships 1.3.1).
  - First-run setup, web UI URL, where systemd logs to, how to change
    the loaded-paper vendor option, how to enable `reject` mode.
  - Comparison-to-baseline: what the rewrite improved (iOS stall,
    media substitution, web UI, image quality), what it didn't (USB-only
    constraint, Pi 3B+ render speed for photos), what regressed
    (re-evaluate after Phase 2).

---

### Phase 8 — Ship

- Manual smoke-test pass through the full PRD §Testing matrix
  (items 1–7). Document the pass in the release commit message.
- Tag the release.
- Write the agent-coding journal: which parts the agent got right
  first try, which needed iteration, which had to be done by hand.
  Publish alongside the README.

---

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

---

## Risks

| Risk | Symptom | Mitigation | Phase |
|---|---|---|---|
| PCL 5e encoding bugs | Blank/garbled pages, printer errors | Tee `papplDeviceWrite()` output, byte-diff against `cups-filter-baseline` | 1, 2 |
| MIME routing mismatch | AirPrint bypasses driver renderer | Explicit logging and byte tracing | 2 |
| Streaming lifecycle mismatch | PAPPL renders before callbacks fire | Verify experimentally during Phase 2 | 2 |
| PAPPL API mismatch | Build/runtime failures | Pin PAPPL 1.4.10 | 0, 7 |
| USB back-channel doesn't work | Supply polling hangs | Aggressive timeout, fall back to "unknown" | 0, 5 |
| HQ1200 raster encoding undocumented | HQ1200 prints blank or at 600 dpi | Skip for v1 (Phase 1–5); attempt Mode 1027 only as Phase 6 stretch | 6 |
| Media coercion at wrong layer | Tray-mismatch pause despite substitution log entry | Verify raster header bytes show Letter dimensions before declaring done | 3 |
| Halftoning quality | Photo prints look poor | Ordered dither as default; APT as Phase 6 stretch | 2, 6 |
| Pi 3B+ too slow on photo PDFs | Long render, possibly stalls | Streaming architecture; 300 dpi default; Pi 5 upgrade path | 0, 2 |
| APT quality poor | Ugly photos | Keep driver-side dither as fallback | 6 |

---

## Open questions

- Does PAPPL 1.4.10 permit true incremental raster streaming through
  the driver's job lifecycle? (Verify during Phase 2.)
- Is APT visually superior enough to justify the complexity?
- Should per-page content heuristics exist eventually (text vs. photo
  detection to switch halftoning mode mid-job)?
- Is clustered-dot ordered dither sufficient, or is error diffusion
  worthwhile on this printer's dot gain curve?

---

## Reference material in this repo

| File | Purpose |
|------|---------|
| `PRD-printer-applicance-rewrite-hl5170dn-pappl-driver.md` | The spec this plan operationalises |
| `hl5170dn_revised_plan.md` | Records the Phase 1→2 architectural decision (driver-owned streaming raster); now incorporated here |
| `phase-0-investigations.md` | Pi-side runbook for the four day-zero investigations. Findings recorded inline. |
| `bring-up-notes.md` | PAPPL 1.3/1.4 quirks and other things learned during Phase 1 that aren't in the PRD or vendor manuals |
| `legacy/brother-hl5170dn-pjl` | Baseline CUPS filter — source of truth for the PJL mapping table |
| `legacy/Brother-HL5170DN-PCL.ppd` | Baseline PPD — historical reference for media/source/type names |
| `legacy/install.sh` | Installs the legacy filter |
| `legacy/README.md` | Why the legacy directory exists and how to install from it |
| `Tech_Manual_AD.pdf` | Full Brother PCL/PJL Technical Reference Guide |
| `Tech_Manual_Ch2_PCL.pdf` / `.md` | Chapter 2: PCL. §6.3.8 is the authoritative source for APT Mode 1024; §6.3.13 for Mode 1027. |
| `Tech_Manual_Ch5_PJL.pdf` / `.md` | Chapter 5: PJL. Source for `INFO STATUS` response format (Phase 5) and `POWERSAVE` command. |
| `text-test.pdf`, `image-test.pdf` | Manual-test inputs, reused from the baseline |
| Git tag `cups-filter-baseline` (`4602cfc`) | Frozen state of the previous CUPS-filter implementation, for diff/reference |

---

## What this project is testing about agent coding

(Per PRD's closing section — copied here so the meta-goal stays
visible during implementation.)

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
