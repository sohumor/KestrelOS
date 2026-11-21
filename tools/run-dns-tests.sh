#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/kestrel-dns.XXXXXX")
trap 'rm -rf "$OUT"' EXIT HUP INT TERM

CC=${CC:-gcc}
CFLAGS="-std=c11 -Wall -Wextra -Werror -O2 -g"

"$CC" $CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$ROOT/kernel/include" -I"$ROOT/abi" \
    "$ROOT/tools/test_dns.c" \
    -o "$OUT/test_dns"

"$OUT/test_dns"
