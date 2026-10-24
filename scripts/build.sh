#!/bin/sh
# Cross-compile navidrome.elf (and libmsettings.so) with the tg5040 toolchain.
# Usage: scripts/build.sh [make-target]    NEXTUI_ROOT=/path/to/NextUI (default: ../NextUI)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

NEXTUI_ROOT="${NEXTUI_ROOT:-$ROOT/../NextUI}"
[ -d "$NEXTUI_ROOT/workspace" ] || git clone --depth 1 https://github.com/LoveRetro/NextUI.git "$NEXTUI_ROOT"

NEXTUI_ROOT="$(cd "$NEXTUI_ROOT" && pwd)"
ENGINE="$(command -v podman || command -v docker)"
IMAGE=ghcr.io/loveretro/tg5040-toolchain:latest
# Docker runs as root by default; keep build outputs owned by the caller.
USER_FLAG=""
case "$ENGINE" in *docker) USER_FLAG="--user $(id -u):$(id -g)" ;; esac
mkdir -p "$ROOT/bin/tg5040"
if [ ! -f "$ROOT/bin/tg5040/libmsettings.so" ]; then
  "$ENGINE" run --rm $USER_FLAG -v "$ROOT:$ROOT" -v "$NEXTUI_ROOT:$NEXTUI_ROOT" "$IMAGE" sh -c "
    cd '$NEXTUI_ROOT/workspace/tg5040/libmsettings' &&
    \${CROSS_COMPILE}gcc -c -fpic msettings.c -I../../all/common &&
    \${CROSS_COMPILE}gcc -c -fpic ../../all/common/displaycal.c -I../../all/common &&
    \${CROSS_COMPILE}gcc -shared -o libmsettings.so msettings.o displaycal.o -ltinyalsa -ldl -lrt -lm &&
    cp libmsettings.so '$ROOT/bin/tg5040/'"
fi
"$ENGINE" run --rm $USER_FLAG -v "$ROOT:$ROOT" -v "$NEXTUI_ROOT:$NEXTUI_ROOT" "$IMAGE" \
  sh -c "cd '$ROOT/src' && make NEXTUI_ROOT='$NEXTUI_ROOT' ${1:-all} 2>&1"
