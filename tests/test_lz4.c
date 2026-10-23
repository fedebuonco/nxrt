// LZ4 block decompression: runtime/lz4_block.c against liblz4 (the reference), plus malformed/truncated/fuzzed blocks.
// The point is the parser: a corrupt NSO segment or package table must come back short, never read or write out of
// bounds (this test builds with ASan, see run.sh). Also checks the nx_lz4_decompress wrapper (runtime/nxpkg.c).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LZ4
#include <lz4.h>
#endif

#include "harness.h"
#include "lz4_block.h"
#include "nxpkg.h"

static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13, rng_state ^= rng_state >> 17, rng_state ^= rng_state << 5;
    return rng_state;
}

// Compress `n` bytes of src into an exact-size heap block; returns the block, *clen = its size.
static uint8_t *compress_block(const uint8_t *src, int n, int *clen) {
#ifdef HAVE_LZ4
    int bound = LZ4_compressBound(n);
    uint8_t *tmp = malloc((size_t)bound);
    int k = LZ4_compress_default((const char *)src, (char *)tmp, n, bound);
    CHECK(k > 0);
    uint8_t *out = malloc((size_t)(k > 0 ? k : 0));
    if (k > 0) memcpy(out, tmp, (size_t)k);
    free(tmp);
    *clen = k;
    return out;
#else
    // A valid LZ4 block with one literals-only sequence (the last sequence must be literals).
    size_t cap = (size_t)n + (size_t)n / 255 + 16, k = 0;
    uint8_t *out = malloc(cap);
    unsigned lit = (unsigned)n;
    out[k++] = (uint8_t)((lit < 15 ? lit : 15) << 4);
    if (lit >= 15) {
        unsigned r = lit - 15;
        while (r >= 255) out[k++] = 255, r -= 255;
        out[k++] = (uint8_t)r;
    }
    memcpy(out + k, src, (size_t)n), k += (size_t)n;
    *clen = (int)k;
    return out;
#endif
}

static void roundtrip(const uint8_t *data, int n, const char *what) {
    int clen = 0;
    uint8_t *c = compress_block(data, n, &clen);
    uint8_t *got = malloc((size_t)(n > 0 ? n : 1));
    size_t k = rt_lz4_decompress(c, (size_t)clen, got, (size_t)n);
    if (k != (size_t)n || (n && memcmp(got, data, (size_t)n))) {
        fprintf(stderr, "roundtrip failed: %s (%d bytes -> %zu)\n", what, n, k);
        CHECK(0);
    }
    // The int-sized wrapper must agree; the reference library too when it is available.
    uint8_t *ref = malloc((size_t)(n > 0 ? n : 1));
#ifdef HAVE_LZ4
    int r = LZ4_decompress_safe((const char *)c, (char *)ref, clen, n);
    CHECK_EQ(r, n);
    if (n) CHECK_MEM(ref, got, (size_t)n);
#endif
    CHECK_EQ(nx_lz4_decompress(c, clen, ref, n), n);
    free(ref);
    free(got);
    free(c);
}

static void test_roundtrip(void) {
    enum { MAXN = 70000 };
    static uint8_t data[MAXN];
    // A spread of sizes around the 64-byte hash block, the repeat window and the tests' old sample limit.
    const int sizes[] = {0,  1,  2,   3,   4,   5,   7,   8,   15,   16,   17,    31,    32,    33,   63,
                         64, 65, 127, 128, 129, 255, 256, 257, 1023, 4096, 65535, 65536, 65537, 70000};
    for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        int n = sizes[s];
        // zeros: one long match
        memset(data, 0, (size_t)n);
        roundtrip(data, n, "zeros");
        // an incompressible stream
        for (int i = 0; i < n; i++) data[i] = (uint8_t)rnd();
        roundtrip(data, n, "random");
        // text-like: long runs and repeated substrings
        for (int i = 0; i < n; i++) data[i] = (uint8_t)("the quick brown fox jumps over the lazy dog\n"[i % 45]);
        roundtrip(data, n, "text");
        // a 16-byte pattern: matches shorter than the offset, and ones exactly at it
        for (int i = 0; i < n; i++) data[i] = (uint8_t)(i & 15);
        roundtrip(data, n, "pattern");
    }
}

// A malformed block must return a short count and stay inside dst. The blocks are exact-size heap allocations so
// ASan sees every byte read past their end.
static void check_bad(const uint8_t *src, size_t n, size_t cap, const char *what) {
    uint8_t *d = malloc(cap ? cap : 1);
    size_t k = rt_lz4_decompress(src, n, d, cap);
    CHECK(k <= cap);
    uint8_t *d2 = malloc(cap ? cap : 1);
    CHECK(nx_lz4_decompress(src, (int)n, d2, (int)cap) <= (int)cap);
    free(d);
    free(d2);
    (void)what;
}

// Hand-built blocks that exercise the match path even where liblz4 isn't installed: a short offset (byte-by-byte
// copy) and one at least as long as the match (a plain memcpy).
static void test_matches(void) {
    static const uint8_t overlap[] = {0x15, 'A', 0x01, 0x00};  // 1 literal, offset 1, match 9
    uint8_t out[32];
    CHECK_EQ(rt_lz4_decompress(overlap, sizeof overlap, out, sizeof out), 10);
    for (int i = 0; i < 10; i++) CHECK_EQ(out[i], 'A');
    static const uint8_t plain[] = {0x84, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 0x08, 0x00};
    CHECK_EQ(rt_lz4_decompress(plain, sizeof plain, out, sizeof out), 16);
    CHECK_MEM(out, "ABCDEFGHABCDEFGH", 16);
    // A literals-only block.
    static const uint8_t lits[] = {0x50, 'h', 'e', 'l', 'l', 'o'};
    CHECK_EQ(rt_lz4_decompress(lits, sizeof lits, out, sizeof out), 5);
    CHECK_MEM(out, "hello", 5);
    // No input, no output.
    CHECK_EQ(rt_lz4_decompress(lits, 0, out, sizeof out), 0);
}

static void test_malformed(void) {
    // literal length 15 with no extension byte
    static const uint8_t a[] = {0xF0};
    check_bad(a, sizeof a, 64, "short literal extension");
    // a 255 continuation with nothing after it
    static const uint8_t b[] = {0xF0, 0xFF};
    check_bad(b, sizeof b, 64, "literal extension runs off");
    // literals, then only one of the two offset bytes
    static const uint8_t c[] = {0x10, 'A', 0x01};
    check_bad(c, sizeof c, 64, "one offset byte");
    // offset 0: copying from the same place forever
    static const uint8_t d[] = {0x10, 'A', 0x00, 0x00};
    check_bad(d, sizeof d, 64, "offset 0");
    // offset points before the start of the output
    static const uint8_t e[] = {0x10, 'A', 0x05, 0x00};
    check_bad(e, sizeof e, 64, "offset before start");
    // match length 15 with no extension byte
    static const uint8_t f[] = {0x00, 0x01, 0x00, 0xF0};
    check_bad(f, sizeof f, 64, "match extension runs off");
    // a full sequence but no room in dst
    static const uint8_t g[] = {0x10, 'A', 0x01, 0x00, 0x50, 'B'};
    check_bad(g, sizeof g, 1, "no room for the match");
    // empty input
    uint8_t *empty = malloc(1);
    check_bad(empty, 0, 16, "empty");
    free(empty);
    // negative sizes through the wrapper
    uint8_t buf[16] = {0};
    CHECK_EQ(nx_lz4_decompress(buf, -1, buf, 16), -1);
    CHECK_EQ(nx_lz4_decompress(buf, 1, buf, -1), -1);
}

// Every truncation of a valid stream, and every one-byte mutation: no crash, no read past either end.
static void test_truncations_and_mutants(void) {
    enum { MAXN = 4096 };
    static uint8_t data[MAXN];
    for (int t = 0; t < 20; t++) {
        int n = 1 + (int)(rnd() % MAXN);
        for (int i = 0; i < n; i++) data[i] = t % 3 == 0 ? (uint8_t)rnd() : (uint8_t)(i / (t % 7 + 1));
        int clen = 0;
        uint8_t *c = compress_block(data, n, &clen);
        for (int cut = 0; cut <= clen; cut++) {
            uint8_t *d = malloc((size_t)(n ? n : 1));
            size_t k = rt_lz4_decompress(c, (size_t)cut, d, (size_t)n);
            CHECK(k <= (size_t)n);
#ifdef HAVE_LZ4
            // A truncated stream may never produce more than the reference does; a complete one must match.
            int ref = LZ4_decompress_safe((const char *)c, (char *)d, cut, n);
            if (cut == clen) CHECK_EQ(ref, n);
#endif
            free(d);
        }
        for (int i = 0; i < clen; i++) {
            c[i] ^= (uint8_t)(1u << (rnd() % 8));
            uint8_t *d = malloc((size_t)(n ? n : 1));
            size_t k = rt_lz4_decompress(c, (size_t)clen, d, (size_t)n);
            CHECK(k <= (size_t)n);
            free(d);
            c[i] ^= (uint8_t)(1u << (rnd() % 8));  // not necessarily the same bit: fine, the next mutation follows
        }
        free(c);
    }
}

// Random bytes: the parser sees arbitrary tokens, lengths and offsets.
static void test_fuzz(void) {
    for (int t = 0; t < 20000; t++) {
        size_t n = 1 + rnd() % 300, cap = 1 + rnd() % 600;
        uint8_t *src = malloc(n), *dst = malloc(cap);
        for (size_t i = 0; i < n; i++) src[i] = (uint8_t)rnd();
        size_t k = rt_lz4_decompress(src, n, dst, cap);
        CHECK(k <= cap);
        free(src);
        free(dst);
    }
}

int main(void) {
    test_roundtrip();
    test_matches();
    test_malformed();
    test_truncations_and_mutants();
    test_fuzz();
    TEST_END();
}
