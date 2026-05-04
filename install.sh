#!/bin/bash
#
# install.sh - Install Brother HL-5170DN PCL/PJL driver on Raspberry Pi
#
# Run as root: sudo bash install.sh
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

# Restart CUPS
echo "--> Restarting CUPS..."
systemctl restart cups

echo ""
echo "==> Done. Next steps:"
echo ""
echo "  1. Add the printer via http://localhost:631"
echo "     - Administration → Add Printer → USB Printer"
echo "     - Choose 'Brother-HL5170DN-PCL.ppd' as the driver"
echo ""
echo "  2. Set queue-wide defaults (use lpadmin -o KEY-default=VALUE,"
echo "     NOT lpoptions -o KEY=VALUE — the latter writes to /etc/cups/lpoptions"
echo "     which silently overrides the queue default):"
echo "     sudo lpadmin -p <printer-name> -o TonerSave-default=OFF"
echo "     sudo lpadmin -p <printer-name> -o Resolution-default=600dpi"
echo "     sudo lpadmin -p <printer-name> -o Duplex-default=DuplexNoTumble"
echo ""
echo "  3. Extract PPD for Mac manual install:"
echo "     scp pi@<pi-ip>:$PPD_DST ~/Downloads/"
echo ""
echo "     Then on macOS: System Settings → Printers & Scanners → Add (+)"
echo "     → IP tab → IPP protocol → queue 'printers/<printer-name>'"
echo "     → Use: Other... → select the downloaded PPD."
