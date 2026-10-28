// FNV-1a 64 of a translated MSL source: the name of its compiled library in cache/shaders (tools/shader_libs.py).
#pragma once
#include <stdint.h>

uint64_t shader_source_key(const char *s);
