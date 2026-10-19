#!/bin/sh
# Remove the HP LaserJet 1010/1012 driver and the print queues that use it.
# Usage: sudo /Library/Printers/HP1010FastRaster/uninstall.sh
set -e

if [ "$(id -u)" != 0 ]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

for q in $(lpstat -e 2>/dev/null); do
    if grep -q 'HP1010FastRaster' "/etc/cups/ppd/$q.ppd" 2>/dev/null; then
        lpadmin -x "$q"
        echo "Removed printer $q"
    fi
done
rm -rf /Library/Printers/HP1010FastRaster
rm -f "/Library/Printers/PPDs/Contents/Resources/HP LaserJet 1010 FastRaster.gz" \
      "/Library/Printers/PPDs/Contents/Resources/HP LaserJet 1012 FastRaster.gz"
pkgutil --forget io.github.dreamer0iq.hp-laserjet-1010 >/dev/null 2>&1 || true
echo "Driver removed"
