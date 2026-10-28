#include "shader_source_key.h"

uint64_t shader_source_key(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        h ^= *p;
        h *= 0x100000001b3ull;
    }
    return h;
}
