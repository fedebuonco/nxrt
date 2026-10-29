// NVN entry points: every NVN function is looked up by name and bound to a host stub.
#include <string.h>

#include "../hle.h"

// The last NVN calls (names), for a hang dump: a frame that stops mid-way shows which call it never came back from.
#define NVN_TRACE_N 48
static const char *nvn_trace[NVN_TRACE_N];
static unsigned nvn_trace_i;
static void nvn_trace_push(const char *name) {
    nvn_trace[nvn_trace_i++ % NVN_TRACE_N] = name;
}
void nvn_trace_dump(void) {
    unsigned n = nvn_trace_i < NVN_TRACE_N ? nvn_trace_i : NVN_TRACE_N;
    LOG("[nvn] last %u calls (oldest first):\n", n);
    for (unsigned k = 0; k < n; k++) LOG("[nvn]   %s\n", nvn_trace[(nvn_trace_i - n + k) % NVN_TRACE_N]);
}

// Every entry point goes through one trampoline so the trace above sees them all (the name comes from the stub
// address, which is what the guest calls).
static void nvn_trampoline(Cpu *c) {
    const char *name = hle_name_at(c->pc);
    if (name) nvn_trace_push(name);
    HostFn fn = name ? hle_lookup_impl(name) : NULL;
    if (fn) { fn(c); return; }
    LOG("[nvn] call to %s with no host implementation\n", name ? name : "?");
}

static uint64_t nvn_proc(const char *name) {
    if (!name || !*name) return 0;
    HostFn fn = hle_lookup_impl(name);
    if (!fn && ENV("NXRT_NVN_LOG")) LOG("[nvn] no host for %s\n", name);
    (void)fn;
    return hle_register(name, nvn_trampoline);  // (nvn_trampoline looks the implementation up by name)
}

HLE_FUNC("nvnBootstrapLoader", nvn_bootstrap_loader) {
    const char *name = (const char *)(uintptr_t)c->x[0];
    c->x[0] = nvn_proc(name);
}

// void* nvnDeviceGetProcAddress(const NVNdevice*, const char*)
HLE_FUNC("nvnDeviceGetProcAddress", nvn_device_get_proc_address) {
    const char *name = (const char *)(uintptr_t)c->x[1];
    c->x[0] = nvn_proc(name);
}
