// The few runtime symbols runtime/nvn/vulkan/shaders.c needs that tools/test_vk_pipelines.c doesn't define
// (the shader/pipeline test links shaders.c + translate.c alone, without the rest of the runtime).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "nx_log.h"
#include "rt.h"

void rt_fatal_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void nx_log_v(NxLogLevel level, NxLogCategory category, const char *fmt, va_list ap) {
    (void)level, (void)category;
    vfprintf(stderr, fmt, ap);
}

void vk_device_fault_report(void) {}

// (runtime/nvn/smaa_data.c builds this from SMAA.hlsl at make time; the pipeline test creates no SMAA pass)
const char smaa_hlsl_src[] = "";
