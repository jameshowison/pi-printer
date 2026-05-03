# Brother HL-5070N PCL/PJL CUPS Driver

A host-side rendering CUPS driver for the Brother HL-5070N laser printer,
designed to run on a Raspberry Pi 3B+ as a WiFi print server.

## The Problem

The HL-5070N's built-in BR-Script (PostScript clone) interpreter runs on a
slow 133MHz onboard CPU with 16MB RAM. The standard Brother CUPS driver sends
PostScript and lets the printer RIP it, causing a long pause before the first
page prints.

## The Solution

This driver flips the rendering workload to the host. Ghostscript converts
incoming jobs to PCL XL raster on the Pi (or on the client machine directly
for configured hosts), and PJL commands in the job header control printer
features without going through BR-Script at all.

```
Mac/Linux (configured)          Raspberry Pi 3B+           HL-5070N
┌─────────────────────┐        ┌──────────────────┐        ┌──────────┐
│ PPD installed       │        │ CUPS + this       │        │          │
│ Renders to PCL      │─PCL──▶│ driver            │─PCL──▶│ Prints   │
│ Sends over network  │        │                   │  USB   │ fast     │
└─────────────────────┘        │ Ghostscript       │        └──────────┘
                                │ fallback for      │
AirPrint / visitors             │ PDF/PS/PWG Raster │
┌─────────────────────┐        │                   │
│ No setup needed     │─PDF──▶│ Converts → PCL    │
│ Standard print UI   │        │ Adds PJL header   │
└─────────────────────┘        └──────────────────┘
```

## Files

| File | Purpose |
|------|---------|
| `Brother-HL5070N-PCL.ppd` | PPD with toner save, resolution, duplex, media type, tray options |
| `brother-hl5070n-pjl` | CUPS filter: detects input type, converts via Ghostscript, prepends PJL header |
| `install.sh` | Sets up dependencies, installs driver, configures CUPS sharing on the Pi |

## Pi Setup

```bash
# On the Pi
sudo bash install.sh

# Add printer via http://localhost:631 → Administration → Add Printer
# Select: Brother-HL5070N-PCL.ppd

# Set defaults
lpoptions -p HL5070N -o TonerSave=OFF
lpoptions -p HL5070N -o Resolution=600dpi
lpoptions -p HL5070N -o Duplex=None
```

## Mac Setup (for full option control)

```bash
# Enable CUPS web interface
cupsctl WebInterface=yes

# Copy PPD from Pi
scp pi@<pi-ip>:/usr/share/cups/model/Brother-HL5070N-PCL.ppd ~/Downloads/
```

Then visit `http://localhost:631`, add the Pi printer, and select the PPD
manually under Other → browse to the downloaded file.

## Printer Options

| Option | Values | Default |
|--------|--------|---------|
| Resolution | 300dpi, 600dpi, HQ1200 | 600dpi |
| Toner Save | OFF, ON | OFF |
| Duplex | None, Long Edge, Short Edge | None |
| Media Type | Plain, Thin, Thick, Thicker, Bond, Envelope variants | Plain |
| Input Slot | Auto, MP Tray, Tray 1, Tray 2, Manual | Auto |

## AirPrint Visitors

No setup needed on visitor devices. Avahi advertises the printer via Bonjour.
The filter maps standard IPP attributes automatically:

- `print-quality=3` (Draft) → Toner Save ON
- `sides=two-sided-long-edge` → Duplex long edge

## Dependencies

- `cups` — print server and filter infrastructure
- `avahi-daemon` — Bonjour/mDNS advertisement for AirPrint
- `ghostscript` — PDF/PostScript/PWG Raster → PCL conversion

## Caveats

- HQ1200 mode (2400×600) will be slow to render on the Pi for complex pages;
  for best performance configure clients to send PCL directly.
- PJL header interaction with Ghostscript's own pxlmono output may need
  tuning — test with a simple document first and check for double-reset issues.
- Brother's LPR binary driver is not used or required.
