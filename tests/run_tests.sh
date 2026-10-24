#!/bin/sh
# Host-side tests for the Subsonic API layer (no device or toolchain needed).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
OUT="${TMPDIR:-/tmp}/nd_test_api"
gcc -std=gnu99 -Wall -Wextra -Wno-unused-parameter -O0 -g -fsanitize=address,undefined \
  -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>' \
  -I"$SRC" -I"$SRC/include" -I"$SRC/include/mbedtls_lib" \
  "$HERE/test_api.c" "$SRC/nd_api.c" "$HERE/stub_log.c" "$SRC/include/parson/parson.c" \
  "$SRC/include/mbedtls_lib/md5.c" "$SRC/include/mbedtls_lib/platform_util.c" \
  -o "$OUT"
"$OUT" "$HERE/fixtures"
