# Brother HL-5170DN — Native PAPPL Driver: Revised Architecture Plan

## What this plan covers

This document operationalises the rewrite of the existing:
- CUPS
- shell-script filter
- Ghostscript `ljet4`
pipeline

into a single PAPPL-based printer application:

`hl5170dn-printer-app`

running as a systemd service.

This revision supersedes the earlier Phase-1-oriented architecture plan.
The major architectural change is:

> The driver now explicitly owns raster generation, halftoning,
> and streaming.

PAPPL remains responsible for:
- IPP
- AirPrint
- web UI
- logging
- printer lifecycle
- USB device ownership
- job management

but no longer owns final raster generation strategy.

This change is informed heavily by:
- Gutenprint's architecture,
- Phase 0 measurements,
- and observed limitations of framework-owned `BLACK_1` raster paths.

The previous filter-based implementation remains preserved at:

- git tag `cups-filter-baseline`
- commit `4602cfc`

---

# Why the rewrite exists

The baseline CUPS filter implementation works adequately for:
- text PDFs
- Mac-originated office documents

but suffers from structural limitations:

1. iPhone AirPrint photo jobs stall under load
2. Media substitution cannot occur at the correct IPP layer
3. CUPS filter/backend separation complicates USB ownership
4. No integrated web UI or observability
5. Generic upstream halftoning quality is poor

PAPPL fixes:
- lifecycle ownership,
- AirPrint integration,
- logging,
- and administration.

The revised raster architecture fixes:
- image quality,
- streaming behaviour,
- and render latency problems.

---

# Architectural decision (post-Phase-1)

## Phase 1 conclusions

Phase 1 validated:
- PAPPL integration,
- USB transport ownership,
- PJL/PCL correctness,
- and minimal raster output.

It also revealed a major architectural limitation:

```text
PDF → PAPPL raster → BLACK_1 → PCL
```

throws away grayscale information too early.

This prevents:
- printer-aware halftoning,
- adaptive photo handling,
- future quality tuning,
- and experimentation with APT.

The Phase 1 `BLACK_1` architecture is therefore considered:
- a bring-up simplification,
- not the final architecture.

---

# Final rendering architecture

The canonical Phase 2+ architecture is:

```text
Document/PDF
    ↓
Ghostscript rasterisation (8-bit grayscale)
    ↓
driver-controlled halftoning
    ↓
PCL raster encoding
    ↓
printer
```

This mirrors the broad design pattern used historically by Gutenprint:

```text
high-bit-depth raster
    ↓
driver-side screening/dithering
    ↓
printer-specific output encoding
```

rather than:

```text
framework-pre-halftoned 1-bit raster
```

---

# Why the driver owns halftoning

## Gutenprint comparison

Gutenprint evolved specifically because generic upstream dithering:
- was visually inferior,
- lacked printer-specific tuning,
- and could not adapt to printer characteristics.

The same considerations apply here.

The HL-5170DN is simpler than a photo inkjet:
- monochrome only,
- fixed dot size,
- no multi-pass carriage alignment,

but two critical issues still matter:

1. toner/dot gain behaviour
2. visible halftone structure

Therefore:
- grayscale data should remain intact as long as possible,
- and the driver should own screening decisions.

---

# Internal raster representation

The driver's canonical internal representation from Phase 2 onward is:

```text
8-bit grayscale raster
```

not:
```text
1-bit raster
```

Ghostscript produces grayscale pages (`pgmraw`).

The driver then decides:
- threshold vs ordered dither,
- resolution,
- compression,
- and whether printer firmware should perform halftoning.

---

# Streaming architecture

The original Phase 1 model buffered raster before transmission.

That model:
- increases memory usage,
- delays first-page output,
- complicates cancellation,
- and creates long USB idle periods.

Phase 2 adopts streaming instead:

```text
Ghostscript stdout
    ↓
driver-side halftone
    ↓
PackBits encoder
    ↓
papplDeviceWrite()
```

Rows are processed incrementally.

Benefits:
- minimal USB silence
- low RAM usage
- responsive cancellation
- immediate first-page output
- simpler handling of large photo jobs

Streaming is now considered the primary fix for:
- Pi rendering latency,
- and USB sleep timing issues.

---

# Relationship to PAPPL

PAPPL remains responsible for:
- IPP
- AirPrint
- printer discovery
- web UI
- USB device ownership
- logging
- job lifecycle
- queue management

The driver owns:
- Ghostscript invocation
- grayscale raster generation
- halftoning
- PCL encoding
- raster streaming
- print-quality strategy

`papplSystemAddMIMEFilter()` is used only to:
- accept document formats,
- and route them into the driver's rendering pipeline.

PAPPL is not considered the owner of final raster generation.

---

# Rendering paths

| Path | Halftoning owner |
|---|---|
| 300 dpi threshold | Driver |
| 600 dpi ordered dither | Driver |
| HQ1200 | Driver |
| APT Mode 1024 | Printer firmware |

APT is treated as:
- a specialised photo path,
- not the universal high-quality mode.

---

# Planned content strategies

| Content type | Planned render path |
|---|---|
| Text/vector-heavy | 600 or 1200 dpi driver-side dither |
| Mixed office documents | 600 dpi driver-side dither |
| Full-page photo jobs | APT Mode 1024 (if validated) |

The driver may later evolve toward:
- per-page heuristics,
- rather than single-job mode selection.

---

# Constraints

- Keep the Brother HL-5170DN
- Pi 3B+ remains the baseline deployment target
- USB-only operation
- No CUPS at runtime
- No Snap packaging
- Build cleanly on Raspberry Pi OS 64-bit

---

# Phase 0 — Completed investigations

## Findings summary

### HQ1200 byte stream

The baseline implementation uses:
- standard PCL packbits compression,
- no undocumented Brother mode.

HQ1200 therefore becomes:
- 1200 dpi raster
- plus standard mode-2 PackBits.

---

### Ghostscript timing

Pi 3B+ render timing:

| Input | Resolution | Time |
|---|---|---|
| text PDF | 600 dpi | ~0.8 s |
| photo PDF | 300 dpi | ~21 s |
| photo PDF | 600 dpi | ~45 s |

This strongly motivates:
- streaming architecture,
- and incremental rendering.

---

### Back-channel support

`papplDeviceRead()` works correctly.

Observed:
- READY state
- SLEEP state
- page count responses

Supply percentage remains unavailable.

---

### PAPPL version

PAPPL 1.4.10 is the pinned baseline.

---

# Phase 1 — Completed bring-up

## Status

Phase 1 completed successfully.

Validated:
- PAPPL integration
- USB ownership
- PJL correctness
- basic PCL raster path
- systemd integration
- web UI

Phase 1 intentionally prioritised:
- protocol correctness,
- not image quality architecture.

The Phase 1 `BLACK_1` path is now retired.

---

# Phase 2 — Driver-owned raster pipeline

## Goals

Phase 2 establishes the final architecture:
- streaming
- grayscale ownership
- driver-side halftoning
- AirPrint compatibility
- image-quality parity

---

## 2.1 — Remove framework-owned halftoning

Remove:

```c
force_raster_type = PAPPL_PWG_RASTER_TYPE_BLACK_1;
```

and stop relying on PAPPL-generated final raster.

The driver's internal representation becomes:
- 8-bit grayscale.

---

## 2.2 — Introduce streaming Ghostscript pipeline

Canonical pipeline:

```text
Ghostscript stdout
    ↓
driver-side dither
    ↓
PackBits encoder
    ↓
papplDeviceWrite()
```

Implementation:
- launch GS subprocess
- render `pgmraw`
- stream rows incrementally
- dither rows in-process
- emit PCL immediately

No full-page raster buffering.

---

## 2.3 — MIME routing

Use `papplSystemAddMIMEFilter()` only to:
- accept PDF,
- PWG Raster,
- Apple Raster,
- JPEG,
- PNG

and route them into the driver's renderer.

---

## 2.4 — Initial dithering strategy

### 300 dpi

Simple threshold acceptable initially.

### 600 dpi

Use:
- clustered-dot ordered dither
- deterministic output
- low CPU overhead

Error diffusion is deferred.

---

## 2.5 — Logging and timing

Add:
- render start timestamp
- first byte timestamp
- first page completion timestamp
- job completion timestamp
- periodic rasterline counters

Goal:
- distinguish render bottlenecks from USB bottlenecks.

---

## 2.6 — AirPrint validation

Primary Phase 2 success criterion:

- iPhone AirPrint photo prints successfully
- no stalls
- recognisable output
- no queue wedging

---

# Phase 3 — Media substitution

## Goals

Implement:
- A4↔Letter coercion
- substitution/reject modes
- user-visible notifications

---

## Important semantic clarification

Media substitution means:

1. logical media rewrite
2. fit-to-page scaling
3. correct raster dimensions
4. correct PJL declaration

not merely:
- changing PJL paper claims.

The raster dimensions themselves must match the substituted media.

---

## Modes

### substitute (default)

A4/A5/A6/etc:
- rewritten to loaded paper
- scaled appropriately
- printed automatically

### reject

Reject at job creation with:
- explicit IPP error
- clear UI message

---

# Phase 4 — Observability

## Goals

Provide:
- structured logs
- streaming diagnostics
- sanitised job names
- per-printer log levels

All driver logs should include:
- job id
- job name
- originating host/user
- timing information

---

# Phase 5 — Back-channel status

## Scope

Time-boxed to one focused day.

Attempt:
- status polling
- sleep detection
- page count
- toner state mapping

Fallback:
- unknown supply state

No instability allowed.

---

# Phase 6 — Advanced rendering

## 6A — APT Mode 1024

Investigate:
- printer-side photo halftoning

Pipeline:

```text
PDF
    ↓
150 dpi grayscale
    ↓
TIFF wrapper
    ↓
Mode 1024
    ↓
printer APT halftoning
```

This is:
- experimental,
- photo-focused,
- and not the default path.

---

## 6B — HQ1200

Implement:
- 1200 dpi grayscale render
- driver-side halftone
- standard PackBits encoding

Mode 1027 remains:
- optional stretch work,
- strictly time-boxed.

---

# Phase 7 — Packaging

## Deliverables

- `make install`
- systemd unit
- udev rules
- dedicated `printapp` user
- README
- deployment documentation

---

# Phase 8 — Release

## Release criteria

- Bonjour discovery works
- Mac PDF printing works
- iPhone AirPrint photo printing works
- media substitution works
- web UI works
- no runtime CUPS dependency
- documented installation path

---

# Major risks

| Risk | Symptom | Mitigation |
|---|---|---|
| PCL encoding bug | Blank/garbled pages | usbmon diff against baseline |
| MIME routing mismatch | AirPrint bypasses intended renderer | explicit logging and byte tracing |
| Streaming lifecycle mismatch | PAPPL renders before callbacks | verify experimentally during Phase 2 |
| Pi too slow for HQ1200 photos | long render time | document limitations |
| APT quality poor | ugly photos | keep driver-side dither as default |
| Media substitution at wrong layer | tray mismatch persists | verify raster dimensions |

---

# Open questions

- Does PAPPL permit true incremental raster streaming through the driver's lifecycle?
- Is APT visually superior enough to justify complexity?
- Should per-page content heuristics exist eventually?
- Is ordered dither sufficient, or is error diffusion worthwhile on this printer?

---

# What “done” looks like

A fresh Pi OS install can:

```bash
make
sudo make install
sudo systemctl enable --now hl5170dn-printer-app
```

and immediately provide:
- working Bonjour printer discovery
- AirPrint support
- robust PDF/photo printing
- media substitution
- web administration
- structured logs
- stable USB operation

without:
- CUPS runtime dependencies,
- external filter scripts,
- or manual intervention.

