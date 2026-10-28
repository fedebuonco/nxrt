// Per-game shader facts (renderer_facts.m): loaded once from NXRT_SHADER_FACTS or <data root>/shader_facts.tsv.
#pragma once
#include <stddef.h>
#include "shader_facts.h"

const ShaderFact *renderer_facts(size_t *n);
