#!/bin/bash
#
# install.sh - Install Brother HL-5070N PCL/PJL driver on Raspberry Pi
#
# Run as root: sudo bash install.sh
#
set -euo pipefail

FILTER_SRC="brother-hl5070n-pjl"
PPD_SRC="Brother-HL5070N-PCL.ppd"
FILTER_DST="/usr/lib/cups/filter/brother-hl5070n-pjl"
PPD_DST="/usr/share/cups/model/Brother-HL5070N-PCL.ppd"

echo "==> Installing Brother HL-5070N PCL/PJL CUPS driver"

# Dependencies
echo "--> Checking dependencies..."
apt-get install -y cups avahi-daemon ghostscript

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
echo "     - Choose 'Brother-HL5070N-PCL.ppd' as the driver"
echo ""
echo "  2. Set good defaults:"
echo "     lpoptions -p <printer-name> -o TonerSave=OFF"
echo "     lpoptions -p <printer-name> -o Resolution=600dpi"
echo "     lpoptions -p <printer-name> -o Duplex=None"
echo ""
echo "  3. On Mac: enable CUPS web UI with:"
echo "     cupsctl WebInterface=yes"
echo "     Then visit http://localhost:631 to add the Pi printer"
echo "     and manually select Brother-HL5070N-PCL.ppd"
echo ""
echo "  4. Extract PPD for Mac manual install:"
echo "     scp pi@<pi-ip>:$PPD_DST ~/Downloads/"
