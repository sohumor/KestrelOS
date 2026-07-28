#!/bin/sh
set -eu

tmp="${TMPDIR:-/tmp}/kestrel-random-test-$$"
trap 'rm -f "$tmp"' EXIT HUP INT TERM

${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -g \
    -fsanitize=address,undefined \
    -Ikernel/include \
    kernel/random_core.c tools/test_random_core.c -o "$tmp"
"$tmp"
