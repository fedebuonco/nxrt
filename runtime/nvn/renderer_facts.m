// Per-game shader facts (split from renderer.m).
#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <stdio.h>

#include "paths.h"
#include "renderer_facts.h"
#include "rt.h"

static ShaderFact *g_facts;
static size_t g_nfacts;
static bool g_facts_loaded;

const ShaderFact *renderer_facts(size_t *n) {
    if (!g_facts_loaded) {
        g_facts_loaded = true;
        const char *p = getenv("NXRT_SHADER_FACTS");
        char path[1200];
        if (!p || !*p) {
            const char *root = rt_data_root();
            if (!root) { *n = 0; return NULL; }
            snprintf(path, sizeof path, "%s/shader_facts.tsv", root);
            p = path;
        }
        g_facts = calloc(256, sizeof *g_facts);
        if (g_facts) {
            g_nfacts = shader_facts_load(p, g_facts, 256);
            if (g_nfacts) LOG("[shader] %zu facts from %s\n", g_nfacts, p);
        }
    }
    *n = g_nfacts;
    return g_facts;
}
