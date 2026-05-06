# Brother HL-5170DN PCL/PJL CUPS Driver

A host-side rendering CUPS driver for the Brother HL-5170DN laser printer,
designed to run on a Raspberry Pi 3B+ as a WiFi print server.

## The Problem

The HL-5170DN's built-in BR-Script (PostScript clone) interpreter runs on a
slow onboard CPU. The standard Brother CUPS driver sends PostScript and lets
the printer RIP it, causing a long pause before the first page prints.

## The Solution

This driver flips the rendering workload to the host. Ghostscript converts
incoming jobs to PCL 5e on the Pi (or on the client machine directly for
configured hosts), and PJL commands in the job header control printer
features without going through BR-Script at all.

```
Mac/Linux (configured)          Raspberry Pi 3B+           HL-5170DN
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
| `Brother-HL5170DN-PCL.ppd` | PPD with toner save, resolution, duplex, media type, tray options |
| `brother-hl5170dn-pjl` | CUPS filter: detects input type, converts via Ghostscript, prepends PJL header |
| `install.sh` | OS-aware installer. Linux: deps + filter + PPD + sharing + auto-discovered queue. macOS: Mac-variant PPD (cupsFilter lines stripped) + optional auto-add queue pointing at the Pi. |

## Pi Setup

Plug the printer in via USB, power it on, then on the Pi:

```bash
sudo bash install.sh
```

That installs cups + cups-filters + ghostscript + avahi + libpaper-utils,
drops the filter and PPD into the standard CUPS paths, runs
`paperconfig -p letter`, auto-discovers the Brother USB device URI via
`lpinfo`, adds (or updates) a shared queue named `HL5170DN`, and pins
defaults: Letter paper, 600dpi, duplex long-edge, toner save off.
Idempotent — safe to re-run after re-imaging.

To override the queue name or USB URI:

```bash
sudo QUEUE_NAME=myprinter PRINTER_URI='usb://Brother/...' bash install.sh
```

**On queue defaults: use `lpadmin -o KEY-default=VALUE`, not
`lpoptions -o KEY=VALUE`.** They look similar but `lpoptions` writes to
`/etc/cups/lpoptions` (or `~/.cups/lpoptions`) which takes precedence
over the queue default. See "CUPS option precedence" below.

## Mac Setup

`install.sh` handles the Mac side too. From a clone of this repo on the
Mac:

```bash
sudo PI_HOSTNAME=<pi-hostname>.local bash install.sh
```

That installs a Mac-variant of the PPD into `/Library/Printers/PPDs/Contents/Resources/`
and adds a local queue pointing at the Pi via IPP. Without `PI_HOSTNAME`,
it just installs the PPD and prints instructions for adding the queue
manually via the GUI or `lpadmin`.

**Why a Mac variant.** The PPD installed on the Mac has its
`*cupsFilter:` lines stripped. The CUPS filter sandbox on macOS blocks
Homebrew's `gs` from loading dylibs outside `/usr/lib` (Apple's
`cups#4508`), so trying to run `brother-hl5170dn-pjl` on the Mac
ends in "Filter failed". With no cupsFilter line, CUPS uses Apple's
built-in `pstops` chain to ship PostScript to the Pi via IPP, and
the Pi's full PPD + filter handles PCL conversion — sandbox-free,
because Linux doesn't sandbox CUPS that way. The Mac print dialog
still shows Toner Save / Resolution / Duplex / Media Type / Input
Slot because their `*OpenUI` declarations stay in the PPD; CUPS
forwards their values to the Pi as IPP attributes per job.

### GUI alternatives

If you'd rather avoid the script:

**Recommended: IP tab.** System Settings → Printers & Scanners → Add
Printer (`+`) → **IP** tab.

- **Address:** Pi's hostname (`pi.local` style) or its IP
- **Protocol:** Internet Printing Protocol — IPP
- **Queue:** `printers/<your-queue-name>`
- **Use:** "Other…" → select the Mac PPD at
  `/Library/Printers/PPDs/Contents/Resources/Brother-HL5170DN-PCL.ppd`
  (drop in via `install.sh` first). "Generic PostScript Printer" also
  works but loses the printer-specific options dropdown.

**Alternative: Bonjour Shared.** Pick the Bonjour-shared entry on the
first tab. Earlier macOS versions had a bug where the local queue
inherited the Pi's `usb://` URI; if `lpstat -v <queue>` shows a `usb://`
URI on a Mac without that printer connected, fix it with:

```bash
sudo lpadmin -p <local-queue-name> -v \
    ipp://<pi-hostname>.local:631/printers/<pi-queue-name>
```

## Big jobs: render on Mac, send raw to Pi

The Pi 3B+ is the rendering bottleneck for image-heavy or HQ1200 pages
(see `plan.md` for the bitmap-throughput math). For those jobs you can
render PDF → PCL on the Mac CPU and ship the already-rendered bytes
straight through the Pi via `lp -o raw`. The Pi's filter detects PCL
input, skips Ghostscript entirely, just wraps with the PJL header, and
streams to USB. Mac CPU does the heavy lifting; Pi is a passthrough
PJL wrapper.

Why not let the Mac's CUPS run the filter locally? Because of the
sandbox issue described in "Mac Setup". Rendering from a Terminal-spawned
`gs` sidesteps the sandbox entirely — the Terminal isn't sandboxed —
and `lp -o raw` skips the local filter chain.

Drop this into your `~/.zshrc` (or `~/.bashrc`):

```bash
# Render PDF → PCL on the Mac CPU, send raw to the Pi (no Mac-side filter)
fastprint() {
    if [ -z "${1:-}" ]; then
        echo "usage: fastprint <pdf> [more pdfs...]" >&2
        return 1
    fi
    local queue="${FASTPRINT_QUEUE:-HL5170DN}"
    local resolution="${FASTPRINT_RES:-600}"
    local paper="${FASTPRINT_PAPER:-letter}"
    local gs="${FASTPRINT_GS:-$(command -v gs)}"
    local pcl
    for pdf in "$@"; do
        pcl=$(mktemp -t fastprint).pcl
        "$gs" -q -dBATCH -dNOPAUSE -dSAFER \
            -sDEVICE=ljet4 -r"$resolution" -sPAPERSIZE="$paper" \
            -dFIXEDMEDIA -dPDFFitPage \
            -sOutputFile="$pcl" "$pdf" \
            && lp -d "$queue" -o raw "$pcl" \
            || echo "fastprint: failed on $pdf" >&2
        rm -f "$pcl"
    done
}
```

Usage:

```bash
fastprint big-photo-document.pdf
FASTPRINT_QUEUE=HL5170DN___tuttle_pi fastprint slides.pdf
FASTPRINT_RES=300 fastprint draft.pdf      # half resolution, ~4× smaller PCL
FASTPRINT_PAPER=a4 fastprint euro-form.pdf
```

For a one-off without the function, the same logic inline:

```bash
gs -q -dBATCH -dNOPAUSE -sDEVICE=ljet4 -r600 -sPAPERSIZE=letter \
   -dFIXEDMEDIA -dPDFFitPage -sOutputFile=/tmp/job.pcl input.pdf \
&& lp -d HL5170DN -o raw /tmp/job.pcl
```

What you give up: the macOS print dialog. fastprint is command-line
only — no per-document margins, page-range selection, multi-up, etc.
Use the regular print dialog (which routes through the Pi) for everyday
text printing. Reach for fastprint only when you've watched the Pi
struggle with a particular job and want the Mac's CPU to take over.

## Direct USB Setup (no Pi)

When the laptop with the printer attached is the only device printing — or
when you only need to share with a small number of clients on the same
machine — skip the Pi entirely. The same filter runs locally; rendering
happens on the laptop CPU (much faster than a Pi 3B+ for HQ1200 or
image-heavy pages), and the WiFi → IPP → USB hop is gone. First-page-out
latency drops noticeably.

### On a Linux laptop

```bash
# Plug the printer into a USB port on the laptop, then:

# Dependencies
sudo apt-get install -y cups cups-filters ghostscript

# Install the filter and PPD
sudo install -m 755 brother-hl5170dn-pjl /usr/lib/cups/filter/
sudo install -m 644 Brother-HL5170DN-PCL.ppd /usr/share/cups/model/

# Find the USB device URI
sudo lpinfo -v | grep -i brother
# Example output:
#   direct usb://Brother/HL-5170DN%20series?serial=E5XXXXX

# Add the queue (paste the URI from above)
sudo lpadmin -p HL5170DN -E \
    -v 'usb://Brother/HL-5170DN%20series?serial=E5XXXXX' \
    -P /usr/share/cups/model/Brother-HL5170DN-PCL.ppd

# Queue defaults (use lpadmin, NOT lpoptions — see "CUPS option precedence")
sudo lpadmin -p HL5170DN -o TonerSave-default=OFF
sudo lpadmin -p HL5170DN -o Resolution-default=600dpi
sudo lpadmin -p HL5170DN -o Duplex-default=DuplexNoTumble

# Smoke test
echo "hello" | lp -d HL5170DN
```

If the submitting user isn't in the `lp` group (or `lpadmin` on some
distros), add them: `sudo usermod -aG lp $USER` then re-login.

### On a Mac laptop

**Constrained by the macOS CUPS filter sandbox.** Unlike Linux, macOS
runs every CUPS filter inside a sandbox profile that blocks `dlopen()`
of dylibs outside `/usr/lib`. Homebrew's `gs` lives at
`/opt/homebrew/bin/gs` (or `/usr/local/bin/gs` on Intel) and depends on
`/opt/homebrew/lib/libgs.dylib`, which the sandbox denies. Result:
"Filter failed" on every job. See Apple's `cups#4508`. Two ways
forward:

**Option A: Sandboxing Relaxed.** Apple-supported config knob in
`cups-files.conf` that loosens (not disables) the filter sandbox so
filters can spawn binaries and load dylibs from non-system paths.
After enabling, the install steps mirror the Linux laptop path above
with macOS-specific paths.

```bash
sudo sh -c 'echo "Sandboxing Relaxed" >> /etc/cups/cups-files.conf'
sudo launchctl kickstart -k system/org.cups.cupsd  # or reboot

# Then install the filter, PPD, gs symlink, and queue:
brew install ghostscript
sudo ln -sf /opt/homebrew/bin/gs /usr/local/bin/gs   # CUPS PATH includes /usr/local/bin
sudo install -m 755 brother-hl5170dn-pjl /usr/libexec/cups/filter/
sudo install -m 644 Brother-HL5170DN-PCL.ppd /Library/Printers/PPDs/Contents/Resources/

sudo lpinfo -v | grep -i brother    # find usb://Brother/... URI
sudo lpadmin -p HL5170DN -E \
    -v 'usb://Brother/HL-5170DN%20series?serial=...' \
    -P /Library/Printers/PPDs/Contents/Resources/Brother-HL5170DN-PCL.ppd
```

Trade-off: this is a security relaxation across the entire CUPS stack
on the Mac, not just for our printer. Reasonable for a personal
machine, often disallowed by corporate MDM policy, and recent macOS
versions have shown signs of breaking this workaround entirely (per
Apple Developer Forums on Tahoe 26.2). Verify with `sw_vers` and check
your IT policy before committing.

**Option B: Skip the filter on the Mac.** Install the Mac-variant PPD
(no `*cupsFilter:` lines) so the Mac's CUPS doesn't try to invoke the
filter at all, then submit pre-rendered PCL via `lp -o raw` for any job
you actually want to print. This is the same shape as the
"Big jobs: render on Mac" section above but with the printer attached
locally instead of via the Pi.

```bash
brew install ghostscript

# Strip cupsFilter lines (same trick install.sh does for Pi-via-Mac)
sed '/^\*cupsFilter:/d' Brother-HL5170DN-PCL.ppd \
    | sudo tee /Library/Printers/PPDs/Contents/Resources/Brother-HL5170DN-PCL.ppd >/dev/null

# Add a USB-backed queue using the stripped PPD
sudo lpinfo -v | grep -i brother
sudo lpadmin -p HL5170DN -E \
    -v 'usb://Brother/HL-5170DN%20series?serial=...' \
    -P /Library/Printers/PPDs/Contents/Resources/Brother-HL5170DN-PCL.ppd
```

Then a `fastprint-usb` variant of the function above that drops the
`-d <queue>` step in favor of the same local queue:

```bash
fastprint-usb() {
    local queue="${FASTPRINT_QUEUE:-HL5170DN}"
    local resolution="${FASTPRINT_RES:-600}"
    local pcl
    pcl=$(mktemp -t fastprint-usb).pcl
    /opt/homebrew/bin/gs -q -dBATCH -dNOPAUSE -sDEVICE=ljet4 \
        -r"$resolution" -sPAPERSIZE=letter -dFIXEDMEDIA -dPDFFitPage \
        -sOutputFile="$pcl" "$1" \
    && lp -d "$queue" -o raw "$pcl"
    rm -f "$pcl"
}
```

GUI app print dialogs won't work because there's no working filter
chain, but command-line workflow is sound and you keep the sandbox
strict. Best fit for "I print rarely and don't mind a one-line shell
command for it."

**Recommendation:** if direct USB on a Mac is the actual constraint,
strongly consider just running a small Linux box (the Pi, a NUC, or
similar) as the print server — Linux's CUPS doesn't sandbox filters
the same way, and the architecture in this repo is built for that.

### Sharing the laptop's printer for AirPrint

Once direct USB is working on the laptop, you can also let phones, tablets,
and other laptops on the same WiFi print to it without any client setup —
the laptop now plays the role the Pi played in the original architecture.
CUPS + Bonjour advertise the queue as an AirPrint-compatible IPP printer,
and incoming PDF / PWG Raster jobs hit the same `brother-hl5170dn-pjl`
filter and convert to PCL on the host CPU.

#### From a Linux laptop

```bash
# cups-filters provides pwgtoraster/rastertopdf used by the filter's
# AirPrint path; avahi-daemon publishes the Bonjour record.
sudo apt-get install -y cups-filters avahi-daemon

# Enable CUPS network sharing and mark the queue as shared
sudo cupsctl --share-printers --remote-any
sudo lpadmin -p HL5170DN -o printer-is-shared=true

# Run avahi
sudo systemctl enable --now avahi-daemon

# Restart CUPS so the Bonjour record is (re)published
sudo systemctl restart cups
```

Open the firewall if you have one running: TCP 631 (IPP) and UDP 5353
(mDNS). On `ufw`: `sudo ufw allow 631/tcp && sudo ufw allow 5353/udp`.

#### From a Mac laptop

System Settings → General → Sharing → toggle on **Printer Sharing**, then
in the right-hand pane check the box next to `HL5170DN`. macOS handles
the Bonjour/AirPrint advertisement automatically.

If the Mac sleeps, the printer goes offline. To keep it reachable for
visitors, enable System Settings → Battery → Options → **Wake for network
access** (Apple Silicon) or System Settings → Energy Saver → **Wake for
network access** (Intel).

#### Verifying the advertisement

On either host, confirm the queue is being advertised correctly:

```bash
# Linux
avahi-browse -rt _ipp._tcp

# macOS
dns-sd -B _ipp._tcp local.
# Then dns-sd -L "HL5170DN @ host" _ipp._tcp local. for the TXT records
```

The TXT record should include `pdl=` listing `application/pdf` and
`image/pwg-raster` (and ideally `image/urf`). If `image/pwg-raster` is
missing, iOS clients will see the printer but fail to print — re-check
that `cups-filters` is installed and the queue is shared.

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
| Brother HL-5170DN driver downloads | https://support.brother.com (search HL-5170DN, select Linux, deb) | LPR deb, CUPSwrapper deb, CUPSwrapper source tarball, BR-Script PPD |
| CUPSwrapper source | `brother-laser1-src-1_0_2-1_tar.gz` from above | The PPD's paper sizes, imageable areas, and option names were initially derived from the HL-5070N CUPSwrapper at `SRC/PARTS/cupswrapperHL5070N-1.0.2` (kept as the historical source — the HL-5070N and HL-5170DN share these values per Brother's PJL manual) |
| OpenPrinting database | https://openprinting.org (search HL-5170DN) | Lists "PCL6" and BR-Script3. The Brother tech manual confirms "PCL6" here means PCL 6 Standard (= PCL 5e), not PCL XL — no `ENTER LANGUAGE=PCLXL` examples appear anywhere in the manual. |
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
  grep brother-hl5170dn /var/log/cups/error_log | tail -30
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

  Should list four lines pointing at `brother-hl5170dn-pjl`. If not,
  `sudo lpadmin -p <queue> -P /usr/share/cups/model/Brother-HL5170DN-PCL.ppd`
  followed by `sudo systemctl restart cups`.

- **macOS shows the printer as offline / Pi sees no IPP traffic.** When you
  add a printer on macOS via the "Bonjour Shared" entry in Printers & Scanners,
  macOS sometimes copies the *upstream* CUPS server's `device-uri` verbatim
  instead of constructing a fresh IPP URI. If the Pi has a USB-attached
  printer, the Mac's local queue ends up with `usb://Brother/...` as its
  device URI and tries to find the printer on the Mac's own USB ports — and
  fails immediately. Symptom: print dialog shows "The printer is offline" but
  `curl http://<pi>:631/` from the Mac responds 200, and the Pi's
  `/var/log/cups/access_log` has no entries from the Mac's IP.

  Diagnostic (on the Mac):
  ```bash
  lpstat -v <local-queue-name>
  ```

  If the device URI starts with `usb://` (and the printer isn't actually on
  the Mac's USB), fix it with:
  ```bash
  sudo lpadmin -p <local-queue-name> -v \
      ipp://<pi-hostname>.local:631/printers/<pi-queue-name>
  ```

  See "Mac Setup → Recommended: IP tab" above to avoid hitting this in the
  first place.

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
