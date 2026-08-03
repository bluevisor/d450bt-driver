#!/bin/bash
# Build and install the D450BT driver, then (re)create the print queue.
set -euo pipefail
cd "$(dirname "$0")"

QUEUE=D450BT

make

echo "==> Installing filter + PPD (admin)"
if [ -t 0 ]; then
    make install
else
    # No TTY (e.g. run from an editor/agent): use a GUI admin prompt.
    osascript -e "do shell script \"install -o root -g wheel -m 755 '$PWD/build/rastertod450bt' /usr/libexec/cups/filter/rastertod450bt && install -o root -g wheel -m 644 '$PWD/ppd/D450BT.ppd' /Library/Printers/PPDs/Contents/Resources/D450BT.ppd\" with administrator privileges"
fi

# Prefer the CUPS bluetooth backend if the printer is paired; that is how
# the D450BT shows up on this Mac (it does not enumerate as a USB device).
URI=$(lpinfo -v | awk '/direct bluetooth:/ {print $2; exit}')
if [ -z "$URI" ]; then
    URI=$(lpinfo -v | awk '/direct usb:.*[Dd]450/ {print $2; exit}')
fi
if [ -z "$URI" ]; then
    echo "!! No bluetooth/USB URI found via lpinfo -v." >&2
    echo "   Pair the printer (or plug it in powered on) and re-run," >&2
    echo "   or create the queue manually:" >&2
    echo "   lpadmin -p $QUEUE -E -v <uri> -P /Library/Printers/PPDs/Contents/Resources/D450BT.ppd" >&2
    exit 1
fi

echo "==> Creating queue '$QUEUE' on $URI"
lpadmin -x "$QUEUE" 2>/dev/null || true
lpadmin -p "$QUEUE" -E -v "$URI" \
        -P /Library/Printers/PPDs/Contents/Resources/D450BT.ppd \
        -o printer-is-shared=false
cupsenable "$QUEUE"
cupsaccept "$QUEUE"

echo "==> Done. Test with:"
echo "    lp -d $QUEUE -o media=w288h432 <file.pdf>"
