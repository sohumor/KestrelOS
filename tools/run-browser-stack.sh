#!/bin/sh
# Build and run the whole-browser guarded-stack gate in isolation.
# No shared archive, application object, disk image, or top-level make target
# is read or rewritten.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/qa-wave2-stack"
CC=${CC:-gcc}

mkdir -p "$OUT"

CFLAGS="-std=c11 -m64 -O2 -g -mno-red-zone -fno-stack-protector \
-fno-omit-frame-pointer \
-Wall -Wextra -Werror -Wno-format-truncation -fstack-usage -DJS_HOST \
-DCSS_WITH_DOM \
-I$ROOT/tools/browser-host -I$ROOT/libgui -I$ROOT/libz -I$ROOT/libtls \
-I$ROOT/libimg -I$ROOT/libweb -I$ROOT/libjs"

compile()
{
    src=$1
    obj=$2
    # shellcheck disable=SC2086
    "$CC" $CFLAGS -c "$ROOT/$src" -o "$OUT/$obj.o"
}

compile tools/test_browser_stack.c test_browser_stack
compile libweb/dom.c dom
compile libweb/entities.c entities
compile libweb/html.c html
compile libweb/css.c css
compile libweb/style.c style
compile libweb/ua_style.c ua_style
compile libweb/url.c url
compile libweb/storage.c storage
compile libweb/jsdom.c jsdom
compile libweb/layout.c layout
compile libweb/paint.c paint
compile libjs/value.c js_value
compile libjs/lex.c js_lex
compile libjs/parse.c js_parse
compile libjs/interp.c js_interp
compile libjs/builtin.c js_builtin
compile libjs/webapi.c js_webapi
compile libtls/hash.c tls_hash
compile libgui/font.c font
compile libgui/font_data.c font_data
compile libgui/draw.c draw

"$CC" -pthread -o "$OUT/test_browser_stack" \
    "$OUT/test_browser_stack.o" "$OUT/dom.o" "$OUT/entities.o" \
    "$OUT/html.o" "$OUT/css.o" "$OUT/style.o" "$OUT/ua_style.o" \
    "$OUT/url.o" "$OUT/storage.o" "$OUT/jsdom.o" "$OUT/layout.o" "$OUT/paint.o" \
    "$OUT/js_value.o" "$OUT/js_lex.o" "$OUT/js_parse.o" \
    "$OUT/js_interp.o" "$OUT/js_builtin.o" "$OUT/js_webapi.o" \
    "$OUT/tls_hash.o" \
    "$OUT/font.o" \
    "$OUT/font_data.o" "$OUT/draw.o"

failed=0
if ! "$OUT/test_browser_stack" text > "$OUT/text.out"; then
    failed=1
fi
cat "$OUT/text.out"
grep -F "STACK-GATE-TOP" "$OUT/text.out" >/dev/null || failed=1
grep -F "STACK-MODULE-OK" "$OUT/text.out" >/dev/null || failed=1
grep -F "lifecycle: complete" "$OUT/text.out" >/dev/null || failed=1
grep -F "#deep" "$OUT/text.out" >/dev/null || failed=1
grep -F "[layout truncated: 0x2]" "$OUT/text.out" >/dev/null || failed=1
grep -F "STACK-GATE mode=text" "$OUT/text.out" >/dev/null || failed=1

if ! "$OUT/test_browser_stack" gui > "$OUT/gui.out"; then
    failed=1
fi
cat "$OUT/gui.out"
grep -F "STACK-GATE mode=gui" "$OUT/gui.out" >/dev/null || failed=1
grep -E "flushes=[2-9][0-9]* changed_pixels=[1-9][0-9]*" \
    "$OUT/gui.out" >/dev/null || failed=1

if [ "$failed" -ne 0 ]; then
    printf '%s\n' "browser stack gate: FAILED" >&2
    exit 1
fi
printf '%s\n' "browser stack gate: 2 paths passed"
