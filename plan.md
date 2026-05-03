# Brother HL-5070N CUPS Driver — Plan

## Architecture

- **Configured Mac/Linux clients**: filter + GS installed locally; pre-render PDF → PCL XL
  via `gs -sDEVICE=pxlmono`; send raw PCL XL to Pi; Pi wraps with PJL header only.
- **AirPrint / visitors**: send PDF, PostScript, or PWG Raster; Pi converts via GS
  (`pxlmono`) and wraps with PJL header.

## Open Question: PCL XL vs PCL 5e

**This is the most important unresolved issue.**

The filter and PPD are built around PCL XL (PCL 6 Enhanced), using Ghostscript's `pxlmono`
device. But "PCL6" as listed on OpenPrinting may mean PCL 6 Standard (= PCL 5e), which is
a completely different protocol.

**To verify:** Check `Tech_Manual_Ch2_PCL.md` for the HL-5070N's supported PDL.
Specifically look for whether `@PJL ENTER LANGUAGE=PCLXL` is listed as a valid language
value. The PJL language-switching section (Ch5 §4.6, "Printer Language Switching") will
list accepted `ENTER LANGUAGE=` values for this printer family.

**If PCL 5e only (not PCL XL):**
- Change GS device from `pxlmono` to `ljet4` (PCL 5e)
- PJL SET commands for PAPER/TRAY/MEDIA/COPIES remain valid
- GS_RES reverts to plain integers (no `1200x600`)
- PPD MIME type reverts from `application/vnd.hp-PCLXL` to something appropriate

## Completed Fixes

All implemented in commit `db5ec63`:

| # | Severity | Fix | Status |
|---|----------|-----|--------|
| 1 | Critical | Replace PCL5 escape sequences + `ENTER LANGUAGE=PCL` in PJL header with `@PJL SET` commands; remove `ESC E` from trailer | Done |
| 2 | Critical | Replace PCL5 numeric codes for paper/tray/media with PJL string values (`LETTER`, `MPTRAY`, `ENVELOPE`, etc.) | Done |
| 3 | Critical | PPD: swap `cups-raster` for `hp-PCLXL` (cost 0) + add `pwg-raster`; filter: fix PWG raster pipeline (`pwgtoraster`→`rastertopdf`→GS); fix PCL detection with `od` hex | Done |
| 4 | Significant | Rename `TMPDIR` → `WORK_DIR` to avoid shadowing system env var | Done |
| 5 | Minor | `GS_RES="1200x600"` for HQ1200 mode (matches printer's 1200×600 raster geometry) | Done |
| 6 | Minor | Remove unused `bc` dependency from filter header | Done |
| 7 | Minor | Update README caveat: replace stale "double-reset" note with PJL SET ordering note | Done |

## PJL SET Values to Verify

From `Tech_Manual_Ch5_PJL.md` — confirm these string values are correct for HL-5070N:

| Setting | PJL Variable | Values used in filter |
|---------|-------------|----------------------|
| Paper size | `PAPER` | `LETTER`, `LEGAL`, `EXECUTIVE`, `A4`, `A5`, `A6`, `COM10`, `MONARCH`, `DL`, `C5`, `B5ENVELOPE` |
| Input tray | `SOURCETRAY` | `MPTRAY`, `TRAY1`, `TRAY2`, `MANUALFEED`, `AUTOSELECT` |
| Media type | `MEDIATYPE` | `PLAIN`, `THIN`, `THICK`, `THICKER`, `BOND`, `ENVELOPE`, `ENVTHICK`, `ENVTHIN` |
| Resolution | `RESOLUTION` | `300`, `600`, `1200` |
| Duplex | `DUPLEX` | `ON`, `OFF` |
| Binding | `BINDING` | `LONGEDGE`, `SHORTEDGE` |
| Toner save | `ECONOMODE` | `ON`, `OFF` |
| Copies | `COPIES` | integer |

## Verification Steps

1. **Read Tech Manual** — confirm PCL XL support and PJL SET string values:
   - `Tech_Manual_Ch5_PJL.md` §6 (Environment Commands) for SET variable names/values
   - `Tech_Manual_Ch5_PJL.md` §4.6 for `ENTER LANGUAGE=` accepted values

2. **GS smoke test** on Pi:
   ```bash
   gs -dBATCH -dNOPAUSE -sDEVICE=pxlmono -sOutputFile=/dev/stdout image-test.pdf \
     | xxd | head
   # Expect: 1b 25 2d 31 32 33 34 35 58 40 50 4a 4c (UEL + @PJL)
   ```

3. **Test print** — check `/var/log/cups/error_log` for filter DEBUG lines confirming
   resolved option values.

4. **Test PCL passthrough** — print from configured Mac; Pi log should show
   `Detected input type: pcl`.

5. **Test AirPrint** — print from iPhone; Pi log should show `pdf` or `raster`.

## Reference

| File | Purpose |
|------|---------|
| `Tech_Manual_AD.pdf` | Full Brother PCL/PJL Technical Reference Guide |
| `Tech_Manual_Ch2_PCL.pdf` / `.md` | Chapter 2: PCL Printer Control Language |
| `Tech_Manual_Ch5_PJL.pdf` / `.md` | Chapter 5: PJL Printer Job Language |
