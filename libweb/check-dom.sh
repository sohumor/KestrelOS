#!/bin/sh
# Build checks and test runs for libweb. Not part of the kernel build;
# run it by hand:  sh libweb/check.sh [--long]
cd "$(dirname "$0")/.." || exit 1
set -e

CFLAGS_TARGET="-m64 -ffreestanding -nostdlib -fno-pic -fno-pie -Wall -Wextra -O2"
SRC="libweb/dom.c libweb/html.c libweb/entities.c"

echo "--- target (freestanding) syntax check"
for f in $SRC; do
    printf '  %s\n' "$f"
    gcc $CFLAGS_TARGET -Ilibc/include -Iabi -Ilibgui -Ilibweb -fsyntax-only "$f"
done

echo "--- host build (asan+ubsan)"
gcc -Wall -Wextra -O1 -g -fsanitize=address,undefined \
    -fno-sanitize-recover=all -Ilibweb \
    -o /tmp/test_dom $SRC tools/test_dom.c

echo "--- host build (plain, for the long runs)"
gcc -Wall -Wextra -O2 -Ilibweb -o /tmp/test_dom_fast $SRC tools/test_dom.c

echo "--- sizes"
for f in $SRC; do
    gcc $CFLAGS_TARGET -Ilibc/include -Iabi -Ilibgui -Ilibweb -c "$f" \
        -o /tmp/sz.o
    printf '  %-22s %6d bytes of object code\n' "$f" \
        "$(size -t /tmp/sz.o | tail -1 | awk '{print $1+$2}')"
done

echo "--- test run (asan+ubsan)"
/tmp/test_dom --fuzz 40000

if [ "$1" = "--long" ]; then
    echo "--- long fuzz, twelve seeds, uninstrumented"
    for s in 1 2 3 4 5 6 7 8 9 10 11 12; do
        /tmp/test_dom_fast --quick --seed "$s" --fuzz 250000 | \
            grep -E 'documents,|passed'
    done
    echo "--- long fuzz under asan+ubsan, four seeds"
    for s in 21 22 23 24; do
        /tmp/test_dom --quick --seed "$s" --fuzz 150000 | \
            grep -E 'documents,|passed'
    done
fi

echo "ok"
