#!/bin/sh
# Build and package Navidrome.pak.zip for NextUI (TrimUI Brick / Smart Pro, tg5040).
# Usage: scripts/package.sh [version]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:-$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' "$ROOT/pak.json")}"
VERSION="${VERSION#v}"   # tags are vX.Y.Z; pak.json uses X.Y.Z
"$ROOT/scripts/build.sh"
STAGE="$ROOT/build/Navidrome.pak"
rm -rf "$ROOT/build" && mkdir -p "$STAGE/bin/tg5040"
cp "$ROOT/launch.sh" "$STAGE/"
sed "s/\"version\": *\"[^\"]*\"/\"version\": \"$VERSION\"/" "$ROOT/pak.json" > "$STAGE/pak.json"
cp -r "$ROOT/res" "$STAGE/res"
cp "$ROOT/bin/tg5040/navidrome.elf" "$ROOT/bin/tg5040/libmsettings.so" "$STAGE/bin/tg5040/"
(cd "$STAGE" && zip -qr "$ROOT/build/Navidrome.pak.zip" .)
echo "Built $ROOT/build/Navidrome.pak.zip (version $VERSION)"
