#include "archive_file.h"

#include <stdint.h>
#include <stdio.h>

bool archive_file_ok(const char *path) {
    if (!path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t b[4];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n != sizeof b) return false;
    uint32_t m = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return m == 0xfeedfacfu || m == 0xfeedfaceu ||  // MH_MAGIC_64 / MH_MAGIC
           m == 0xcefaedfeu || m == 0xcffaedfeu ||  // the same, other byte order
           m == 0xcafebabeu || m == 0xbebafecau ||
           m == 0xbebafecbu;                        // the archive's own (FAT-like) header
}
