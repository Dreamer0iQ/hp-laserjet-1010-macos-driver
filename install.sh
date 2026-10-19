#!/bin/sh
# Install the HP LaserJet 1010/1012 driver from a source build and add a print
# queue for any connected printer.   Usage: make && sudo ./install.sh
set -e

DEST=/Library/Printers/HP1010FastRaster

if [ "$(id -u)" != 0 ]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi
cd "$(dirname "$0")"
if [ ! -x build/rastertolj1010 ] || [ ! -f build/hp1010fr.ppd ]; then
    echo "Build first: make" >&2
    exit 1
fi

# cupsd only runs filters that are root-owned and not group/world-writable.
install -d -o root -g wheel -m 755 "$DEST" "$DEST/Filters" "$DEST/PPDs"
install -o root -g wheel -m 755 build/rastertolj1010 "$DEST/Filters/rastertolj1010"
install -o root -g wheel -m 755 uninstall.sh "$DEST/uninstall.sh"
for m in 1010 1012; do
    gzip -9nc "build/hp${m}fr.ppd" > "$DEST/PPDs/HP LaserJet $m FastRaster.gz"
done
chown root:wheel "$DEST"/PPDs/*.gz
chmod 644 "$DEST"/PPDs/*.gz
echo "Driver installed to $DEST"

sh scripts/postinstall
