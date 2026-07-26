#!/bin/sh
# Build each browser-stack archive on its own and report its size, so a
# compile error in one library is not hidden behind another.
for a in libz libimg libweb libjs libtls; do
    printf '=== %s ===\n' "$a"
    make "build/$a.a" 2>&1 | grep -E 'error|Error' | head -8
    if [ -f "build/$a.a" ]; then
        printf '    %s bytes\n' "$(stat -c %s "build/$a.a")"
    else
        printf '    (not built)\n'
    fi
done
