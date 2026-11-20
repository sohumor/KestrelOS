#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/kestrel-tcp-rst.XXXXXX")
trap 'rm -rf "$OUT"' EXIT HUP INT TERM
"${CC:-gcc}" -std=c11 -Wall -Wextra -Werror -O2 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$ROOT/kernel/include" -I"$ROOT/abi" \
    "$ROOT/tools/test_tcp_rst.c" "$ROOT/kernel/tcp_reassembly.c" \
    "$ROOT/kernel/net_checksum.c" -o "$OUT/test_tcp_rst"
"$OUT/test_tcp_rst"
