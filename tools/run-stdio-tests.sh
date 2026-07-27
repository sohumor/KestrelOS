#!/bin/sh
# Build the real target libc/stdio.c under renamed symbols, then compare it
# with the host formatter.  All artifacts live in a private temporary dir.

set -u

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/kestrel-stdio.XXXXXX") || exit 2
trap 'rm -rf -- "$TMP"' EXIT HUP INT TERM

CC=${CC:-gcc}
OBJCOPY=${OBJCOPY:-objcopy}
COMMON="-std=c11 -Wall -Wextra -Werror -g -fno-builtin"
INCLUDES="-I$ROOT/libc/include -I$ROOT/abi"
RENAMES="--redefine-sym printf=k_printf \
--redefine-sym vprintf=k_vprintf \
--redefine-sym snprintf=k_snprintf \
--redefine-sym vsnprintf=k_vsnprintf \
--redefine-sym puts=k_puts \
--redefine-sym putchar=k_putchar \
--redefine-sym getchar=k_getchar \
--redefine-sym write=k_write \
--redefine-sym read=k_read"
SAN="-O1 -fno-omit-frame-pointer -fsanitize=address,undefined"
PLAIN="-O2"

if ! command -v "$OBJCOPY" >/dev/null 2>&1; then
    echo "error: objcopy is required to isolate target stdio symbols" >&2
    exit 2
fi

# Intentional word splitting turns the option groups above into argv entries.
# No group contains a path with whitespace.
# shellcheck disable=SC2086
$CC $COMMON $SAN $INCLUDES -c "$ROOT/libc/stdio.c" \
    -o "$TMP/stdio-san-raw.o" || exit 2
# shellcheck disable=SC2086
$OBJCOPY $RENAMES "$TMP/stdio-san-raw.o" "$TMP/stdio-san.o" || exit 2
# shellcheck disable=SC2086
$CC $COMMON $SAN "$ROOT/tools/test_stdio.c" "$TMP/stdio-san.o" \
    -o "$TMP/test-stdio-san" || exit 2

if [ "${1:-}" = "--x509-crash" ]; then
    ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$TMP/test-stdio-san" --x509-crash
    exit $?
fi
if [ "$#" -ne 0 ]; then
    echo "usage: tools/run-stdio-tests.sh [--x509-crash]" >&2
    exit 2
fi

# shellcheck disable=SC2086
$CC $COMMON $PLAIN $INCLUDES -c "$ROOT/libc/stdio.c" \
    -o "$TMP/stdio-plain-raw.o" || exit 2
# shellcheck disable=SC2086
$OBJCOPY $RENAMES "$TMP/stdio-plain-raw.o" "$TMP/stdio-plain.o" || exit 2
# shellcheck disable=SC2086
$CC $COMMON $PLAIN "$ROOT/tools/test_stdio.c" "$TMP/stdio-plain.o" \
    -o "$TMP/test-stdio-plain" || exit 2

echo "=== ASan + UBSan ==="
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$TMP/test-stdio-san"
SAN_RC=$?

echo "=== non-sanitized guard-page run ==="
"$TMP/test-stdio-plain"
PLAIN_RC=$?

if [ "$SAN_RC" -ne 0 ] || [ "$PLAIN_RC" -ne 0 ]; then
    exit 1
fi
exit 0
