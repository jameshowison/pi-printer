#!/bin/bash
#
# install.sh - Install Brother HL-5170DN PCL/PJL driver on Raspberry Pi
#
# Run as root: sudo bash install.sh
#
# Optional environment variables:
#   QUEUE_NAME    Queue name to create (default: HL5170DN)
#   PRINTER_URI   Override the auto-discovered USB device URI
#   SKIP_QUEUE    Set to 1 to skip queue creation (driver-only install)
#
# Idempotent: safe to re-run after a re-image. Will update an existing
# queue's URI and PPD rather than fail.
#
set -euo pipefail

FILTER_SRC="brother-hl5170dn-pjl"
PPD_SRC="Brother-HL5170DN-PCL.ppd"
FILTER_DST="/usr/lib/cups/filter/brother-hl5170dn-pjl"
PPD_DST="/usr/share/cups/model/Brother-HL5170DN-PCL.ppd"

# Old paths from the HL-5070N-named release; remove if present so the
# orphaned filter doesn't get picked up by stale queue PPDs.
OLD_FILTER_DST="/usr/lib/cups/filter/brother-hl5070n-pjl"
OLD_PPD_DST="/usr/share/cups/model/Brother-HL5070N-PCL.ppd"

QUEUE_NAME="${QUEUE_NAME:-HL5170DN}"

echo "==> Installing Brother HL-5170DN PCL/PJL CUPS driver"

# Dependencies
# - cups-filters provides pwgtoraster and rastertopdf used by the AirPrint path
echo "--> Checking dependencies..."
apt-get install -y cups cups-filters avahi-daemon ghostscript

# Clean up any orphaned files from the previous HL-5070N name
for f in "$OLD_FILTER_DST" "$OLD_PPD_DST"; do
    if [ -e "$f" ]; then
        echo "--> Removing orphaned: $f"
        rm -f "$f"
    fi
done

# Filter
echo "--> Installing filter: $FILTER_DST"
install -m 755 -o root -g root "$FILTER_SRC" "$FILTER_DST"

# PPD
echo "--> Installing PPD: $PPD_DST"
install -m 644 -o root -g root "$PPD_SRC" "$PPD_DST"

# CUPS config: enable sharing and browsing
echo "--> Configuring CUPS for network sharing..."
cupsctl --share-printers --remote-any

# Ensure avahi is running (needed for Bonjour/AirPrint advertisement)
echo "--> Enabling avahi-daemon..."
systemctl enable avahi-daemon
systemctl start avahi-daemon

# Restart CUPS so it picks up the new filter and PPD
echo "--> Restarting CUPS..."
systemctl restart cups

# Wait briefly for CUPS to be reachable after restart (cupsd takes a
# second or two to start listening; lpadmin would otherwise fail with
# "client-error-not-found" in that window).
for i in 1 2 3 4 5 6 7 8 9 10; do
    if lpstat -r >/dev/null 2>&1; then break; fi
    sleep 1
done

# ---- Add / update the queue -------------------------------------------

if [ "${SKIP_QUEUE:-0}" = "1" ]; then
    echo "--> SKIP_QUEUE=1, skipping queue setup."
else
    echo "--> Discovering printer USB URI..."

    if [ -n "${PRINTER_URI:-}" ]; then
        URI="$PRINTER_URI"
        echo "    Using PRINTER_URI from environment: $URI"
    else
        # lpinfo lines look like:
        #   direct usb://Brother/HL-5170DN%20series?serial=L4J624176
        # Match the family (HL-50xxN / HL-51xxDN) so this works for the
        # HL-5070N too (same PJL/PCL vocabulary).
        URI=$(lpinfo -v 2>/dev/null \
            | grep -i 'usb://Brother/HL-5' \
            | head -1 \
            | awk '{print $2}')

        if [ -z "$URI" ]; then
            echo ""
            echo "!! No Brother USB printer detected via lpinfo."
            echo "   - Make sure the printer is plugged in via USB and powered on."
            echo "   - List visible devices with:  sudo lpinfo -v"
            echo "   - Then re-run this script, or pass the URI explicitly:"
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
    # -E enables the queue and accepts jobs; lpadmin treats this command
    # as create-or-update depending on whether the queue exists.
    lpadmin -p "$QUEUE_NAME" -E -v "$URI" -P "$PPD_DST"

    echo "--> Marking queue shared (for AirPrint and other LAN clients)..."
    lpadmin -p "$QUEUE_NAME" -o printer-is-shared=true

    echo "--> Setting queue defaults..."
    # Use lpadmin -o KEY-default=VALUE, not lpoptions -o KEY=VALUE.
    # lpoptions writes to /etc/cups/lpoptions (system-wide override) which
    # silently beats the queue default in /etc/cups/printers.conf. See
    # README "CUPS option precedence".
    lpadmin -p "$QUEUE_NAME" -o TonerSave-default=OFF
    lpadmin -p "$QUEUE_NAME" -o Resolution-default=600dpi
    lpadmin -p "$QUEUE_NAME" -o Duplex-default=DuplexNoTumble
fi

echo ""
echo "==> Done."
if [ "${SKIP_QUEUE:-0}" != "1" ]; then
    echo ""
    echo "    Queue:   $QUEUE_NAME (shared)"
    lpstat -p "$QUEUE_NAME" 2>/dev/null | sed 's/^/    /' || true
    echo ""
    echo "    Smoke test:"
    echo "      echo 'hello world' | lp -d $QUEUE_NAME"
    echo ""
    echo "    AirPrint discovery from another device on the LAN:"
    echo "      avahi-browse -rt _ipp._tcp        # on Linux"
    echo "      dns-sd -B _ipp._tcp local.        # on macOS"
fi
echo ""
echo "    Mac/iPhone clients: see README 'Mac Setup' and 'AirPrint Visitors'."
echo "    Re-running this script is safe; it updates the existing install."
echo ""
