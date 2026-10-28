// The shader_facts.tsv parser (shader_facts.h). Deliberately small and dependency-free: the file is hand-written
// per game, and a bad row must be a logged skip, never a wrong translation.
#include "shader_facts.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int kind_of(const char *s, size_t n) {
    char b[8];
    if (n >= sizeof b) return -1;
    for (size_t i = 0; i < n; i++) b[i] = (char)tolower((unsigned char)s[i]);
    b[n] = 0;
    if (!strcmp(b, "float") || !strcmp(b, "f") || !strcmp(b, "0")) return 0;
    if (!strcmp(b, "sint") || !strcmp(b, "s") || !strcmp(b, "i") || !strcmp(b, "1")) return 1;
    if (!strcmp(b, "uint") || !strcmp(b, "u") || !strcmp(b, "2")) return 2;
    return -1;
}

bool shader_fact_parse(const char *line, ShaderFact *out) {
    if (!line || !out) return false;
    memset(out, 0, sizeof *out);
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p || *p == '#') return false;
    char *end = NULL;
    unsigned long long hash = strtoull(p, &end, 16);
    if (end == p || *end == 0 || *end == '#') return false;
    if (!isspace((unsigned char)*end)) return false;
    out->hash = (uint64_t)hash;
    p = end;
    bool any = false;
    for (;;) {
        bool comma = false;
        while (*p == ',' || isspace((unsigned char)*p)) {
            if (*p == ',') comma = true;
            p++;
        }
        if (!*p || *p == '#') {
            if (comma) return false;  // a trailing comma
            break;
        }
        char kind = *p++;
        if (kind != 'a' && kind != 'r') return false;
        long index = strtol(p, &end, 10);
        if (end == p || *end != '=') return false;
        if (index < 0 || index >= (kind == 'a' ? SHADER_FACT_ATTRS : SHADER_FACT_RTS)) return false;
        p = end + 1;
        const char *value = p;
        while (*p && *p != ',' && !isspace((unsigned char)*p)) p++;
        int k = kind_of(value, (size_t)(p - value));
        if (k < 0) return false;
        if (kind == 'a') {
            out->attr_mask |= 1u << index;
            out->attr_kind[index] = (uint8_t)k;
        } else {
            out->rt_mask |= (uint8_t)(1u << index);
            out->rt_kind[index] = (uint8_t)k;
        }
        any = true;
    }
    return any;
}

const ShaderFact *shader_fact_find(const ShaderFact *facts, size_t n, uint64_t hash) {
    for (size_t i = 0; i < n; i++)
        if (facts[i].hash == hash) return &facts[i];
    return NULL;
}

void shader_fact_apply(const ShaderFact *f, uint8_t attr_kind[SHADER_FACT_ATTRS], uint8_t rt_kind[SHADER_FACT_RTS]) {
    if (!f) return;
    for (int i = 0; i < SHADER_FACT_ATTRS; i++)
        if (f->attr_mask & (1u << i)) attr_kind[i] = f->attr_kind[i];
    for (int i = 0; i < SHADER_FACT_RTS; i++)
        if (f->rt_mask & (1u << i)) rt_kind[i] = f->rt_kind[i];
}

size_t shader_facts_load(const char *path, ShaderFact *out, size_t cap) {
    if (!path || !out || !cap) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    size_t n = 0;
    while (n < cap && fgets(line, sizeof line, f)) {
        ShaderFact fact;
        if (shader_fact_parse(line, &fact)) out[n++] = fact;
    }
    fclose(f);
    return n;
}
