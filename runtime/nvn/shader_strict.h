// The strict shader-preparation gate (NXRT_SHADER_STRICT=1): count first visits and skipped draws; a scripted route with
// either is a coverage failure. Counters only; renderer.m owns the atexit report.
#pragma once
#include <stdbool.h>

void shader_strict_first_visit(void);
void shader_strict_skip(void);
int shader_strict_first_visits(void);
int shader_strict_skips(void);
bool shader_strict_failed(void);
void shader_strict_reset(void);  // (tests)
void shader_strict_exit(void);  // NXRT_SHADER_STRICT: report + exit 1 if a first visit or skip was seen
