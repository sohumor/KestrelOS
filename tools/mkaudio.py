#!/usr/bin/env python3
"""Generate the fixed-format HDA playback fixture (48 kHz stereo s16le)."""

import math
import struct
import sys


def tone():
    return b"".join(struct.pack("<hh", *([round(12000 * math.sin(
        2 * math.pi * 440 * i / 48000))] * 2)) for i in range(48000))


if __name__ == "__main__":
    with open(sys.argv[1], "wb") as f:
        f.write(tone())
