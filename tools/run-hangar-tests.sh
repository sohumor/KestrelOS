#!/bin/sh
set -eu

tmp="${TMPDIR:-/tmp}/kestrel-hangar-test-$$"
trap 'rm -f "$tmp"' EXIT HUP INT TERM

${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -g \
    -fsanitize=address,undefined \
    -Ikernel/include \
    kernel/hangar_fmt.c tools/test_hangar.c -o "$tmp"
"$tmp"
