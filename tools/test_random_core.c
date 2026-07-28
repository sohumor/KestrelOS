#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "random_core.h"

static int checks;
static int failures;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        failures++; \
    } \
} while (0)

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int equals_hex(const uint8_t *got, size_t len, const char *hex)
{
    for (size_t i = 0; i < len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0 || got[i] != (uint8_t)((hi << 4) | lo))
            return 0;
    }
    return hex[len * 2] == '\0';
}

static void test_sha256(void)
{
    struct random_sha256 s;
    uint8_t out[32];

    random_sha256_init(&s);
    random_sha256_final(&s, out);
    CHECK(equals_hex(
              out, sizeof(out),
              "e3b0c44298fc1c149afbf4c8996fb924"
              "27ae41e4649b934ca495991b7852b855"),
          "SHA-256 empty vector");

    random_sha256_init(&s);
    random_sha256_update(&s, "a", 1);
    random_sha256_update(&s, "bc", 2);
    random_sha256_final(&s, out);
    CHECK(equals_hex(
              out, sizeof(out),
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad"),
          "SHA-256 abc split-update vector");
}

static void test_chacha20(void)
{
    uint8_t key[32];
    uint8_t nonce[12] = {
        0x00, 0x00, 0x00, 0x09, 0x00, 0x00,
        0x00, 0x4a, 0x00, 0x00, 0x00, 0x00,
    };
    uint8_t out[64], again[64], next[64];

    for (int i = 0; i < 32; i++)
        key[i] = (uint8_t)i;
    random_chacha20_block(out, key, 1, nonce);
    CHECK(equals_hex(
              out, sizeof(out),
              "10f1e7e4d13b5915500fdd1fa32071c4"
              "c7d1f4c733c068030422aa9ac3d46c4e"
              "d2826446079faa0914c2d705d98b02a2"
              "b5129cd1de164eb9cbd083e8a2503c4e"),
          "RFC 8439 ChaCha20 block vector");

    random_chacha20_block(again, key, 1, nonce);
    CHECK(memcmp(out, again, sizeof(out)) == 0,
          "ChaCha20 is deterministic for one state");
    random_chacha20_block(next, key, 2, nonce);
    CHECK(memcmp(out, next, sizeof(out)) != 0,
          "ChaCha20 counter separates adjacent blocks");
}

int main(void)
{
    test_sha256();
    test_chacha20();
    if (failures) {
        fprintf(stderr, "FAIL: %d/%d random-core checks failed\n",
                failures, checks);
        return 1;
    }
    printf("PASS: %d SHA-256/ChaCha20 random-core checks\n", checks);
    return 0;
}
