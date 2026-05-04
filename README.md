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
incoming jobs to PCL 5e on the Pi (or on the client machine directly for
configured hosts), and PJL commands in the job header control printer
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

# Set queue-wide defaults (substitute your queue name for HL5070N)
sudo lpadmin -p HL5070N -o TonerSave-default=OFF
sudo lpadmin -p HL5070N -o Resolution-default=600dpi
sudo lpadmin -p HL5070N -o Duplex-default=DuplexNoTumble
```

**Use `lpadmin -o KEY-default=VALUE`, not `lpoptions -o KEY=VALUE`.** They look
similar but `lpoptions` writes to `/etc/cups/lpoptions` (or `~/.cups/lpoptions`)
which takes precedence over the queue default. See "CUPS option precedence"
below.

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
- `cups-filters` — provides `pwgtoraster` and `rastertopdf` used by the AirPrint
  PWG-raster path
- `avahi-daemon` — Bonjour/mDNS advertisement for AirPrint
- `ghostscript` — PDF/PostScript/PWG Raster → PCL conversion

## Reference Sources

| Document | URL | Notes |
|----------|-----|-------|
| Brother PCL/PJL Technical Reference Manual | https://download.brother.com/welcome/doc002907/Tech_Manual_AD.pdf | PJL environment variables including `ECONOMODE`, `ECONOLEVEL`, `RESOLUTION`, `DUPLEX`, `BINDING`. Primary source for all PJL commands used in the filter. Also on ManualsLib: search "Brother PCL Technical Reference" |
| Brother HL-5070N driver downloads | https://support.brother.com (search HL-5070N, select Linux, deb) | LPR deb, CUPSwrapper deb, CUPSwrapper source tarball, BR-Script PPD |
| CUPSwrapper source | `brother-laser1-src-1_0_2-1_tar.gz` from above | Contains original PostScript-based PPD at `SRC/PARTS/cupswrapperHL5070N-1.0.2` — used as basis for paper sizes, imageable areas, and option names in this PPD |
| OpenPrinting database | https://openprinting.org (search HL-5070N) | Lists "PCL6" and BR-Script3. The Brother tech manual confirms "PCL6" here means PCL 6 Standard (= PCL 5e), not PCL XL — no `ENTER LANGUAGE=PCLXL` examples appear anywhere in the manual. |
| CUPS PPD spec | https://www.cups.org/doc/spec-ppd.html | PPD 4.3 format reference |
| Ghostscript ljet4 device | https://ghostscript.com/docs/9.54.0/Devices.htm | PCL 5e output device used for raster conversion |

## CUPS option precedence

When CUPS resolves an option value for a job, it walks this list and uses
the first hit:

1. Per-job options on the command line (`lp -o Duplex=None …`)
2. The submitting user's `~/.cups/lpoptions`
3. The system-wide `/etc/cups/lpoptions` (written by `sudo lpoptions -o …`)
4. The `Option KEY VALUE` lines for the queue in `/etc/cups/printers.conf`
   (written by `sudo lpadmin -p NAME -o KEY-default=VALUE`)
5. The PPD's `*Default<Key>: …` line

The trap: `sudo lpoptions -p NAME -o Duplex=None` and
`sudo lpadmin -p NAME -o Duplex-default=None` both look like they set
"the queue default", but `lpoptions` writes a system-wide override at
level 3 that beats anything `lpadmin` puts at level 4. Always use
`lpadmin -o KEY-default=VALUE` for queue defaults.

Diagnosing a stuck default: the truth lives in the filter trace under
`Resolved: …` (see Troubleshooting below), not in `lpoptions -l` output.
If the filter sees the wrong value, inspect the lookup chain in order:

```bash
cat ~/.cups/lpoptions 2>/dev/null
sudo cat /etc/cups/lpoptions 2>/dev/null
sudo awk '/^<Printer NAME>/,/^<\/Printer>/' /etc/cups/printers.conf
```

## Troubleshooting

- **See what the filter is doing.** The filter logs DEBUG-prefixed lines, but
  CUPS's default `LogLevel warn` doesn't capture them. Turn on debug logging
  before reproducing the issue:

  ```bash
  sudo cupsctl --debug-logging
  # ... print a job ...
  grep brother-hl5070n /var/log/cups/error_log | tail -30
  sudo cupsctl --no-debug-logging
  ```

  Toggling `cupsctl` triggers a CUPS reconfigure that briefly takes the queue
  offline. If `lp` returns "The printer or class does not exist" right after
  `cupsctl --debug-logging`, wait a couple of seconds and retry — your `lp`
  command landed in the reload window.

- **Confirm the active filter chain.** If the trace shows nothing at all, the
  queue may still be using the previous PPD/driver:

  ```bash
  grep cupsFilter /etc/cups/ppd/<queue>.ppd
  ```

  Should list four lines pointing at `brother-hl5070n-pjl`. If not,
  `sudo lpadmin -p <queue> -P /usr/share/cups/model/Brother-HL5070N-PCL.ppd`
  followed by `sudo systemctl restart cups`.

## Caveats

- HQ1200 mode (2400×600) will be slow to render on the Pi for complex pages;
  for best performance configure clients to send PCL directly.
- The filter sets paper, tray, media type, and copies via `@PJL SET`
  (with `LPARM : PCL` for `PAPER` per Brother manual Ch5 §2). PJL settings
  persist across the PCL `<ESC>E` reset that begins Ghostscript's `ljet4`
  output, so they remain in effect for the actual print data. If a setting
  doesn't take effect, check `/var/log/cups/error_log` for the resolved
  option values (see Troubleshooting).
- Brother's LPR binary driver is not used or required.
