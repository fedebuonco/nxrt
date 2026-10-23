// LZ4 block decompression, shared by the NSO loader (runtime/loader.c), the shader archives (runtime/nvn/shader_prep.c)
// and the game package's compressed RomFS (runtime/nxpkg.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decompress one LZ4 block into dst (at most dstlen bytes). Returns the bytes written: a truncated or malformed block
// stops early and the caller sees a short result, as with LZ4_decompress_safe. Never reads src past srclen or writes
// dst past dstlen, whatever the input says.
size_t rt_lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);

#ifdef __cplusplus
}
#endif
