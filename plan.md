# Brother HL-5170DN CUPS Driver — Plan

## Project Context

### Goals

- Print fast from a **Mac** (mostly text PDFs) over WiFi.
- Print from an **iPhone** (occasional photos) via AirPrint, no client setup.
- Optionally serve guests on the network without per-device setup.

### Constraints (stated explicitly so they're revisitable)

- **Keep the Brother HL-5170DN.** It's a 2003-vintage USB+10/100 Ethernet
  monochrome laser. Mechanically excellent, toner is cheap, the user is
  bloody-mindedly attached to it. A modern $150 AirPrint laser would
  delete this entire project but is off the table for now.
- **Pi 3B+ as the print server.** Current rendering machine. See
  "Performance Characteristics" below for when this becomes the
  bottleneck.
- **Existing infrastructure:** Mac on macOS (recent), iPhone on iOS,
  printer USB-attached to the Pi.

### Non-goals

- Multi-printer support, color, scanning, fax. Just printing to this
  one Brother.

## Performance Characteristics

The architecture has one unshakable cost: rendering a high-DPI bitmap
and shipping it to a 2003-era printer.

**Bitmap math** at 600dpi monochrome letter:
- 8.5″ × 11″ × 600dpi² ≈ 33 megapixels per page
- After PCL 5e delta-row compression, ~5–10 MB on the wire per text
  page; 10–25 MB for a photo-heavy page
- USB 2.0 effective throughput to this printer is ~50–100 Mbps real
  (the printer's USB receive buffer is small), so transfer is
  measured in seconds-to-tens-of-seconds for photo pages

**The unshakable part:** for any printer that doesn't speak modern
protocols natively (PDF/IPP Everywhere), the print server must
generate a full bitmap and stream it over the printer's connection.
The Pi can't get faster than "render 33 MP and push 25 MB over USB
2.0." A modern AirPrint printer ships ~3 MB of PDF over WiFi to the
printer's onboard renderer, which has hardware-accelerated halftoning
and renders directly to the print engine — fundamentally a different,
shorter pipeline. No amount of host-side compute closes that gap.

**Pi 3B+ is fine for the 95% case** (text PDFs from Mac, which is
what most printing is). It's marginal for photo PDFs from iOS, where
GS rendering can take minutes and run into the printer's USB
sleep-mode quirks. **Watch for an iPhone-photo print taking >30s as
the trigger to consider:**

1. Lowering `Resolution-default` to `300dpi` (4× less pixel work,
   3–4× smaller bitstream, mildly worse photo quality)
2. Upgrading the Pi to a Pi 5 (~5–10× faster GS render, same
   architecture, drop-in)
3. Switching the printer connection from USB to its built-in 10/100
   Ethernet (`socket://<printer-ip>:9100` instead of `usb://...`) —
   sidesteps libusb and printer-USB-sleep stalls, throughput is
   comparable to USB 2.0
4. Caving and buying a $150 modern AirPrint laser (deletes the
   project)

Mac/Linux clients with the PPD installed (Path B in the README) can
do the GS render locally and send raw PCL to the Pi, which costs the
Pi zero render work. iPhones cannot install drivers, so AirPrint
guests are stuck rendering on the Pi.

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

### Filter (`brother-hl5170dn-pjl`)

- GS device: `ljet4` (PCL 5e), resolution as plain integer (300 / 600 / 1200).
- GS gets `-sPAPERSIZE=$GS_PAPER` so its emitted paper escape matches the
  PJL setting.
- PJL header (all `@PJL SET`, no in-band PCL escapes):
  `RESOLUTION`, `ECONOMODE`, `DUPLEX`, `BINDING`, `SOURCETRAY`, `MEDIATYPE`,
  `COPIES`, then `LPARM : PCL PAPER` (PCL-specific per manual Ch5 §2,
  line 1010), then `@PJL ENTER LANGUAGE=PCL`. PJL SETs persist across
  the `<ESC>E` reset that begins GS's ljet4 output, so they take effect
  for the actual page data.
- PJL trailer: `<ESC>E` page eject + UEL + `@PJL` + final UEL.
- Input detection by hex magic via `od -An -tx1` (PostScript `%!`, PDF `%PDF`,
  PWG raster `RaS2`, PCL `0x1B`).
- AirPrint/PWG raster pipeline: `pwgtoraster` → `rastertopdf` → `gs -sDEVICE=ljet4`.
- Working directory variable `WORK_DIR` (does not shadow `$TMPDIR`).

### PPD (`Brother-HL5170DN-PCL.ppd`)

- `application/vnd.hp-PCL 0` — cost-0 passthrough for configured clients.
- `application/vnd.cups-postscript 150` — PostScript via filter.
- `application/pdf 200` — PDF via filter.
- `image/pwg-raster 150` — AirPrint via filter.

## Verification Steps

1. **GS smoke test on Pi** — done. `ljet4` produces canonical PCL 5e (`1b 45`
   ESC E reset, `1b 26 6c …` `<ESC>&l…` page setup, delta-row compressed
   raster). Use `-q` to suppress the GS banner, which otherwise lands in
   stdout and contaminates the dump:
   ```bash
   gs -q -dBATCH -dNOPAUSE -sDEVICE=ljet4 -sOutputFile=/dev/stdout test.pdf \
     | xxd | head
   ```

2. **Pi-side print test** — done. Job submitted via `lp -d <queue> file.pdf`,
   filter trace shows `Detected input type: pdf` → `Converting pdf → PCL 5e
   via Ghostscript (ljet4) at 600dpi paper=letter` → `Job N complete`. Pages
   print on the physical printer.

3. **Mac via Bonjour Shared (Path A) — done.** Mac added the queue via
   System Settings; Bonjour discovery worked but the local queue inherited
   the Pi's USB device URI. After fixing with
   `sudo lpadmin -p <queue> -v ipp://<pi>:631/printers/<pi-queue>`, prints
   went through end-to-end with duplex. README's "Mac Setup" recommends the
   IP-tab path to skip this gotcha.

4. **iPhone AirPrint — partial.** iPhone discovered the printer over
   Bonjour and Send-Document succeeded (3.6MB PDF payload reached the
   Pi). Filter started. Job stalled for ~1.5h then user canceled —
   USB backend was looping `Waiting for printer to become available`,
   suggesting the printer entered USB sleep mid-render and the filter
   blocked on a full pipe writing to the wedged USB backend. Not
   reproduced cleanly yet. Modern iOS sends `application/pdf` (not PWG
   raster) for AirPrint, so this exercises the PDF path, not the
   `pwgtoraster` chain.

## Operational Notes (learned from Pi-side test)

- **Filter debug output requires `cupsctl --debug-logging`.** CUPS's default
  `LogLevel warn` discards `DEBUG:`-prefixed lines, so `grep brother-hl5170dn
  /var/log/cups/error_log` returns nothing on a stock install. Toggle debug
  logging on while reproducing, off after.

- **`cupsctl` toggling restarts CUPS.** `lp` commands issued in the ~1–2s
  reload window get rejected with "The printer or class does not exist."
  Either `sleep 2` after `cupsctl`, or leave debug logging on for the test
  session.

- **Use `lpadmin -o KEY-default=VALUE` for queue defaults, never
  `lpoptions -o KEY=VALUE`.** `sudo lpoptions` writes to
  `/etc/cups/lpoptions` (a system-wide override) which silently beats the
  queue default in `/etc/cups/printers.conf`. The filter sees whichever
  the resolution chain ends up at; check it with the `Resolved: …` line
  in the trace, not `lpoptions -l`. README "CUPS option precedence" section
  has the full lookup order.

- **Driver also works on HL-5070N.** The two models share the same
  PJL/PCL vocabulary for every variable we use (Brother manual groups
  them together at lines 727 / 803 / 1014). Project name and PPD nickname
  target the HL-5170DN since that's the test hardware, but the 5070N is
  a drop-in if anyone has one.

## Open Items

- **iPhone photo print stall.** Job 10 (iPhone AirPrint, 3.6MB PDF) hung
  for 1.5h with the USB backend looping `Waiting for printer to become
  available`. Most likely cause: printer USB sleep mode + filter
  blocking on a full pipe to the wedged backend. Possible fixes (in
  rough order of cost):
  - Disable the printer's sleep/deep-sleep from its panel menu
  - Set `Resolution-default=300dpi` on the queue to cut bitstream 3–4×
    and shrink the rendering window the printer might sleep through
  - Switch the queue's device URI from `usb://...` to
    `socket://<printer-ip>:9100` (5170DN has 10/100 Ethernet); avoids
    libusb entirely. Requires giving the printer a stable IP.
  - Pi 5 upgrade — faster render, smaller window for the printer to
    sleep through
- **macOS Bonjour Shared inheriting the upstream USB device URI** —
  documented in README troubleshooting. Workaround is the IP-tab path
  for adding the printer; one-line `lpadmin -v ipp://...` fix if the
  user took the Bonjour Shared path.
- `cupstestppd` reports two pre-existing PPD-spec gaps: missing
  `*Manufacturer` and `*PSVersion`. Cosmetic; CUPS accepts the PPD
  without them. Worth fixing if pursuing strict spec compliance. (The
  earlier 8.3 `*PCFileName` warning is gone — fixed by the rename to
  `BR5170DN.PPD`.)

## Reference

| File | Purpose |
|------|---------|
| `Tech_Manual_AD.pdf` | Full Brother PCL/PJL Technical Reference Guide |
| `Tech_Manual_Ch2_PCL.pdf` / `.md` | Chapter 2: PCL Printer Control Language |
| `Tech_Manual_Ch5_PJL.pdf` / `.md` | Chapter 5: PJL Printer Job Language |
