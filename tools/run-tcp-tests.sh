#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/kestrel-tcp.XXXXXX")
trap 'rm -rf "$OUT"' EXIT HUP INT TERM

CC=${CC:-gcc}
CFLAGS="-std=c11 -Wall -Wextra -Werror -O2 -g"

"$CC" $CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$ROOT/kernel/include" \
    "$ROOT/tools/test_tcp_reassembly.c" \
    "$ROOT/kernel/tcp_reassembly.c" \
    -o "$OUT/test_tcp_reassembly"

"$OUT/test_tcp_reassembly"
