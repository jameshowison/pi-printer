# Brother HL-5070N CUPS Driver — Plan

## Architecture

- **Configured Mac/Linux clients**: filter + GS installed locally; pre-render
  PDF → PCL 5e via `gs -sDEVICE=ljet4`; send raw PCL 5e to Pi; Pi wraps with
  PJL header only (cost 0 passthrough via `application/vnd.hp-PCL`).
- **AirPrint / visitors**: send PDF, PostScript, or PWG Raster; Pi converts
  via GS (`ljet4`) and wraps with PJL header.

## PCL 5e vs PCL XL — Resolved

**Resolution: PCL 5e only.** Searching the Brother PCL/PJL Technical Reference
Manual for `PCLXL`, `PCL XL`, `PCL6`, or `Enhanced` returns zero hits in either
chapter. Every `ENTER LANGUAGE` example uses `PCL` or `POSTSCRIPT`. Ch2 is
titled "PCL Printer Control Language" (singular) and uses escape-sequence
syntax (`<ESC>E` reset). The OpenPrinting "PCL6" tag for this printer means
PCL 6 Standard = PCL 5e.

Implemented in commit `7be63fc` (Switch from PCL XL to PCL 5e).

## Implementation State

### Filter (`brother-hl5070n-pjl`)

- GS device: `ljet4` (PCL 5e), resolution as plain integer (300 / 600 / 1200).
- PJL header: `@PJL SET RESOLUTION/ECONOMODE/DUPLEX/BINDING`, then
  `@PJL ENTER LANGUAGE=PCL`, then `<ESC>E` reset, then in-band PCL 5e
  escapes for tray / paper / media / copies (`<ESC>&l#H/A/M/X`).
- PJL trailer: `<ESC>E` page eject + UEL + `@PJL` + final UEL.
- Input detection by hex magic via `od -An -tx1` (PostScript `%!`, PDF `%PDF`,
  PWG raster `RaS2`, PCL `0x1B`).
- AirPrint/PWG raster pipeline: `pwgtoraster` → `rastertopdf` → `gs -sDEVICE=ljet4`.
- Working directory variable `WORK_DIR` (does not shadow `$TMPDIR`).

### PPD (`Brother-HL5070N-PCL.ppd`)

- `application/vnd.hp-PCL 0` — cost-0 passthrough for configured clients.
- `application/vnd.cups-postscript 150` — PostScript via filter.
- `application/pdf 200` — PDF via filter.
- `image/pwg-raster 150` — AirPrint via filter.

## Verification Steps

1. **GS smoke test on Pi.** Confirm `ljet4` produces PCL 5e:
   ```bash
   gs -dBATCH -dNOPAUSE -sDEVICE=ljet4 -sOutputFile=/dev/stdout image-test.pdf \
     | xxd | head
   # Expect: 1b 45 ... (ESC E reset) followed by PCL 5e escape sequences
   ```

2. **Test print from configured client.** Print PDF from Mac → Pi → printer.
   Check `/var/log/cups/error_log` for `Detected input type: pcl` (passthrough)
   and the resolved option values.

3. **Test AirPrint.** Print from iPhone. Pi log should show
   `Detected input type: raster` (PWG) or `pdf`.

4. **Verify each PJL setting takes effect.** If tray/paper/media options don't
   work, the next thing to try is moving them from in-band PCL escapes to PJL
   `SET` with the `LPARM : PCL` modifier (manual Ch5 §6, line 1010 — PCL-specific
   variables "must be set using the LPARM : PCL option").

## Open Items

- `cupstestppd` reports two pre-existing PPD-spec gaps: missing `*Manufacturer`
  and `*PSVersion`. Cosmetic; CUPS accepts the PPD without them. Worth fixing
  if pursuing strict spec compliance.
- `*PCFileName` warning (8.3 limit violation) — same status.

## Reference

| File | Purpose |
|------|---------|
| `Tech_Manual_AD.pdf` | Full Brother PCL/PJL Technical Reference Guide |
| `Tech_Manual_Ch2_PCL.pdf` / `.md` | Chapter 2: PCL Printer Control Language |
| `Tech_Manual_Ch5_PJL.pdf` / `.md` | Chapter 5: PJL Printer Job Language |
