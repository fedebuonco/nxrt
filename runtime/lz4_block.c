// LZ4 block decompression (see lz4_block.h). The block format: a token, the literals, a 2-byte match offset and the match
// length, repeated; the last sequence has literals only. Every length is extended by 255-valued bytes while the input
// lasts. This is LZ4_decompress_safe's logic with the size checks kept ahead of every read and write: NSO sections,
// shader archives and a package's CompressedStorage are all LZ4, and a corrupt one must fail short, not crash.
#include "lz4_block.h"

#include <string.h>

size_t rt_lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {
    const uint8_t *ip = src, *iend = src + srclen;
    uint8_t *op = dst, *oend = dst + dstlen;
    while (ip < iend) {
        unsigned tok = *ip++;
        size_t lit = tok >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (ip >= iend) return (size_t)(op - dst);
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if ((size_t)(iend - ip) < lit || (size_t)(oend - op) < lit) return (size_t)(op - dst);
        memcpy(op, ip, lit);
        op += lit, ip += lit;
        if (ip >= iend) break;  // the last sequence has literals only
        if (iend - ip < 2) return (size_t)(op - dst);
        size_t off = ip[0] | (size_t)ip[1] << 8;
        ip += 2;
        size_t ml = tok & 15;
        if (ml == 15) {
            unsigned b;
            do {
                if (ip >= iend) return (size_t)(op - dst);
                b = *ip++;
                ml += b;
            } while (b == 255);
        }
        ml += 4;
        if (!off || off > (size_t)(op - dst) || (size_t)(oend - op) < ml) return (size_t)(op - dst);
        const uint8_t *m = op - off;
        if (off >= ml)
            memcpy(op, m, ml), op += ml;
        else
            while (ml--) *op++ = *m++;  // overlapping: byte by byte
    }
    return (size_t)(op - dst);
}
