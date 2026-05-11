#!/bin/bash
# run-test.sh — stamp a per-test label on every page then submit via ipptool
#
# Usage: run-test.sh LABEL TESTFILE [PDF]
#
#   LABEL     Printed label, also used as job-name in logs.
#              e.g. "P2-T1a" or "P2-T1a: text 300dpi duplex"
#              Avoid PostScript special chars: ( ) < > [ ] { } / %
#   TESTFILE  Path to .test file relative to repo root or absolute.
#              e.g. tests/print-duplex-long-300.test
#   PDF       Input PDF (default: /tmp/2page-test.pdf)
#
# Example:
#   ./run-test.sh "P2-T1a" tests/print-duplex-long-300.test /tmp/2page-test.pdf
#   ./run-test.sh "P6A-T1" tests/print-apt.test /home/tuttle/pi-printer/image-test.pdf

set -euo pipefail

LABEL="${1:?Usage: run-test.sh LABEL TESTFILE [PDF]}"
TESTFILE="${2:?Usage: run-test.sh LABEL TESTFILE [PDF]}"
PDF="${3:-/tmp/2page-test.pdf}"
PRINTER_URI="ipp://localhost:8000/ipp/print"
STAMPED="/tmp/run-test-$$.pdf"

trap 'rm -f "$STAMPED"' EXIT

# Stamp LABEL at the top of every page.
# The -c block installs a BeginPage hook before -f processes the PDF.
# Coordinates assume Letter (612x792 pts); label sits ~17pt from the top edge.
# Note: does not adapt to A4/envelope page sizes.
gs -q -dBATCH -dNOPAUSE -sDEVICE=pdfwrite \
   -sOutputFile="$STAMPED" \
   -c "<< /BeginPage {
         gsave
         /Helvetica findfont 9 scalefont setfont
         28 775 moveto
         ($LABEL) show
         grestore
       } bind >> setpagedevice" \
   -f "$PDF"

ipptool -tv \
   -f "$STAMPED" \
   -d filetype=application/pdf \
   -d "jobname=$LABEL" \
   "$PRINTER_URI" \
   "$TESTFILE"
