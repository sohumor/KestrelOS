#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/qa-jsdom"
CC=${CC:-gcc}

mkdir -p "$OUT"
CFLAGS="-std=c11 -m64 -O2 -g -Wall -Wextra -Werror -DJS_HOST -DCSS_WITH_DOM \
-I$ROOT/libweb -I$ROOT/libjs -I$ROOT/libtls"

for src in \
    tools/test_jsdom.c \
    libweb/dom.c libweb/entities.c libweb/html.c libweb/css.c \
    libweb/url.c libweb/storage.c \
    libweb/jsdom.c \
    libtls/hash.c \
    libjs/value.c libjs/lex.c libjs/parse.c libjs/interp.c libjs/builtin.c \
    libjs/webapi.c
do
    obj=$(printf '%s' "$src" | tr '/.' '__')
    # shellcheck disable=SC2086
    "$CC" $CFLAGS -c "$ROOT/$src" -o "$OUT/$obj.o"
done

"$CC" -o "$OUT/test_jsdom" "$OUT"/*.o
"$OUT/test_jsdom"
