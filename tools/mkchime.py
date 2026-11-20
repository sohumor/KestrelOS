#!/usr/bin/env python3
"""Generate a small 22.05 kHz unsigned 8-bit mono WAV for Audio Player."""

import math
import sys
import wave


def chime():
    rate = 22050
    notes = (523.25, 659.25, 783.99, 1046.50, 783.99, 659.25)
    samples = bytearray()
    for frequency in notes:
        for i in range(rate):
            t = i / rate
            envelope = max(0, min(t / 0.02, (0.9 - t) / 0.2, 1))
            value = 128 + round(80 * envelope * math.sin(2 * math.pi * frequency * t))
            samples.append(value)
    return samples


if __name__ == "__main__":
    with wave.open(sys.argv[1], "wb") as wav:
        wav.setparams((1, 1, 22050, 0, "NONE", "not compressed"))
        wav.writeframes(chime())
