#!/bin/sh
# Live test against a real server (default: https://demo.navidrome.org demo/demo).
# Needs network. Usage: tests/run_live_test.sh [server_url user password]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
OUT="${TMPDIR:-/tmp}/nd_live_test"
gcc -std=gnu99 -O1 -g -w -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>' \
  -I"$SRC" -I"$SRC/include" -I"$SRC/include/mbedtls_lib" \
  "$HERE/live_test.c" "$SRC/nd_api.c" "$SRC/nd_net.c" "$HERE/stub_log.c" \
  "$SRC/include/parson/parson.c" "$SRC/include/mbedtls_entropy_alt.c" "$SRC"/include/mbedtls_lib/*.c \
  -lz -lpthread -o "$OUT"
"$OUT" "$@"
