// Program translation/lookup (renderer_programs.m).
#pragma once
#include "renderer_prog.h"

Prog *renderer_get_program(NvnProgram *p, ShaderKey *vkey, ShaderKey *fkey);
bool renderer_translate_program(Prog *pr, const uint8_t *vcode, size_t vmax, const uint8_t *fcode, size_t fmax,
                                ShaderKey *vkey, ShaderKey *fkey, uint64_t vaddr, uint64_t faddr);
uint64_t renderer_prog_content_key(uint64_t vh, uint64_t fh, const ShaderKey *vkey, const ShaderKey *fkey);
Prog *renderer_prep_find_prog(uint64_t k);
void renderer_programs_reset(void);
