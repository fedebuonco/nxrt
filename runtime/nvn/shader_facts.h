// Per-game shader translation facts: what the shader code cannot tell (integer vertex attributes / render targets).
// One line of shader_facts.tsv: <hex shader hash><TAB>a1=float,a2=uint,r0=uint
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SHADER_FACT_ATTRS 32
#define SHADER_FACT_RTS 8

typedef struct {
    uint64_t hash;  // shader_stage_hash of the stage the facts apply to
    uint32_t attr_mask;
    uint8_t attr_kind[SHADER_FACT_ATTRS];  // 0 float, 1 signed int, 2 unsigned int (ShaderKey.attr_kind)
    uint8_t rt_mask;
    uint8_t rt_kind[SHADER_FACT_RTS];  // 0 float, 1 signed int, 2 unsigned int (ShaderKey.rt_kind)
} ShaderFact;

// Parse one line. false for a comment, a blank line, or a malformed fact (the caller logs and skips it).
bool shader_fact_parse(const char *line, ShaderFact *out);
// Load a file, keeping at most cap facts; returns the number kept (0 when it cannot be opened). Malformed rows are
// skipped, not guessed at.
size_t shader_facts_load(const char *path, ShaderFact *out, size_t cap);
// Linear lookup by hash; NULL when the hash has no facts.
const ShaderFact *shader_fact_find(const ShaderFact *facts, size_t n, uint64_t hash);
// Overwrite only the kinds the fact pins; the key's other entries stay as the draw state built them.
void shader_fact_apply(const ShaderFact *f, uint8_t attr_kind[SHADER_FACT_ATTRS], uint8_t rt_kind[SHADER_FACT_RTS]);
