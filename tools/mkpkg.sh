#!/bin/sh
# Build a flat installer package from a payload directory without pkgbuild.
# pkgbuild archives extended attributes (e.g. com.apple.provenance, which
# cannot be cleared on recent macOS) as "._*" AppleDouble entries; plain cpio
# leaves them out, so the package installs exactly the files in <root>.
#
# Usage: mkpkg.sh <root> <scripts-dir> <identifier> <version> <install-location> <out.pkg>
set -e

ROOT=$1 SCRIPTS=$2 ID=$3 VERSION=$4 LOCATION=$5 OUT=$6
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/flat"

# Bom: every entry owned by root:wheel, with the payload's own modes.
(
    cd "$ROOT"
    find . | sort | while read -r p; do
        mode=$(stat -f '%p' "$p")
        if [ -d "$p" ]; then
            printf '%s\t%s\t0/0\n' "$p" "$mode"
        else
            set -- $(cksum < "$p")
            printf '%s\t%s\t0/0\t%s\t%s\n' "$p" "$mode" "$2" "$1"
        fi
    done
) > "$WORK/filelist"
mkbom -i "$WORK/filelist" "$WORK/flat/Bom"

(cd "$ROOT" && find . | sort | cpio -o --format odc -R 0:0 2>/dev/null) | gzip -9n > "$WORK/flat/Payload"
# pkgutil --flatten archives Scripts itself and wants it as a directory.
cp -R "$SCRIPTS" "$WORK/flat/Scripts"

files=$(find "$ROOT" | wc -l | tr -d ' ')
kbytes=$(du -sk "$ROOT" | cut -f1)
cat > "$WORK/flat/PackageInfo" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<pkg-info overwrite-permissions="true" relocatable="false" identifier="$ID" postinstall-action="none" version="$VERSION" format-version="2" install-location="$LOCATION" auth="root">
    <payload numberOfFiles="$files" installKBytes="$kbytes"/>
    <scripts>
        <postinstall file="./postinstall" timeout="600"/>
    </scripts>
</pkg-info>
EOF

rm -f "$OUT"
pkgutil --flatten "$WORK/flat" "$OUT"
