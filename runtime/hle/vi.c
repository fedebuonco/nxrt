// nn::vi (display/layer) and nv:: (graphics driver init), implemented on the host.
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../hle.h"
#include "../kernel.h"
#include "../frame_rate.h"

static uint32_t vsync_r, vsync_w;
static KEvent *vsync_event;
uint32_t vi_vsync_handle(void) { return vsync_r; }
volatile uint64_t g_vsync_count;  // vsyncs signalled so far (runtime/hle/framepace.c, runtime/nvn/pace.c)
volatile uint64_t g_vsync_ns;     // plat_now_ns() of the latest (stored after the count: count >= the vsync it's from)
uint64_t g_native_window;

static void *vsync_thread(void *arg) {
    (void)arg;
    uint64_t period = rt_frame_period_ns();
    plat_thread_realtime(period);
    plat_thread_perf_core(PLAT_PERF_VSYNC);
    uint64_t next = plat_now_ns();
    for (;;) {
        uint64_t wanted = rt_frame_period_ns();
        if (wanted != period) {
            period = wanted;
            plat_thread_realtime(period);
            next = plat_now_ns();
        }
        next += period;
        plat_sleep_until_ns(next);
        uint64_t now = plat_now_ns();
        if (now > next + period * 3) {
            static int warned;
            if (warned++ < 20) LOG("[vi] vsync timer late by %llums (host throttling?)\n", (unsigned long long)((now - next) / 1000000));
            next = now;  // don't burst-signal to catch up
        }
        __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_vsync_ns, now, __ATOMIC_RELEASE);
        pthread_mutex_lock(&g_klock);
        kernel_signal(vsync_event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

HLE_FUNC("_ZN2nn2vi10InitializeEv", vi_initialize) { (void)c; }
HLE_FUNC("_ZN2nn2vi8FinalizeEv", vi_finalize) { (void)c; }

HLE_FUNC("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_close_display) { (void)c; }

HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_destroy_layer) { (void)c; }
HLE_FUNC("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", vi_set_layer_crop) { c->x[0] = 0; }
// How the layer is scaled to the screen (Fire Emblem: Three Houses sets it at boot): the window image is placed by
// the renderer (runtime/nvn/output_scale.c), so it only succeeds. (Run as the SDK's own code it asks the display
// service for a session object the runtime doesn't model, and gets null.)
HLE_FUNC("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", vi_set_layer_scaling_mode) { c->x[0] = 0; }

// Indirect images: what a library applet draws for the game to show (the software keyboard's inline mode, Tomodachi Life
// at boot). There is no applet: the image's memory is what an RGBA8 image needs, and the image is transparent (no
// keyboard drawn). Run as the SDK's own code these ask the display service the runtime doesn't model, and get null.
// (exported as hle_<symbol>, and listed in hle_symbols.txt: the software keyboard calls them from inside sdk, where a
// direct call never passes the dispatcher)
void hle__ZN2nn2vi33GetIndirectImageMemoryRequirementEPmS1_ii(Cpu *c) {
    uint64_t w = (uint32_t)c->x[2], h = (uint32_t)c->x[3];
    *(uint64_t *)(uintptr_t)c->x[0] = (w * h * 4 + 0xFFF) & ~0xFFFull;
    *(uint64_t *)(uintptr_t)c->x[1] = 0x1000;
    c->x[0] = 0;
}
// GetIndirectImage(u64* width, u64* height, void* buffer, u64 size, u64 layer, int w, int h[, crop]): nothing drawn
static void indirect_image(Cpu *c) {
    if (c->x[2] && c->x[3]) memset((void *)(uintptr_t)c->x[2], 0, c->x[3]);
    if (c->x[0]) *(uint64_t *)(uintptr_t)c->x[0] = (uint32_t)c->x[5];
    if (c->x[1]) *(uint64_t *)(uintptr_t)c->x[1] = (uint32_t)c->x[6];
    c->x[0] = 0;
}
void hle__ZN2nn2vi16GetIndirectImageEPmS1_Pvmmii(Cpu *c) { indirect_image(c); }
void hle__ZN2nn2vi16GetIndirectImageEPmS1_Pvmmiiffff(Cpu *c) { indirect_image(c); }
HLE_FUNC("_ZN2nn2vi33GetIndirectImageMemoryRequirementEPmS1_ii", vi_get_indirect_image_memory_requirement) {
    hle__ZN2nn2vi33GetIndirectImageMemoryRequirementEPmS1_ii(c);
}
HLE_FUNC("_ZN2nn2vi16GetIndirectImageEPmS1_Pvmmii", vi_get_indirect_image) { indirect_image(c); }
HLE_FUNC("_ZN2nn2vi16GetIndirectImageEPmS1_Pvmmiiffff", vi_get_indirect_image_crop) { indirect_image(c); }

HLE_FUNC("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window) {
    if (!g_native_window) g_native_window = (uint64_t)hle_alloc(0x100);
    *(uint64_t *)(uintptr_t)c->x[0] = g_native_window;
    c->x[0] = 0;
}

// Reboot: the vsync timer and its event live on (the kernel's handle table doesn't): new handles at the next boot.
RT_ON_RESET(vi_reset) {
    vsync_r = vsync_w = 0;
    g_native_window = 0;  // (HLE heap)
}

HLE_FUNC("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_vsync_event) {
    if (!vsync_event) {
        vsync_event = kernel_create_event_pair(&vsync_w, &vsync_r);
        pthread_t t;
        pthread_create(&t, NULL, vsync_thread, NULL);
    } else if (!vsync_r) {
        vsync_w = kernel_new_handle(vsync_event);
        vsync_r = kernel_new_handle(vsync_event);
    }
    // nn::os::AttachReadableHandleToSystemEvent(event, handle, managed=false, clear_mode=AutoClear)
    c->x[1] = vsync_r;
    c->x[2] = 0;
    c->x[3] = 1;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nv18InitializeGraphicsEPvm", nv_initialize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv16FinalizeGraphicsEv", nv_finalize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_allocator) { (void)c; }
HLE_FUNC("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_dt_allocator) { (void)c; }
