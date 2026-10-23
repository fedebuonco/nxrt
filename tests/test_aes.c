// The package's AES (runtime/nxpkg.c): NCA headers are AES-XTS, sections AES-CTR, and the key area ECB. These are
// FIPS-197 / NIST SP 800-38A vectors, so a wrong round count, ShiftRows or counter increment shows up immediately.
// On AArch64 the CTR path is the NEON/AES one (the software path is only a fallback), so both get exercised.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "nxpkg.h"

static void unhex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void test_ecb(void) {
    // FIPS-197 C.1: AES-128 decryption.
    uint8_t key[16], data[16];
    unhex("000102030405060708090a0b0c0d0e0f", key, 16);
    unhex("69c4e0d86a7b0430d8cdb78070b4c55a", data, 16);
    nx_aes128_ecb_decrypt(key, data, 16);
    static const uint8_t want[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                     0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    CHECK_MEM(data, want, 16);
    // A partial block is left alone (the caller only passes whole blocks).
    uint8_t partial[16];
    memcpy(partial, want, 16);
    nx_aes128_ecb_decrypt(key, partial, 8);
    CHECK_MEM(partial, want, 16);
    // A zero-length decrypt does nothing.
    nx_aes128_ecb_decrypt(key, partial, 0);
    CHECK_MEM(partial, want, 16);
}

static void test_ctr(void) {
    // NIST SP 800-38A F.2.1: CTR-AES128 encrypt, decrypted here (the same XOR).
    uint8_t key[16], iv[16], buf[64];
    unhex("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
    unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv, 16);
    unhex("874d6191b620e3261bef6864990db6ce"
          "9806f66b7970fdff8617187bb9fffdff"
          "5ae4df3edbd5d35e5b4f09020db03eab"
          "1e031dda2fbe03d1792170a0f3009cee",
          buf, 64);
    nx_aes128_ctr(key, iv, buf, 64);
    static const uint8_t want[64] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73,
                                     0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7,
                                     0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4,
                                     0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef, 0xf6, 0x9f, 0x24, 0x45,
                                     0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
    CHECK_MEM(buf, want, 64);
    // Lengths that are not a multiple of 16 use the stream prefix.
    uint8_t part[20];
    unhex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff", part, 20);
    nx_aes128_ctr(key, iv, part, 20);
    CHECK_MEM(part, want, 20);
    // Encrypting the plaintext gives the ciphertext back (CTR is its own inverse), including the counter carry
    // across a block boundary.
    uint8_t round[64];
    memcpy(round, want, 64);
    nx_aes128_ctr(key, iv, round, 64);
    unhex("874d6191b620e3261bef6864990db6ce"
          "9806f66b7970fdff8617187bb9fffdff"
          "5ae4df3edbd5d35e5b4f09020db03eab"
          "1e031dda2fbe03d1792170a0f3009cee",
          buf, 64);
    CHECK_MEM(round, buf, 64);
    // The counter's low byte carries into the next byte (RIPEMD-style big-endian increment).
    uint8_t ctr_key[16] = {0}, ctr_iv[16] = {0}, next_iv[16] = {0};
    ctr_iv[15] = 0xff;
    next_iv[14] = 0x01;  // 0xff + 1
    uint8_t a[32] = {0}, b[32] = {0};
    nx_aes128_ctr(ctr_key, ctr_iv, a, 32);
    nx_aes128_ctr(ctr_key, ctr_iv, b, 16);
    nx_aes128_ctr(ctr_key, next_iv, b + 16, 16);
    CHECK_MEM(a, b, 32);
}

int main(void) {
    test_ecb();
    test_ctr();
    TEST_END();
}
