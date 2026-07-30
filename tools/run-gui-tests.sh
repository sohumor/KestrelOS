#!/bin/sh
set -eu

mkdir -p build/host
${CC:-cc} -std=c11 -Wall -Wextra -Werror \
  -Iabi -Ilibgui -idirafter libc/include \
  tools/test_gui_primitives.c libgui/draw.c libgui/font_data.c \
  -o build/host/test-gui-primitives
build/host/test-gui-primitives
