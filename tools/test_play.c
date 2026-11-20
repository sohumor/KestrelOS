#define PLAY_HOST_TEST
#include "../apps/play.c"
#include <assert.h>

static void put16(uint8_t *p, unsigned int v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static unsigned int fixture(uint8_t *buf, int channels, int bits, int rate)
{
    unsigned int bytes = 4 * channels * bits / 8;
    memset(buf, 0, 128);
    memcpy(buf, "RIFF", 4);
    put32(buf + 4, 36 + bytes);
    memcpy(buf + 8, "WAVEfmt ", 8);
    put32(buf + 16, 16);
    put16(buf + 20, 1);
    put16(buf + 22, channels);
    put32(buf + 24, rate);
    put32(buf + 28, rate * channels * bits / 8);
    put16(buf + 32, channels * bits / 8);
    put16(buf + 34, bits);
    memcpy(buf + 36, "data", 4);
    put32(buf + 40, bytes);
    return 44 + bytes;
}

int main(void)
{
    uint8_t buf[128], copy[128];
    struct wave_pcm pcm;
    int16_t out[2];
    int checks = 0;
    const int rates[] = { 8000, 11025, 22050, 32000, 44100, 48000, 96000, 192000 };
    for (unsigned int r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        for (int channels = 1; channels <= 2; channels++) {
            for (int bits = 8; bits <= 16; bits += 8) {
                unsigned int n = fixture(buf, channels, bits, rates[r]);
                assert(parse_wave(buf, n, &pcm) == NULL);
                assert(pcm.frames == 4 && pcm.rate == (unsigned int)rates[r]);
                for (uint64_t frame = 0; frame < pcm.output_frames; frame++)
                    convert_frame(&pcm, frame, out);
                checks++;
            }
        }
    }
    unsigned int n = fixture(buf, 1, 8, 24000);
    buf[44] = 0; buf[45] = 128; buf[46] = 255; buf[47] = 128;
    assert(parse_wave(buf, n, &pcm) == NULL);
    convert_frame(&pcm, 0, out);
    assert(out[0] == -32768 && out[1] == -32768);
    convert_frame(&pcm, 1, out);
    assert(out[0] == -16384 && out[1] == -16384);
    convert_frame(&pcm, 4, out);
    assert(out[0] == 32512);
    n = fixture(buf, 2, 16, 48000);
    put16(buf + 44, 32767); put16(buf + 46, 32768);
    assert(parse_wave(buf, n, &pcm) == NULL);
    convert_frame(&pcm, 0, out);
    assert(out[0] == 32767 && out[1] == -32768);
    memcpy(copy, buf, n);
    assert(parse_wave(buf, 0, &pcm) != NULL);
    assert(parse_wave(buf, 11, &pcm) != NULL);
    assert(parse_wave(buf, n - 1, &pcm) != NULL);
    buf[0] = 'X'; assert(parse_wave(buf, n, &pcm) != NULL); buf[0] = 'R';
    put16(buf + 20, 3); assert(parse_wave(buf, n, &pcm) != NULL); put16(buf + 20, 1);
    put16(buf + 22, 3); assert(parse_wave(buf, n, &pcm) != NULL); put16(buf + 22, 2);
    put32(buf + 24, 0); assert(parse_wave(buf, n, &pcm) != NULL); put32(buf + 24, 48000);
    put16(buf + 34, 24); assert(parse_wave(buf, n, &pcm) != NULL); put16(buf + 34, 16);
    put16(buf + 32, 1); assert(parse_wave(buf, n, &pcm) != NULL); put16(buf + 32, 4);
    put32(buf + 40, 0); assert(parse_wave(buf, n, &pcm) != NULL);
    put32(buf + 40, 3); assert(parse_wave(buf, n, &pcm) != NULL);
    put32(buf + 40, 0xFFFFFFFF); assert(parse_wave(buf, n, &pcm) != NULL);
    memcpy(buf, copy, n);
    memmove(buf + 22, buf + 12, n - 12);
    memcpy(buf + 12, "JUNK", 4); put32(buf + 16, 1);
    put32(buf + 4, n + 2);
    assert(parse_wave(buf, n + 10, &pcm) == NULL);
    printf("PASS play: %d PCM format/rate combinations, interpolation, stereo, padded chunks, empty and malformed WAV\n", checks);
    return 0;
}
