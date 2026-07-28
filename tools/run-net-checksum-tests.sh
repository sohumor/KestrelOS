#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/kestrel-net-checksum.XXXXXX")
trap 'rm -rf "$OUT"' EXIT HUP INT TERM

CC=${CC:-gcc}
CFLAGS="-std=c11 -Wall -Wextra -Werror -O2 -g"

"$CC" $CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$ROOT/kernel/include" -I"$ROOT/abi" \
    "$ROOT/tools/test_net_checksum.c" \
    "$ROOT/kernel/net_checksum.c" \
    -o "$OUT/test_net_checksum"

"$OUT/test_net_checksum"
