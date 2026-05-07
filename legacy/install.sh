#!/bin/bash
#
# install.sh - Install Brother HL-5170DN PCL/PJL driver
#
# Detects host OS and runs the appropriate install path:
#
#   Linux  (Pi or any Linux laptop with the printer attached via USB):
#     sudo bash install.sh
#       Installs cups + cups-filters + ghostscript + avahi, drops the
#       filter into /usr/lib/cups/filter, the full PPD into
#       /usr/share/cups/model, runs paperconfig -p letter, auto-discovers
#       the Brother USB device URI via lpinfo, adds (or updates) a
#       shared queue, and pins sensible defaults (Letter, 600dpi,
#       duplex long-edge).
#
#   Darwin (macOS, with the actual printer hosted on a Pi):
#     sudo bash install.sh
#       Installs a Mac-variant of the PPD into /Library/Printers/PPDs.
#       The Mac PPD has its *cupsFilter: lines stripped because the CUPS
#       filter sandbox on macOS blocks Homebrew's gs from loading dylibs
#       outside /usr/lib (Apple cups issue #4508). With no cupsFilter
#       lines, CUPS uses Apple's built-in pstops chain to ship PostScript
#       to the Pi via IPP, and the Pi (which has the full PPD + filter)
#       does the rendering. Custom options (TonerSave / Resolution /
#       Duplex / etc.) still appear in the Mac print dialog because the
#       PPD's OpenUI declarations are intact, and they're forwarded to
#       the Pi via IPP attributes.
#
#       For laptop-side rendering of large jobs (where the Pi 3B+ is the
#       bottleneck), use the `fastprint` shell function — see README
#       "Big jobs: render on Mac, send raw to Pi". That path runs gs
#       from Terminal (no sandbox) and submits raw PCL via lp -o raw.
#
# Optional environment variables (Linux):
#   QUEUE_NAME    Queue name to create (default: HL5170DN)
#   PRINTER_URI   Override the auto-discovered USB device URI
#   SKIP_QUEUE    Set to 1 to skip queue creation (driver-only install)
#
# Optional environment variables (macOS):
#   QUEUE_NAME    Mac-side queue name to create (default: HL5170DN)
#   PI_HOSTNAME   Pi hostname/IP — when set, auto-adds the queue pointing
#                 at ipp://<PI_HOSTNAME>:631/printers/<PI_QUEUE>. Without
#                 it, install.sh just lays down the PPD and prints
#                 instructions for adding the queue manually.
#   PI_QUEUE      Pi-side queue name (default: HL5170DN)
#
# Idempotent: safe to re-run after a re-image. Will update an existing
# queue's URI and PPD rather than fail.
#
set -euo pipefail

#---- Common -----------------------------------------------------------

# Resolve sibling files relative to this script so the installer works
# whether invoked from the repo root (`bash legacy/install.sh`), from
# inside legacy/ (`bash install.sh`), or with an absolute path.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FILTER_SRC="$SCRIPT_DIR/brother-hl5170dn-pjl"
PPD_SRC="$SCRIPT_DIR/Brother-HL5170DN-PCL.ppd"
QUEUE_NAME="${QUEUE_NAME:-HL5170DN}"

#---- Linux (Pi or Linux laptop) ---------------------------------------

install_linux() {
    local FILTER_DST="/usr/lib/cups/filter/brother-hl5170dn-pjl"
    local PPD_DST="/usr/share/cups/model/Brother-HL5170DN-PCL.ppd"

    # Old paths from the HL-5070N-named release; remove if present so the
    # orphaned filter doesn't get picked up by stale queue PPDs.
    local OLD_FILTER_DST="/usr/lib/cups/filter/brother-hl5070n-pjl"
    local OLD_PPD_DST="/usr/share/cups/model/Brother-HL5070N-PCL.ppd"

    echo "==> Installing Brother HL-5170DN PCL/PJL CUPS driver — Linux"

    # Dependencies
    # - cups-filters provides pwgtoraster and rastertopdf for AirPrint path
    # - libpaper-utils provides paperconfig (system paper-size default)
    echo "--> Checking dependencies..."
    apt-get install -y cups cups-filters avahi-daemon ghostscript libpaper-utils

    # Raspberry Pi OS ships with /etc/papersize=a4 (UK locale defaults).
    # Set it to letter so CUPS advertises Letter as the queue's media-default
    # over IPP/AirPrint. The filter also coerces A4→Letter at render time as
    # a defense in depth, but advertising the right default avoids the
    # coercion path for AirPrint clients in the first place.
    if command -v paperconfig >/dev/null 2>&1; then
        echo "--> Setting system papersize to letter (paperconfig)..."
        paperconfig -p letter || true
    elif [ -w /etc/papersize ]; then
        echo "--> Setting /etc/papersize to letter..."
        echo letter > /etc/papersize
    fi

    # Clean up any orphaned files from the previous HL-5070N name
    for f in "$OLD_FILTER_DST" "$OLD_PPD_DST"; do
        if [ -e "$f" ]; then
            echo "--> Removing orphaned: $f"
            rm -f "$f"
        fi
    done

    echo "--> Installing filter: $FILTER_DST"
    install -m 755 -o root -g root "$FILTER_SRC" "$FILTER_DST"

    echo "--> Installing PPD: $PPD_DST"
    install -m 644 -o root -g root "$PPD_SRC" "$PPD_DST"

    echo "--> Configuring CUPS for network sharing..."
    cupsctl --share-printers --remote-any

    echo "--> Enabling avahi-daemon..."
    systemctl enable avahi-daemon
    systemctl start avahi-daemon

    echo "--> Restarting CUPS..."
    systemctl restart cups

    # Wait briefly for CUPS to come back up
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if lpstat -r >/dev/null 2>&1; then break; fi
        sleep 1
    done

    if [ "${SKIP_QUEUE:-0}" = "1" ]; then
        echo "--> SKIP_QUEUE=1, skipping queue setup."
    else
        echo "--> Discovering printer USB URI..."

        local URI
        if [ -n "${PRINTER_URI:-}" ]; then
            URI="$PRINTER_URI"
            echo "    Using PRINTER_URI from environment: $URI"
        else
            # lpinfo lines: direct usb://Brother/HL-5170DN%20series?serial=...
            # Match the family (HL-50xxN / HL-51xxDN); same PJL/PCL vocab.
            URI=$(lpinfo -v 2>/dev/null \
                | grep -i 'usb://Brother/HL-5' \
                | head -1 \
                | awk '{print $2}')

            if [ -z "$URI" ]; then
                echo ""
                echo "!! No Brother USB printer detected via lpinfo."
                echo "   - Make sure the printer is plugged in via USB and powered on."
                echo "   - List visible devices with:  sudo lpinfo -v"
                echo "   - Then re-run, or pass the URI explicitly:"
                echo "       sudo PRINTER_URI='usb://Brother/...' bash install.sh"
                echo "   - Or skip the queue step and add it manually:"
                echo "       sudo SKIP_QUEUE=1 bash install.sh"
                echo "       sudo lpadmin -p $QUEUE_NAME -E -v <uri> -P $PPD_DST"
                echo ""
                exit 1
            fi
            echo "    Detected: $URI"
        fi

        if lpstat -p "$QUEUE_NAME" >/dev/null 2>&1; then
            echo "--> Queue '$QUEUE_NAME' already exists; updating URI and PPD."
        else
            echo "--> Adding queue '$QUEUE_NAME'..."
        fi
        lpadmin -p "$QUEUE_NAME" -E -v "$URI" -P "$PPD_DST"

        echo "--> Marking queue shared (for AirPrint and other LAN clients)..."
        lpadmin -p "$QUEUE_NAME" -o printer-is-shared=true

        echo "--> Setting queue defaults..."
        lpadmin -p "$QUEUE_NAME" -o TonerSave-default=OFF
        lpadmin -p "$QUEUE_NAME" -o Resolution-default=600dpi
        lpadmin -p "$QUEUE_NAME" -o Duplex-default=DuplexNoTumble
        lpadmin -p "$QUEUE_NAME" -o PageSize-default=Letter
        lpadmin -p "$QUEUE_NAME" -o media-default=letter
    fi

    echo ""
    echo "==> Done (Linux)."
    if [ "${SKIP_QUEUE:-0}" != "1" ]; then
        echo ""
        echo "    Queue:   $QUEUE_NAME (shared)"
        lpstat -p "$QUEUE_NAME" 2>/dev/null | sed 's/^/    /' || true
        echo ""
        echo "    Smoke test:"
        echo "      echo 'hello world' | lp -d $QUEUE_NAME"
        echo ""
        echo "    AirPrint discovery from another device:"
        echo "      avahi-browse -rt _ipp._tcp        # on Linux"
        echo "      dns-sd -B _ipp._tcp local.        # on macOS"
    fi
    echo ""
    echo "    Mac clients: run this same install.sh on the Mac."
    echo "    Re-running is safe; it updates the existing install."
    echo ""
}

#---- macOS (printer hosted on a Pi via IPP) --------------------------

install_macos() {
    local PPD_DIR="/Library/Printers/PPDs/Contents/Resources"
    local PPD_DST="$PPD_DIR/Brother-HL5170DN-PCL.ppd"
    local PI_QUEUE="${PI_QUEUE:-HL5170DN}"

    echo "==> Installing Brother HL-5170DN PCL/PJL driver — macOS"
    echo "    PPD-only install. Pi handles all rendering. (CUPS filter"
    echo "    sandbox on macOS blocks host-side Ghostscript — Apple cups"
    echo "    issue #4508. For laptop-side rendering of big jobs, use"
    echo "    the fastprint function — see README.)"

    # Verify Ghostscript (only required for the optional fastprint flow).
    if command -v gs >/dev/null 2>&1; then
        echo "--> Ghostscript found at $(command -v gs) — fastprint will work."
    else
        echo "--> Ghostscript not found. To enable the optional fastprint"
        echo "    function for big jobs, install via: brew install ghostscript"
    fi

    # Install the Mac-variant PPD: source minus the *cupsFilter: lines.
    # CUPS without a cupsFilter for application/postscript falls back to
    # Apple's built-in pstops chain, which produces PostScript and ships
    # it to the Pi via the IPP backend. The Pi's full PPD (with the
    # filter) handles the actual PCL conversion. Custom options remain
    # functional because their OpenUI declarations stay in the PPD; CUPS
    # forwards their values to the Pi as IPP attributes.
    echo "--> Installing Mac PPD (cupsFilter lines stripped): $PPD_DST"
    mkdir -p "$PPD_DIR"
    sed '/^\*cupsFilter:/d' "$PPD_SRC" > "$PPD_DST"
    chmod 644 "$PPD_DST"

    # Verify the strip actually removed the filter lines
    if grep -q '^\*cupsFilter:' "$PPD_DST"; then
        echo "!! Warning: cupsFilter lines still present in installed PPD."
        echo "   Continuing, but jobs may try to invoke brother-hl5170dn-pjl"
        echo "   on the Mac and fail with 'Filter failed'."
    fi

    if [ -n "${PI_HOSTNAME:-}" ]; then
        echo "--> Auto-adding queue '$QUEUE_NAME' pointing at $PI_HOSTNAME"
        lpadmin -p "$QUEUE_NAME" -E \
            -v "ipp://${PI_HOSTNAME}:631/printers/${PI_QUEUE}" \
            -P "$PPD_DST"

        # Mac-side defaults (forwarded to the Pi via IPP per-job attributes)
        lpadmin -p "$QUEUE_NAME" -o Resolution-default=600dpi
        lpadmin -p "$QUEUE_NAME" -o Duplex-default=DuplexNoTumble
        lpadmin -p "$QUEUE_NAME" -o PageSize-default=Letter

        # Set as user default so app print dialogs pick it up
        if [ -n "${SUDO_USER:-}" ]; then
            sudo -u "$SUDO_USER" lpoptions -d "$QUEUE_NAME" 2>/dev/null || true
        fi

        echo ""
        echo "==> Done (macOS)."
        echo ""
        echo "    Queue:   $QUEUE_NAME"
        lpstat -p "$QUEUE_NAME" 2>/dev/null | sed 's/^/    /' || true
        echo "    Backend: ipp://${PI_HOSTNAME}:631/printers/${PI_QUEUE}"
        echo ""
        echo "    Smoke test:"
        echo "      echo 'hello world' | lp -d $QUEUE_NAME"
    else
        echo ""
        echo "==> Done (macOS) — PPD installed, queue not auto-added."
        echo ""
        echo "    Add the printer via the GUI:"
        echo "      System Settings → Printers & Scanners → Add Printer"
        echo "      → IP tab → Protocol: IPP"
        echo "      → Address:  <pi-hostname>.local"
        echo "      → Queue:    printers/$PI_QUEUE"
        echo "      → Use:      Other... → $PPD_DST"
        echo ""
        echo "    Or via command line (substitute your Pi's hostname):"
        echo "      sudo lpadmin -p $QUEUE_NAME -E \\"
        echo "          -v 'ipp://<pi-hostname>.local:631/printers/$PI_QUEUE' \\"
        echo "          -P '$PPD_DST'"
        echo ""
        echo "    Or re-run install.sh with PI_HOSTNAME set to auto-add:"
        echo "      sudo PI_HOSTNAME=<pi-hostname>.local bash install.sh"
    fi
    echo ""
    echo "    For big jobs, see README 'Big jobs: render on Mac' for the"
    echo "    fastprint shell function."
    echo ""
}

#---- Dispatcher -------------------------------------------------------

OS=$(uname -s)
case "$OS" in
    Linux)
        install_linux
        ;;
    Darwin)
        install_macos
        ;;
    *)
        echo "!! Unsupported OS: $OS"
        echo "   Supported: Linux (Pi / Linux laptop) and Darwin (macOS)."
        exit 1
        ;;
esac
