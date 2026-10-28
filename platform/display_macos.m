#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "display_match.h"
#include "nx_log.h"

static __unsafe_unretained NSWindow *g_win;
static CGDirectDisplayID g_id;
static CGDisplayModeRef g_orig;
static int g_want;
static bool g_full;

static double mode_hz(CGDisplayModeRef m) {
    double hz = CGDisplayModeGetRefreshRate(m);
    return hz > 0 ? hz : 0;
}

static CGError set_mode(CGDirectDisplayID id, CGDisplayModeRef m) {
    CGDisplayConfigRef cfg;
    CGError r = CGBeginDisplayConfiguration(&cfg);
    if (r != kCGErrorSuccess) return r;
    r = CGConfigureDisplayWithDisplayMode(cfg, id, m, NULL);
    if (r != kCGErrorSuccess) { CGCancelDisplayConfiguration(cfg); return r; }
    return CGCompleteDisplayConfiguration(cfg, kCGConfigureForAppOnly);
}

static void restore_mode(bool same_screen) {
    if (!g_orig) return;
    if (same_screen) {
        CGError r = set_mode(g_id, g_orig);
        NX_LOGI(NX_LOG_APP, "[display] restored %.0f Hz%s\n", mode_hz(g_orig), r == kCGErrorSuccess ? "" : " (failed)");
    }
    CGDisplayModeRelease(g_orig);
    g_orig = NULL;
    g_id = kCGNullDirectDisplay;
}

static void apply(void) {
    static int off = -1;
    if (off < 0) {
        const char *e = getenv("NXRT_SCREEN_MATCH");
        off = e && !strcmp(e, "0");
        if (off) NX_LOGI(NX_LOG_APP, "[display] screen matching off (NXRT_SCREEN_MATCH=0)\n");
    }
    if (!g_win) return;
    NSScreen *scr = g_win.screen;
    bool full = (g_win.styleMask & NSWindowStyleMaskFullScreen) != 0;
    CGDirectDisplayID id = scr ? [[scr.deviceDescription objectForKey:@"NSScreenNumber"] unsignedIntValue] : kCGNullDirectDisplay;
    if (g_orig && (!full || id != g_id)) restore_mode(id == g_id);
    if (off || !full || g_want <= 0 || !scr) return;

    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(id);
    if (!cur) return;
    double hz = mode_hz(cur);
    if (hz <= 0 || display_hz_multiple(hz, g_want)) {
        if (hz > 0) NX_LOGI(NX_LOG_APP, "[display] fullscreen %.0f Hz already a multiple of %d fps\n", hz, g_want);
        CGDisplayModeRelease(cur);
        return;
    }

    CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, (__bridge CFDictionaryRef)@{
        (__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes: @YES
    });
    DisplayModeInfo *infos = NULL;
    int n = modes ? (int)CFArrayGetCount(modes) : 0;
    if (n > 0) infos = calloc((size_t)n, sizeof *infos);
    for (int i = 0; i < n; i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        infos[i] = (DisplayModeInfo){
            .hz = mode_hz(m),
            .width = (uint32_t)CGDisplayModeGetWidth(m),
            .height = (uint32_t)CGDisplayModeGetHeight(m),
            .pixel_width = (uint32_t)CGDisplayModeGetPixelWidth(m),
            .pixel_height = (uint32_t)CGDisplayModeGetPixelHeight(m),
            .usable = CGDisplayModeIsUsableForDesktopGUI(m),
        };
    }
    DisplayModeInfo cur_info = {
        .hz = hz,
        .width = (uint32_t)CGDisplayModeGetWidth(cur),
        .height = (uint32_t)CGDisplayModeGetHeight(cur),
        .pixel_width = (uint32_t)CGDisplayModeGetPixelWidth(cur),
        .pixel_height = (uint32_t)CGDisplayModeGetPixelHeight(cur),
        .usable = true,
    };
    int pick = display_pick_mode(infos, n, &cur_info, g_want);
    if (pick < 0) {
        NX_LOGI(NX_LOG_APP, "[display] screen at %.0f Hz has no mode at a multiple of %d Hz\n", hz, g_want);
    } else {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, pick);
        CGError r = set_mode(id, m);
        NX_LOGI(NX_LOG_APP, "[display] screen at %.0f Hz %s %.0f Hz for %d fps%s\n", hz,
                r == kCGErrorSuccess ? "switched to" : "could not switch to", infos[pick].hz, g_want,
                r == kCGErrorSuccess ? "" : " (NXRT_SCREEN_MATCH=0 keeps it)");
        if (r == kCGErrorSuccess && !g_orig) {
            g_orig = CGDisplayModeRetain(cur);
            g_id = id;
        }
    }
    free(infos);
    if (modes) CFRelease(modes);
    CGDisplayModeRelease(cur);
}

void plat_display_bind(void *nswindow) {
    g_win = (__bridge NSWindow *)nswindow;
}

void plat_display_tick(int fps, bool fullscreen) {
    if (!g_win) return;
    if (fps == g_want && fullscreen == g_full) return;
    g_want = fps;
    g_full = fullscreen;
    dispatch_async(dispatch_get_main_queue(), ^{ apply(); });
}

void plat_display_restore(void) {
    dispatch_async(dispatch_get_main_queue(), ^{ restore_mode(true); });
}
