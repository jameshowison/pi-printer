# Test Results

## Format

Each run records: date, code version (tag + git hash), test ID, IPP log verdict, and physical observation.
Physical column is blank for tests not yet physically verified.

---

## Run 1 — 2026-05-12

**Version:** 0.1.0 (git `62b7c91`)
**Binary built:** 2026-05-11
**Tester:** tuttle / jlh5498
**Printer:** Brother HL-5170DN (USB, tuttle-pi)

### Preflight

| Test | Result | Notes |
|------|--------|-------|
| PRE-1 service active | PASS | `systemctl is-active` → `active` |
| PRE-2 IPP reachable | PASS | `get-printer-attributes` → `successful-ok` |
| PRE-3 web UI loads | — | not checked this run |
| PRE-4 newest binary running | PASS | binary mtime (2026-05-11 14:58) < service start (2026-05-11 14:59) |

### Phase 2 — Output quality

| Test | IPP | Log verdict | Physical observation |
|------|-----|-------------|----------------------|
| P2-T1a text 300dpi duplex | PASS | `300dpi duplex=LONGEDGE` + `ok — 2 page(s)` | Acceptable. Slightly below quality of previous setup. Duplex correct (book-flip). Header visible ~1.5 in from top edge (see note 1). No footer visible (see note 2). |
| P2-T1b text 600dpi duplex | PASS | `gs … -r600` + `ok — 2 page(s)` | Better than T1a. Still slightly below previous setup but acceptable. |
| P2-T1c image 300dpi | PASS | `ok — 1 page(s)` | No header/footer visible (see note 3). Shading failures on line art — tonal graduations collapse (not acceptable for image content at 300 dpi). |
| P2-T1d image 600dpi | PASS | `gs … -r600` + `ok — 1 page(s)` | No header/footer visible. Shading on lines looks good. **Keep this sheet for Phase 6A APT comparison.** |
| P2-T1e chart 300dpi | PASS | `ok — 1 page(s)` | No header/footer visible. Halftoning broken: bottom geometric shapes — 25% renders white, 50% jumps to black; top area — 40% renders white, 50% jumps to black. Tonal range severely compressed at 300 dpi. |
| P2-T1f chart 600dpi | PASS | `gs … -r600` + `ok — 1 page(s)` | No header/footer visible. Halftoning correct across the tonal range. **Keep this sheet for Phase 6A decision gate.** |
| P2-T2 AirPrint text (URF) | PASS | `image/urf`; raster path; `ok` in 2s | Text from web page via Safari — looks great. "Best quality" selected on phone. No pdf_filter/GS involved. |
| P2-T2 AirPrint photo (JPEG) | PASS | `image/jpeg`; raster path; `ok` in 4s | Photo from iPhoto — acceptable; some vertical lines on outer edges (likely toner artifact, not a driver issue). About as good as this printer produces for photos. "Best quality" selected. |
| P2-T3a duplex short-edge | PASS | `duplex=SHORTEDGE` + `ok — 2 page(s)` | Calendar-style flip correct. |
| P2-T3b duplex long-edge | PASS | `duplex=LONGEDGE` + `ok — 2 page(s)` | Book-style flip correct. |

### Notes

1. **Header position (T1a/T1b):** Label appears ~1.5 in from the top of the printed sheet rather than at the top margin. GS BeginPage hook places text at y=775 on a 612×792 pt page; origin offset or media-box shift may be pushing it down. Needs investigation.

2. **Footer not printed (all tests):** Footer set at y=10 pt falls within the printer's unprintable bottom margin (~17 pt / 0.24 in on the HL-5170DN). Never visible. Needs coordinate adjustment.

3. **Header not visible on image/chart PDFs (T1c–T1f):** The BeginPage hook draws the label *before* page content. Full-page raster/vector content in image-test.pdf and chart-test.pdf paints on top of the label, hiding it. Text-test.pdf has white margins so the label shows through. Needs a different overlay approach (e.g. EndPage hook or foreground layer).

4. **Halftoning at 300 dpi (T1c, T1e):** Significant tonal compression — midtones below ~40–50% render as white, then snap to black. Not a regression; 600 dpi (T1d, T1f) halftones correctly. Low-resolution paths are acceptable for text but not for images or charts.

### Tests deferred / not yet run

| Test | Reason |
|------|--------|
| P2-T4 cancel mid-job | Not yet run |
| P3-T1 A4→Letter substitute | Not yet run |
| P3-T2 envelope not coerced | Not yet run |
| P3-T3 reject mode | Not yet run |
| P4-T1 control chars sanitised | Not yet run |
| P5-T1 supply level web UI | Not yet run |
| P6A-T1–T7 APT tests | Not yet run |

---

## Run 2 — 2026-05-13

**Version:** (git `5f6cbb1`)
**Tester:** tuttle / jlh5498
**Printer:** Brother HL-5170DN (USB, tuttle-pi)
**Script:** `apt-compare.sh` — printed chart-test.pdf in three conditions (job IDs 1–3 after service reinstall)

### Phase 6A — APT comparison (apt-compare.sh)

| Test | IPP | Log verdict | Physical observation |
|------|-----|-------------|----------------------|
| P6A-T1 APT chart (chart-apt.pdf) | PASS | `using APT Mode 1024 (150 dpi input)` + `apt_render: ok — 1 page(s)` | Printed. Quality worse than 600dpi direct — see decision gate below. Label visible at top and bottom. |
| P6A-T2 APT text quality | PASS | `apt_render: ok` | Text in APT output is thickened and blurry — not acceptable for text content. |
| P6A-T3 APT not taken at quality=4 | PASS | `pdf_filter` path used for 150dpi-direct (quality=4); no `apt_render` in log | APT correctly bypassed at quality=4. |

### Phase 6A — Decision gate

| Comparison | Observation |
|------------|-------------|
| 600dpi-direct vs APT-Mode-1024 (chart) | 600dpi clearly better — sharper halftones and text |
| 150dpi-direct vs APT-Mode-1024 (chart) | 150dpi better (note: 150dpi request was substituted to 600dpi by driver) |
| Overall ranking | 600dpi-direct > 150dpi-direct > APT-Mode-1024 |
| APT text quality | Poor — characters thickened and blurry |
| Recommended action | Do not use APT (quality=5) for standard printing; 600dpi quality=4 preferred |

### Bugs found and fixed this run

| Bug | Fix | Commit |
|-----|-----|--------|
| APT jobs printed a blank page after each real page | `apt_render_pdf` was sending `ESC*rC\x0c` directly then calling `rendpage_cb` which sent them again — double form feed. Removed the duplicate write. | `5f6cbb1` |
| `rendjob_cb` (PJL trailer) never called for APT jobs | `job_started` was incorrectly set to `false` before `goto done`. Removed that line. | `5f6cbb1` |
