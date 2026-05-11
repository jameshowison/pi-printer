# Brother HL-5170DN PAPPL Printer App

A native [PAPPL](https://www.msweet.org/pappl/)-based printer application for the Brother HL-5170DN USB laser printer, running as a systemd service on a Raspberry Pi 3B+. Replaces the earlier CUPS + Ghostscript filter pipeline; see [Comparison with the CUPS baseline](#comparison-with-the-cups-baseline).

Source: <https://github.com/jameshowison/pi-printer>

## How it works

```
iPhone / Mac / Linux
      ↓ IPP / AirPrint (port 8000)
hl5170dn-printer-app (PAPPL)
      ↓ Ghostscript (pgmraw, 8-bit grayscale)
      ↓ driver-side halftone + PackBits
      ↓ PJL + PCL5e
HL-5170DN (USB)
```

PAPPL owns IPP, AirPrint advertisement, web UI, and the USB device. The driver owns Ghostscript invocation, grayscale raster generation, halftoning, and PCL encoding. No CUPS required at runtime.

## Build

### 1. Install PAPPL 1.4.10 from source

The apt package (`libpappl1t64 / libpappl-dev`) is version 1.3.1, which lacks `papplSystemAddMIMEFilter()` — the API required for PDF support. You must build 1.4.x from source.

```bash
# PAPPL build dependencies
sudo apt-get install -y \
    libcups2-dev libssl-dev zlib1g-dev libpam0g-dev \
    libavahi-client-dev libusb-1.0-0-dev

# Build and install PAPPL 1.4.10
git clone https://github.com/michaelrsweet/pappl.git ~/pappl
cd ~/pappl && git checkout v1.4.10
./configure --prefix=/usr/local
make -j4 && sudo make install && sudo ldconfig
```

Verify:
```bash
pkg-config --modversion pappl          # expect 1.4.10
ldd ./hl5170dn-printer-app | grep pappl  # expect /usr/local/lib/libpappl.so.1
```

Leave the apt `libpappl1t64` runtime installed (other packages may depend on it), but remove `libpappl-dev` to avoid header-path ambiguity.

### 2. Install runtime dependencies

```bash
sudo apt-get install -y ghostscript libusb-1.0-0
```

### 3. Build the printer app

```bash
cd ~/pi-printer
make
```

`PKG_CONFIG_PATH` in the Makefile points at `/usr/local/lib/pkgconfig` so the source-built PAPPL is picked up automatically.

## Install

```bash
sudo make install
sudo systemctl enable --now hl5170dn-printer-app
```

`make install` idempotently:
- Copies the binary to `/usr/local/bin/`
- Installs the systemd unit to `/etc/systemd/system/`
- Installs the udev rule to `/etc/udev/rules.d/99-brother-hl5170dn.rules`
- Creates the `printapp` system user in the `lp` group (no home dir, no login shell)
- Reloads udev rules and runs `systemctl daemon-reload`

Safe to re-run after a rebuild or re-image.

> **Note:** The printer's USB URI is hard-coded in `src/main.c` (`DEVICE_URI`) with the serial number of this specific unit (`L4J624176`). If you're using a different HL-5170DN, update that constant before building.

## Web UI

After the service starts, the admin UI is at:

```
http://tuttle-pi.local:8000/
```

The `/` page shows printer status and job history. Per-printer log level is adjustable from the UI.

> **Known limitation:** The live log viewer at `/logs` requires PAPPL to write to a file rather than stderr. The service currently logs to stderr (captured by journald); the web log viewer may not populate. Use `journalctl` instead (see [Logging](#logging)).

## Logging

All output goes to the system journal:

```bash
journalctl -u hl5170dn-printer-app -o cat -f             # follow live
journalctl -u hl5170dn-printer-app -o cat --since today  # today's output
```

`-o cat` shows only the app's own log lines (level + timestamp + message) without
journalctl's metadata prefix. For interactive paging, set `SYSTEMD_LESS=FRXMK`
to get a readable less experience.

Log level defaults to DEBUG. The admin UI's printer page lets you change it per-printer without restarting the service.

## Configuration

### Loaded paper

The printer knows what paper is physically in the tray via a vendor option. Default is Letter.

```bash
# Tell the printer app A4 is loaded
lp -d hl5170dn -o loaded-paper=iso_a4_210x297mm /dev/null

# Revert to Letter
lp -d hl5170dn -o loaded-paper=na_letter_8.5x11in /dev/null
```

Or set it persistently from the web UI (Printer → Driver settings → Loaded paper).

### Media substitution

When a client requests a media size that doesn't match the loaded paper, the default behaviour is silent substitution: the content is scaled to fit the loaded paper and a log entry is written. To instead reject mismatched jobs:

```bash
lp -d hl5170dn -o media-mismatch-action=reject /dev/null
```

Envelope sizes (DL, C5, Com10, Monarch, ISO-B5) are never coerced and always pass through unchanged.

## Print options

| Option | IPP attribute | Values | Default |
|--------|--------------|--------|---------|
| Resolution | `printer-resolution` | `300dpi`, `600dpi` | `600dpi` |
| Duplex | `sides` | `one-sided`, `two-sided-long-edge`, `two-sided-short-edge` | `one-sided` |
| Print quality | `print-quality` | `3` (draft/econo), `4` (normal), `5` (high/APT) | `4` |
| Input tray | `media-source` | `tray-1`, `by-pass-tray`, `auto` | `auto` |
| Media type | `media-type` | `stationery`, `stationery-lightweight`, `stationery-heavyweight`, `stationery-bond`, envelope variants | `stationery` |
| Copies | `copies` | integer | `1` |

`print-quality=5` (High) routes to the APT photo path (printer-side halftoning at 150 dpi input via TIFF/Mode-1024). `print-quality=4` uses driver-side ordered dither at 600 dpi. See [plan.md §Phase 6A](plan.md) for APT status.

Example:
```bash
lp -d hl5170dn -o sides=two-sided-long-edge -o print-quality=4 document.pdf
lp -d hl5170dn -o print-quality=5 photo.pdf   # APT path
```

## Comparison with the CUPS baseline

The CUPS + shell-filter + Ghostscript `ljet4` pipeline that preceded this rewrite is preserved at git tag `cups-filter-baseline` (`4602cfc`).

**Improved:**
- **AirPrint photo prints from iOS** — the CUPS baseline stalled indefinitely (USB backend waiting while the filter blocked on a full pipe). The PAPPL driver owns the USB device and streams raster incrementally; the stall is eliminated.
- **A4/Letter media substitution** — CUPS filters can't rewrite media at the IPP attribute resolution step. This driver does it before Ghostscript is invoked, so the raster dimensions match the substituted media.
- **Web admin UI** — `http://pi.local:8000/` with live job queue, printer status, and adjustable log level. CUPS offered none of this without a separate admin tool.
- **Image quality** — 8-bit grayscale raster kept intact through the pipeline; driver-side ordered dither at 600 dpi; APT (printer-side halftoning) available at `print-quality=5`.

**Unchanged:**
- USB-only transport. Using `socket://...:9100` (Ethernet) would bypass the interesting USB back-channel work and the keepalive risk that motivated the streaming architecture.
- Render speed on Pi 3B+ for photo-heavy PDFs (~45 s at 600 dpi). Streaming mitigates the USB keepalive risk but doesn't make Ghostscript faster. A Pi 5 or a laptop running the printer app locally renders 5–10× faster.

**Regressions:** to be evaluated after Phase 2 and Phase 3 physical print verification. No regressions observed in bench testing.

## Troubleshooting

**Port 8000 already in use:**
```bash
ss -tlnp | grep 8000      # identify the holder
sudo systemctl restart hl5170dn-printer-app
```
Only arises if a second instance was started manually alongside the managed service.

**Printer not found / USB permission denied:**
```bash
lsusb | grep -i brother    # confirm kernel sees the device
ls -la /dev/bus/usb/...    # check GROUP=lp MODE=0660
id printapp                # confirm printapp is in lp group
```
If the udev rule isn't applied, unplug and replug the printer or run:
```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```

**Confirm which PAPPL version the binary uses:**
```bash
ldd /usr/local/bin/hl5170dn-printer-app | grep pappl
# expect /usr/local/lib/libpappl.so.1 — NOT /usr/lib/.../libpappl1t64.so
```

## Reference material

| File | Purpose |
|------|---------|
| [plan.md](plan.md) | Implementation plan and phase status |
| [PRD-printer-applicance-rewrite-hl5170dn-pappl-driver.md](PRD-printer-applicance-rewrite-hl5170dn-pappl-driver.md) | Requirements spec |
| [bring-up-notes.md](bring-up-notes.md) | PAPPL API gotchas encountered during bring-up |
| [Tech_Manual_Ch2_PCL.pdf](Tech_Manual_Ch2_PCL.pdf) | Brother PCL reference; §6.3.8 = APT Mode 1024 |
| [Tech_Manual_Ch5_PJL.pdf](Tech_Manual_Ch5_PJL.pdf) | Brother PJL reference; source of all `@PJL SET` commands |
| [legacy/](legacy/) | The CUPS-filter baseline (also at git tag `cups-filter-baseline` `4602cfc`) |
