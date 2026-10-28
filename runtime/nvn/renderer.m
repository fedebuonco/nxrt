// Executes recorded NVN command lists with Metal.
#include <pthread.h>
#include <dirent.h>
#include <unistd.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <sys/stat.h>
#include <SDL3/SDL_timer.h>

#include "maxwell/shader.h"
#include "nvn.h"
#include "paths.h"
#include "shader_facts.h"
#include "shader_source_key.h"
#include "shader_rec_norm.h"
#include "archive_file.h"
#include "renderer_archive.h"
#include "renderer_context.h"
#include "renderer_cache.h"
#include "renderer_prog.h"
#include "renderer_metal4.h"
#include "renderer_pipelines.h"
#include "renderer_pass.h"
#include "renderer_textures.h"
#include "renderer_clears.h"
#include "renderer_formats.h"
#include "renderer_samplers.h"
#include "renderer_programs.h"
#include "renderer_facts.h"
#include "renderer_prep.h"
#include "renderer_compute.h"
#include "renderer_bind.h"
#include "renderer_snapshots.h"
#include "renderer_copies.h"
#include "renderer.h"
#include "nxgame.h"

#define dev (renderer_context()->dev)
#define cmdbuf (renderer_context()->cmdbuf)
#define enc (renderer_context()->enc)
#define clear_pipes (renderer_context()->clear_pipes)
#define cache (renderer_context()->cache)
#define st (renderer_context()->st)
#define g_passes_in_cb (renderer_context()->passes_in_cb)
#define pass_sx (renderer_context()->pass_sx)
#define pass_sy (renderer_context()->pass_sy)
#define pass_rt_w (renderer_context()->pass_rt_w)
#define pass_rt_h (renderer_context()->pass_rt_h)
#define vp_sx (renderer_context()->vp_sx)
#define vp_sy (renderer_context()->vp_sy)
#define vp_ox (renderer_context()->vp_ox)
#define vp_oy (renderer_context()->vp_oy)
#define pass_rt (renderer_context()->pass_rt)
#define opening_pass (renderer_context()->opening_pass)
#define tex_sc (renderer_context()->tex_sc)
#define tex_fr (renderer_context()->tex_fr)
#define tex_wh (renderer_context()->tex_wh)
#define draw_watched (renderer_context()->draw_watched)

enum { RECOLOUR_TINT_FLOATS = 16 };
#include "pace.h"
#include "shader_prep.h"
#include "shader_strict.h"
#include "slowframe.h"

// (stats) GPU wait time by cause: 0 = guest fence (nvnSyncWait), 1 = full flush (CPU reads GPU results / ring), 2 = sync all
static uint64_t stat_wait_by[3], stat_wait_n[3];
static uint64_t stat_clears_as_load, stat_clear_passes;  // (deferred clears)
void renderer_stat_clear_pass(void) { stat_clear_passes++; }
static uint64_t gp_draws_all;  // (NXRT_GPU_PASSES: draws so far, never reset like stat_draws)
static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes;
static double stat_gpu_s;
// GPU busy time (dynamic resolution: output_scale.c; the "gpu" of the [metal] stats line), ns so far: the game's
// command buffers, the window image's (metal.m backend_present), and the plain sum of their spans (diagnostics)
uint64_t g_gpu_total_ns, g_gpu_win_ns, g_gpu_sum_ns;
// Frame pacing (pace.c): the GPU's busy time so far, the game's command buffers and the window image's (renderer_gpu_busy).
int64_t pace_gpu_ns(void) {
    return (int64_t)(__atomic_load_n(&g_gpu_total_ns, __ATOMIC_RELAXED) + __atomic_load_n(&g_gpu_win_ns, __ATOMIC_RELAXED));
}
// (forward: texture-lifetime cleanup and stats, defined further down)
static uint64_t op_counts[CMD_COUNT];
bool g_debug_frame;
uint64_t renderer_exec_frame(void);  // (metal.m: the frame whose commands are being translated)
extern char g_capture_dir[512];  // (NXRT_DUMP_TRIGGER: a capture, lighter than a debug frame)
void renderer_save_texture(id<MTLTexture> tex, const char *path);
void renderer_save_texture_alpha(id<MTLTexture> tex, const char *path);
void renderer_save_game_texture(NvnTexture *t, id<MTLTexture> tex, const char *path, const char *alpha_path);  // (metal.m)
static char stats_text[320];

// ------------------------------------------------------------------ state
// state: see renderer_state.h (held by RendererContext)
extern uint32_t g_window_origin_lower_left_hack;
uint32_t nvn_window_origin(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

static double stat_compile_ms;  // time spent building shaders/pipelines (first-use stutter)
static int stat_compiles;
static inline double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
#define TIMED_COMPILE(expr) ({ double t0_ = now_ms(); __auto_type r_ = (expr); stat_compile_ms += now_ms() - t0_; stat_compiles++; \
                               slow_note_build("pipeline", (uint64_t)((now_ms() - t0_) * 1e6)); r_; })

// NXRT_LOG_SKIPS=1: the first time each skip site leaves a draw out, which one (its line here) and the program
static void skip_note(int line);
static bool gpu_passes(void);
static void gp_log(void);
static uint64_t stat_counters_gpu;  // timing counters given the GPU's time (the game's 30 / 60 fps choice)
const char *renderer_stats(void) {
    static int dumps;
    if (dumps++ < 3) {
        LOG("[metal] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, op_counts[i]);
        LOG("\n");
    }
    extern int g_tex_live;
    extern uint64_t g_tex_live_bytes;
    struct task_vm_info vmi;
    mach_msg_type_number_t vn = TASK_VM_INFO_COUNT;
    uint64_t foot = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmi, &vn) == KERN_SUCCESS ? vmi.phys_footprint : 0;
    snprintf(stats_text, sizeof stats_text, "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, gpu %.0fms, wait %.0fms, compile %d/%.0fms, fbcopy %llu | tex %d (%.0f MB) views %d snaps %lu mem %.0f MB | waits fence %.0fms/%llu flush %.0fms/%llu all %.0fms/%llu | deferred: clears as load %llu, clear passes %llu | counters gpu %llu | pace %s, %llu frames over", stat_draws, stat_skipped,
             stat_passes, stat_dispatches, stat_flushes, stat_gpu_s * 1000.0, (double)stat_wait_ns / 1e6 * 125.0 / 3.0, stat_compiles, stat_compile_ms, renderer_feedback_copies(),
             g_tex_live, g_tex_live_bytes / 1048576.0, renderer_views_live(), (unsigned long)renderer_snap_copy_count(), foot / 1048576.0, stat_wait_by[0] / 24e3, (unsigned long long)stat_wait_n[0], stat_wait_by[1] / 24e3, (unsigned long long)stat_wait_n[1], stat_wait_by[2] / 24e3, (unsigned long long)stat_wait_n[2], (unsigned long long)stat_clears_as_load, (unsigned long long)stat_clear_passes,
             (unsigned long long)stat_counters_gpu, pace_is_slow() ? "30" : "60", (unsigned long long)pace_frames_over());
    if (gpu_passes()) gp_log();
    stat_counters_gpu = 0;
    stat_compiles = 0;

    stat_compile_ms = 0;
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    memset(stat_wait_by, 0, sizeof stat_wait_by); memset(stat_wait_n, 0, sizeof stat_wait_n);
    stat_gpu_s = 0;
    stat_clears_as_load = stat_clear_passes = 0;
    return stats_text;
}
void renderer_skip_note(int line) { skip_note(line); }


void renderer_note_compile(double ms) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    stat_compiles++;
    stat_compile_ms += ms;
    pthread_mutex_unlock(&lock);
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

// The per-draw tint (maxwell/translate.c): only for a game that tints draws or instances. Set before anything is
// translated (shader preparation runs on the start screen, before renderer_init): nx_game is static data.
__attribute__((constructor)) static void tint_for_game(void) {
    extern bool maxwell_draw_tint;
    maxwell_draw_tint = NX_HOOK(draw_tint) || NX_HOOK(instance_tint);
}

// Sleep: finish the GPU work in flight before the machine goes down; wake: log. Audio and a full restore are the next
// part of A9 (they need a device sleep to verify).
static void sleep_flush(bool waking) {
    extern void audio_suspend(bool suspend);
    if (waking) {
        LOG("[sleep] woke: resuming audio\n");
        audio_suspend(false);
        return;
    }
    LOG("[sleep] will sleep: pausing audio, flushing the GPU\n");
    audio_suspend(true);
    renderer_flush();
}

void renderer_init(id<MTLDevice> d) {
    renderer_context_init(d);
    renderer_archive_init(d);
    st_defaults();
}

static id<MTLCommandBuffer> cb(void) {
    if (!cmdbuf) cmdbuf = [renderer_queue() commandBuffer];
    return cmdbuf;
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
void renderer_texture_fraction(NvnTexture *t, float *fx, float *fy);  // (metal.m: the part the contents occupy)
void renderer_get_scale(float *sx, float *sy);
bool renderer_texture_presented(NvnTexture *t);
// Widescreen UI: 2D draws into the swapchain image use a centred 16:9 region (uniform scale) instead of
// the stretched wide mapping. vp_* is the mapping for the current draw: physical = guest * s + off.
#define RSCALE_BUFFER_INDEX 18

static int debug_pass_no;
int renderer_debug_pass_no(void) { return debug_pass_no; }
// Capture tool: NXRT_CAPTURE_WATCH=<hex>[,<hex>...] names textures (by address) or fragment programs (by code
// address). In a capture's first frame, each draw that samples a watched texture or runs a watched program gets its
// target saved right after it (draw_<n>_<pass>_<addr>.png: the pass is ended there and resumed, loading), and the
// program's MSL saved once (<fs>_fs.metal). The pass snapshots are named by the pass's own target (pass_rt).
static int debug_draw_no;
static bool capture_first_frame(void) {
    extern uint64_t g_debug_exec_frame;
    return g_debug_frame && g_capture_dir[0] && renderer_exec_frame() == g_debug_exec_frame;
}
bool renderer_capture_watches(uint64_t a) {
    static uint64_t w[16];
    static int n = -1;
    if (n < 0) {
        n = 0;
        const char *e = getenv("NXRT_CAPTURE_WATCH");
        while (e && *e && n < 16) {
            char *end;
            w[n] = strtoull(e, &end, 16);
            if (end == e) break;
            n++;
            e = *end == ',' ? end + 1 : end;
        }
    }
    for (int i = 0; i < n; i++)
        if (a && w[i] == a) return true;
    return false;
}
// Attachment traffic. A tile GPU loads and stores every attachment of every pass, and the game clears each target
// with a full clear before drawing it: 13 of the 45 passes of a Field_11C frame were clear quads alone, and the game
// also re-sets the targets it already has between draw groups, which ended the pass (store + load of everything,
// 3840x2160 depth, stencil and colour at 2x). Measured, Field_11C: 488 -> 343 MB of attachment traffic per frame at 1x,
// 1701 -> 1297 MB at 2x, 45 -> 42 passes, picture unchanged. ~2 ms a frame on a 68 GB/s M1 Air.
// - A full clear is recorded on its texture and becomes the load action of the next pass that binds the texture as a
//   target (no load, no clear quad), whatever comes in between (other targets, dispatches, submits: the clear stays
//   with the texture). It gets a pass of its own, which only stores, only if the texture is read first (sampled,
//   copied, presented). That took the remaining 12 clear-only passes of the frame (65 MB at 1x). NXRT_MTL_LOAD_CLEAR=0:
//   clears are quads again; NXRT_MTL_DEFER_CLEAR=0: a recorded clear is flushed as soon as its targets change.
// - SetRenderTargets with the targets already bound keeps the pass open. NXRT_MTL_RT_DEDUP=0: ends it as before.
// (Don't-care depth / stencil stores were tried here too: the next pass loads the depth in nearly every case, only ~2
// of the ~8 stores a frame precede a clear of the texture, and a guess learned from the previous frame read stale
// depth at scene changes. Not worth a wrong frame.)
void renderer_texture_will_read(NvnTexture *t) { renderer_td_flush(t); }  // (metal.m: the presented image)


static void ring_reset(void);
// Submitted work runs asynchronously; last_cb is the most recently committed command buffer (the queue
// executes in order, so waiting for it waits for everything before it). Other guest threads (nvnSyncWait)
// read last_cb, so it is guarded by cb_lock; everything else here runs on the submitting thread.
static id<MTLCommandBuffer> last_cb;
static pthread_mutex_t cb_lock = PTHREAD_MUTEX_INITIALIZER;
// Fences: each committed command buffer gets a sequence number; a fence remembers the number of the submission
// that contains it, and nvnSyncWait waits only for that one (not for everything submitted since), so the game's
// CPU work on frame N+1 overlaps the GPU's frame N as on the Switch.
#define CB_HIST 256
static uint64_t submit_seq;
static id<MTLCommandBuffer> cb_hist[CB_HIST];

// ---- the game's GPU timing counters (nvnCommandBufferReportCounter, two per frame)
// The game times its GPU work with these and takes its 30 fps mode when frames look slow (pace.c). They used to get
// the host clock at translation time: our command translation (~2 ms however long the GPU took), so a Mac that
// couldn't hold 60 kept the game in its 60 fps mode - slow motion at 30. Now the command buffer is split at each
// counter (Apple GPUs have no timestamps inside a buffer), and once that's done the counter gets the GPU's time
// there: the end of the buffer before it, or the start of the one after it if the GPU sat idle in between. Until
// then the host clock (the game waits on its fence before reading them; resolved after each wait).
// NXRT_MTL_COUNTER=cpu: the host clock only.
#define CTR_MAX 64
static struct { uint64_t *out; const uint64_t *start; uint64_t before, after; } ctr_pend[CTR_MAX];
static int nctr;
static pthread_mutex_t ctr_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t ns_to_gpu_ticks(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * 384 / 625); }  // (614.4 MHz)

static void counters_resolve(void) {
    pthread_mutex_lock(&ctr_lock);
    pthread_mutex_lock(&cb_lock);
    int k = 0;
    for (int i = 0; i < nctr; i++) {
        uint64_t bs = ctr_pend[i].before, as = ctr_pend[i].after;
        if (submit_seq - bs >= CB_HIST || (as <= submit_seq && submit_seq - as >= CB_HIST)) continue;  // (gone)
        id<MTLCommandBuffer> b = bs ? cb_hist[bs % CB_HIST] : nil;
        id<MTLCommandBuffer> a = as <= submit_seq ? cb_hist[as % CB_HIST] : nil;
        // both sides done: a buffer not committed yet (a frame's first chunk still being translated) would leave a
        // start counter at the previous frame's end - backend_execute commits one at the end of each list
        bool done = as <= submit_seq && (!b || b.status >= MTLCommandBufferStatusCompleted) &&
                    (!a || a.status >= MTLCommandBufferStatusCompleted);
        if (!done) {
            ctr_pend[k++] = ctr_pend[i];
            continue;
        }
        double t = b ? b.GPUEndTime : 0;
        if (a && a.GPUStartTime > t) t = a.GPUStartTime;
        if (t > 0) {
            uint64_t *out = ctr_pend[i].out;
            out[1] = pace_end_counter(ctr_pend[i].start, out, ns_to_gpu_ticks((uint64_t)(t * 1e9)));  // (host time base)
            stat_counters_gpu++;
        }
    }
    nctr = k;
    pthread_mutex_unlock(&cb_lock);
    pthread_mutex_unlock(&ctr_lock);
}

// A counter waits for the buffer after it: true when one ended the list just translated (backend_execute commits
// one then, empty, so the counter isn't held until the next list).
static bool counters_want_next(void) {
    pthread_mutex_lock(&ctr_lock);
    bool want = false;
    for (int i = 0; i < nctr && !want; i++) want = ctr_pend[i].after > submit_seq;
    pthread_mutex_unlock(&ctr_lock);
    return want;
}

static int g_wait_kind;
static uint64_t cum_wait_ticks;  // GPU waits so far (mach ticks; never reset)
static void wait_cb(id<MTLCommandBuffer> c) {
    if (!c) return;
    uint64_t t0 = mach_absolute_time();
    extern void rq_watch(const char *what, uint64_t arg);
    rq_watch("waitUntilCompleted (GPU)", 0);
    [c waitUntilCompleted];
    rq_watch(NULL, 0);
    uint64_t d = mach_absolute_time() - t0;
    stat_wait_ns += d;
    __atomic_fetch_add(&cum_wait_ticks, d, __ATOMIC_RELAXED);  // (the slow-frame log)
    stat_wait_by[g_wait_kind] += d;
    if (d > 24000) stat_wait_n[g_wait_kind]++;  // (> ~1 ms counts as a real wait)
    counters_resolve();
}

// Textures the game destroyed (any thread): their cached views and snapshot copies are dropped on the render thread.
static pthread_mutex_t gone_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { void *nvn, *mtl; } gone[4096];
static int ngone;
void renderer_texture_gone(void *nvn_tex, void *mtl_tex) {
    pthread_mutex_lock(&gone_lock);
    if (ngone < 4096) { gone[ngone].nvn = nvn_tex; gone[ngone].mtl = mtl_tex; ngone++; }
    pthread_mutex_unlock(&gone_lock);
}
static void drain_gone(void) {
    pthread_mutex_lock(&gone_lock);
    int n = ngone;
    static struct { void *nvn, *mtl; } local[4096];
    memcpy(local, gone, sizeof local[0] * n);
    ngone = 0;
    pthread_mutex_unlock(&gone_lock);
    for (int i = 0; i < n; i++) {
        if (local[i].mtl) renderer_views_forget(local[i].mtl);
        if (local[i].nvn) {
            renderer_td_forget(local[i].nvn);
            renderer_feedback_forget(local[i].nvn);
            for (int k = 0; k < 8; k++) if (renderer_snapshot_texture(k) == local[i].nvn) renderer_snapshot_take(k, NULL);
        }
    }
}

// A completed command buffer's share of the GPU's busy time (ns), counted once: the GPU starts a buffer while the one
// before it still runs (the game's frame is several - begin_pass commits every 8 passes - and the window image's is
// one more), so their GPUStartTime..GPUEndTime spans overlap; a buffer can also start and then sit waiting for the one
// before it to produce what it reads. Summed, they counted that overlap twice or more: M3 Max 5120x1440, frames that
// held 60 fps measured "game 16 + window 3 ms", and at the 1/3 floor (1707x480) still "game 12 + window 5" - Dynamic
// sat at its floor where a fixed 1x (3840x1080) holds 60. Their union is counted: each buffer from where the latest
// end so far left off (one queue: they start in order). A window buffer gets only its time after the game's work.
static pthread_mutex_t busy_lock = PTHREAD_MUTEX_INITIALIZER;
static double busy_end;  // (s, GPU clock) the latest command buffer end counted
uint64_t renderer_gpu_busy(id<MTLCommandBuffer> done, bool win) {
    double s = done.GPUStartTime, e = done.GPUEndTime;
    if (!(e > s) || e - s > 1.0) return 0;  // (not run, or nonsense)
    pthread_mutex_lock(&busy_lock);
    if (busy_end > e + 1.0) busy_end = 0;  // (clock base changed)
    double from = s > busy_end ? s : busy_end;
    if (e > busy_end) busy_end = e;
    pthread_mutex_unlock(&busy_lock);
    uint64_t ns = e > from ? (uint64_t)((e - from) * 1e9) : 0;
    __atomic_add_fetch(win ? &g_gpu_win_ns : &g_gpu_total_ns, ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_gpu_sum_ns, (uint64_t)((e - s) * 1e9), __ATOMIC_RELAXED);
    return ns;
}

// GPU time by kind of pass (NXRT_GPU_PASSES=1, profiling): every pass gets a command buffer of its own (the chunk size
// becomes 1), labelled with its render target (physical size, format of color 0, attachments). Each buffer's share of
// the GPU's busy time (renderer_gpu_busy) is added to its label's row; renderer_stats logs the top rows with the
// rest of the 2-second stats line. Work encoded after a pass and before the next (compute, copies) counts with it.
#define GP_MAX 256
static struct { char key[64]; uint64_t ns, passes, draws; } gp_rows[GP_MAX];
static int gp_n;
static pthread_mutex_t gp_lock = PTHREAD_MUTEX_INITIALIZER;
static char gp_cur[64];  // (render thread: the label of the pass the current command buffer holds)
static uint64_t gp_draws0;
static bool gpu_passes(void) {
    static int on = -1;
    if (on < 0) on = getenv("NXRT_GPU_PASSES") && atoi(getenv("NXRT_GPU_PASSES"));
    return on;
}
static void gp_add(const char *key, uint64_t ns, uint64_t draws) {
    pthread_mutex_lock(&gp_lock);
    int i = 0;
    while (i < gp_n && strcmp(gp_rows[i].key, key)) i++;
    if (i == gp_n && gp_n < GP_MAX) {
        memset(&gp_rows[gp_n], 0, sizeof gp_rows[0]);
        snprintf(gp_rows[gp_n++].key, sizeof gp_rows[0].key, "%s", key);
    }
    if (i < gp_n) gp_rows[i].ns += ns, gp_rows[i].passes++, gp_rows[i].draws += draws;
    pthread_mutex_unlock(&gp_lock);
}
static void gp_log(void) {
    pthread_mutex_lock(&gp_lock);
    uint64_t total = 0;
    for (int i = 0; i < gp_n; i++) total += gp_rows[i].ns;
    for (int k = 0; k < 14 && total; k++) {
        int best = -1;
        for (int i = 0; i < gp_n; i++) if (gp_rows[i].ns && (best < 0 || gp_rows[i].ns > gp_rows[best].ns)) best = i;
        if (best < 0) break;
        LOG("[gpu-passes] %5.1f%% %7.1f ms  %5llu passes %7llu draws  %s\n", 100.0 * gp_rows[best].ns / total,
            gp_rows[best].ns / 1e6, (unsigned long long)gp_rows[best].passes, (unsigned long long)gp_rows[best].draws,
            gp_rows[best].key);
        gp_rows[best].ns = 0;
    }
    if (total) LOG("[gpu-passes] total %.1f ms\n", total / 1e6);
    gp_n = 0;
    pthread_mutex_unlock(&gp_lock);
}

// nvnCommandBufferBindProgram(cmd, program, stages): the program is bound for those stages only. Most games bind one
// program for vertex + fragment; some bind a program per stage (Fire Emblem: Three Houses: a vertex-only program, then
// a fragment-only one). The renderer works from one program: the one bound for every stage when that's a single
// program (as before), else a combined program made from the stages' (cached per combination, its stages refreshed
// on each bind). A NULL program unbinds only the stages in the mask (Unity binds NULL for the ones it doesn't use).
#define COMBINED_MAX 256
static struct { NvnProgram *from[6]; NvnProgram prog; } g_combined[COMBINED_MAX];
static int g_ncombined;
static void bind_program(NvnProgram *p, uint32_t stages) {
    if (!stages) stages = 0x3f;  // (an old recording without the mask: every stage, as before)
    for (int i = 0; i < 6; i++)
        if (stages & (1u << i)) st.bound[i] = p;
    NvnProgram *one = NULL;
    bool mixed = false;
    for (int i = 0; i < 6; i++) {
        NvnProgram *b = st.bound[i];
        if (!b || !(b->stage_mask & (1u << i))) continue;  // (a stage this program doesn't have: doesn't decide)
        if (!one) one = b;
        else if (b != one) mixed = true;
    }
    if (!mixed) {
        st.program = one ? one : p;
        return;
    }
    int k = 0;
    while (k < g_ncombined && memcmp(g_combined[k].from, st.bound, sizeof st.bound)) k++;
    if (k == g_ncombined) {
        if (g_ncombined == COMBINED_MAX) k = g_ncombined - 1;  // (full: the last slot is reused)
        else g_ncombined++;
        memcpy(g_combined[k].from, st.bound, sizeof st.bound);
    }
    NvnProgram *c = &g_combined[k].prog;
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 6; i++) {
        NvnProgram *b = st.bound[i];
        if (b && (b->stage_mask & (1u << i))) c->stages[i] = b->stages[i], c->stage_mask |= 1u << i;
    }
    st.program = c;
}

static void skip_note(int line) {
    stat_skipped++;
    shader_strict_skip();
    extern bool g_debug_frame;
    if (g_debug_frame)
        LOG("[skip] debug frame: site %d left out a draw (vs %llx fs %llx)\n", line, st.program ? st.program->stages[0].code_addr : 0,
            st.program ? st.program->stages[1].code_addr : 0);
    static int on = -1;
    static uint32_t hits[8192];
    if (on < 0) on = getenv("NXRT_LOG_SKIPS") && atoi(getenv("NXRT_LOG_SKIPS"));
    if (!on || line <= 0 || line >= 8192) return;
    uint32_t n = ++hits[line];
    if ((n & (n - 1)) == 0 && (n == 1 || n >= 1024))  // (the first, then 1024, 2048, ...: which site keeps skipping)
        LOG("[skip] site %d: %u draws left out (program %p)\n", line, n, (void *)st.program);
}

// Commit recorded work without waiting.
void renderer_submit(void) {
    renderer_end_pass();
    drain_gone();
    if (cmdbuf) {
        id<MTLCommandBuffer> c = cmdbuf;
        cmdbuf = nil;
        const char *key = NULL;  // (a copy the completion handler owns)
        uint64_t draws = gp_draws_all - gp_draws0;
        if (gpu_passes()) key = strdup(gp_cur[0] ? gp_cur : "(no pass: compute / copies)");
        [c addCompletedHandler:^(id<MTLCommandBuffer> done) {
            uint64_t ns = renderer_gpu_busy(done, false);
            stat_gpu_s += ns / 1e9;
            if (key) gp_add(key, ns, draws), free((void *)key);
            if (done.error) LOG("[metal] command buffer error: %s\n", done.error.localizedDescription.UTF8String);
        }];
        [c commit];
        g_passes_in_cb = 0;
        gp_cur[0] = 0;
        gp_draws0 = gp_draws_all;
        pthread_mutex_lock(&cb_lock);
        last_cb = c;
        submit_seq++;
        cb_hist[submit_seq % CB_HIST] = c;
        pthread_mutex_unlock(&cb_lock);
        stat_flushes++;
        counters_resolve();  // (also without fence waits: a movie, loading - the table doesn't fill)
    }
}

// Finish all recorded GPU work (the CPU is about to read GPU results, or reuse the ring).
void renderer_shutdown(void) {
    renderer_archive_save(false);  // (a screenshot / scripted _exit() skips atexit)
}

void renderer_flush(void) {
    renderer_submit();
    pthread_mutex_lock(&cb_lock);
    id<MTLCommandBuffer> c = last_cb;
    pthread_mutex_unlock(&cb_lock);
    g_wait_kind = 1;
    wait_cb(c);
    g_wait_kind = 0;
    ring_reset();
}

// Wait for submitted work without committing anything new (nvnSyncWait / QueueFinish).
// Callable from any thread: everything submitted so far was committed by renderer_submit.
void renderer_sync(void) {
    pthread_mutex_lock(&cb_lock);
    id<MTLCommandBuffer> c = last_cb;
    pthread_mutex_unlock(&cb_lock);
    g_wait_kind = 2;
    wait_cb(c);
    g_wait_kind = 0;
}

// (the slow-frame log, slowframe.h; the game thread) running totals: no heap imports here (the heap is shared once)
void renderer_slow_counters(uint64_t *submits, uint64_t *gpu_wait_ns, uint64_t *imports) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    *submits = __atomic_load_n(&submit_seq, __ATOMIC_RELAXED);
    *gpu_wait_ns = (uint64_t)((unsigned __int128)__atomic_load_n(&cum_wait_ticks, __ATOMIC_RELAXED) * tb.numer / tb.denom);
    *imports = 0;
}

uint64_t renderer_submit_seq(void) {
    pthread_mutex_lock(&cb_lock);
    uint64_t q = submit_seq;
    pthread_mutex_unlock(&cb_lock);
    return q;
}
uint64_t renderer_issued_seq(void) { return renderer_submit_seq(); }  // (rqueue.h: committed here is submitted)

// Sequence number a fence placed now belongs to: the pending (uncommitted) work's submission, else the last one.
uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&cb_lock);
    uint64_t q = submit_seq + (cmdbuf ? 1 : 0);
    pthread_mutex_unlock(&cb_lock);
    return q;
}

// Wait until submission `seq` has completed (0 = nothing to wait for).
void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    if (ENV("NXRT_LOG_FENCES")) {  // (debug) how far behind the newest submission the waited fence is
        extern uint64_t renderer_present_count(void);
        static uint64_t f0, hist[6], frames_seen;
        uint64_t f = renderer_present_count();
        uint64_t pend = submit_seq + (cmdbuf ? 1 : 0);
        uint64_t age = pend > seq ? pend - seq : 0;
        bool done_already = false;
        pthread_mutex_lock(&cb_lock);
        if (seq <= submit_seq && submit_seq - seq < CB_HIST && cb_hist[seq % CB_HIST])
            done_already = cb_hist[seq % CB_HIST].status >= MTLCommandBufferStatusCompleted;
        pthread_mutex_unlock(&cb_lock);
        hist[age > 4 ? 4 : age]++;
        if (done_already) hist[5]++;
        if (f - f0 >= 300) {
            frames_seen = f - f0;
            LOG("[fences] per %llu frames: waits by age (submissions behind newest) 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu, already done %llu; submissions now %llu\n",
                (unsigned long long)frames_seen, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], (unsigned long long)submit_seq);
            memset(hist, 0, sizeof hist);
            f0 = f;
        }
    }
    pthread_mutex_lock(&cb_lock);
    id<MTLCommandBuffer> c = nil;
    if (seq > submit_seq) c = last_cb;  // not committed yet (other thread): wait for everything committed so far
    else if (submit_seq - seq < CB_HIST) c = cb_hist[seq % CB_HIST];
    pthread_mutex_unlock(&cb_lock);
    wait_cb(c);
}

static bool gpu_passes(void);
bool renderer_gpu_passes(void);

// Pass debug/capture callbacks (renderer_pass.m calls these; the debug state stays here).
void renderer_pass_debug_begin(MTLRenderPassDescriptor *rp) {
    if (!g_debug_frame) return;  // (a capture / debug frame: each pass, its targets and how each starts)
        static const char *const la[] = {"dontcare", "load", "clear"};
        LOG("[pass] frame %llu:", (unsigned long long)renderer_exec_frame());
        for (int i = 0; i < st.rt.ncolor; i++)
            if (st.rt.color[i]) LOG(" c%d@%llx %s mtl %p", i, (unsigned long long)st.rt.color[i]->addr, la[rp.colorAttachments[i].loadAction % 3],
                                    (__bridge void *)rp.colorAttachments[i].texture);
        if (st.rt.depth) LOG(" d@%llx %s", (unsigned long long)st.rt.depth->addr, la[rp.depthAttachment.loadAction % 3]);
        LOG("\n");
}

void renderer_pass_gpu_label(MTLRenderPassDescriptor *rp) {
    if (!renderer_gpu_passes()) return;
        id<MTLTexture> t0 = rp.colorAttachments[0].texture, td = rp.depthAttachment.texture;
        id<MTLTexture> any = t0 ? t0 : td;
        int nc = 0;
        for (int i = 0; i < 8; i++) nc += rp.colorAttachments[i].texture != nil;
        snprintf(gp_cur, sizeof gp_cur, "%lux%lu fmt %lu x%d%s%s", any ? (unsigned long)any.width : 0,
                 any ? (unsigned long)any.height : 0, t0 ? (unsigned long)t0.pixelFormat : 0, nc, td ? " +depth" : "",
                 rp.colorAttachments[0].loadAction == MTLLoadActionClear ? " clear" : "");
}

void renderer_pass_debug_end(NvnTexture *c0) {
        // each pass's colour target as it ends: a debug frame, or a capture's first frame (in a capture, 30 frames of
        // this stalled the game: one frame is a few seconds) - into the capture folder, named by pass and target
        extern uint64_t g_debug_exec_frame;
        bool snap = g_debug_frame && (!g_capture_dir[0] || renderer_exec_frame() == g_debug_exec_frame);
        if (snap && c0 && c0->backend) {
            id<MTLTexture> t = renderer_texture(c0, nil, true);
            id<MTLCommandBuffer> c = cmdbuf;
            cmdbuf = nil;
            [c commit];
            [c waitUntilCompleted];
            char path[700], apath[700];
            if (g_capture_dir[0]) {
                if (draw_watched) {  // (a watched draw's target, right after it)
                    snprintf(path, sizeof path, "%s/draw_%04d_p%03d_%llx.png", g_capture_dir, debug_draw_no, debug_pass_no, (unsigned long long)c0->addr);
                    snprintf(apath, sizeof apath, "%s/draw_%04d_p%03d_%llx_a.png", g_capture_dir, debug_draw_no, debug_pass_no, (unsigned long long)c0->addr);
                } else {
                    snprintf(path, sizeof path, "%s/pass_%03d_%llx.png", g_capture_dir, debug_pass_no, (unsigned long long)c0->addr);
                    snprintf(apath, sizeof apath, "%s/pass_%03d_%llx_a.png", g_capture_dir, debug_pass_no++, (unsigned long long)c0->addr);
                }
                LOG("[pass] snapshot %s\n", path);
            } else {
                snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no);
                snprintf(apath, sizeof apath, "build/pass_%03d_a.png", debug_pass_no++);
            }
            renderer_save_game_texture(c0, t, path, apath);  // (the part the pass drew: Dynamic resolution)
        }
}
void renderer_stat_pass_add(void) { stat_passes++; }
void renderer_stat_dispatch_add(void) { stat_dispatches++; }
void renderer_stat_clears_load_add(void) { stat_clears_as_load++; }
id<MTLCommandBuffer> renderer_command_buffer(void) { return cb(); }
bool renderer_gpu_passes(void) { return gpu_passes(); }

// ------------------------------------------------------------------ render passes
static __unsafe_unretained id last_bias_enc;  // encoder the depth bias was last set on



// Physical size of the current pass (guest size times the attachments' allocation scale).
// ------------------------------------------------------------------ shaders (debug dump)


// ------------------------------------------------------------------ draw support
uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
static int cbuf_base = 3, tex_base = 8;

static id<MTLBuffer> ring;
static uint64_t ring_off;
#define RING_SIZE (64u << 20)

static uint64_t ring_alloc(const void *data, uint64_t size) {
    if (!ring) ring = [dev newBufferWithLength:RING_SIZE options:MTLResourceStorageModeShared];
    uint64_t aligned = (ring_off + 255) & ~255ull;
    if (aligned + size > RING_SIZE) {
        // Start a fresh ring; command buffers in flight keep the old one alive until they complete.
        ring = [dev newBufferWithLength:(size > RING_SIZE ? size : RING_SIZE) options:MTLResourceStorageModeShared];
        aligned = 0;
    }
    memcpy((uint8_t *)ring.contents + aligned, data, size);
    ring_off = aligned + size;
    return aligned;
}
id<MTLBuffer> renderer_ring_buffer(void) { return ring; }
uint64_t renderer_ring_alloc(const void *data, uint64_t size) { return ring_alloc(data, size); }


static void ring_reset(void) { ring_off = 0; }

// Bind guest memory [addr, addr+size) as a buffer: zero-copy inside the heap, copied otherwise.
static void bind_guest(id<MTLRenderCommandEncoder> e, bool vertex_stage, uint64_t addr, uint64_t size, int index) {
    if (addr >= HEAP_BASE && addr + size <= HEAP_BASE + HEAP_MAX && (addr & 3) == 0) {
        if (vertex_stage) [e setVertexBuffer:g_guest_buffer offset:addr - HEAP_BASE atIndex:index];
        else [e setFragmentBuffer:g_guest_buffer offset:addr - HEAP_BASE atIndex:index];
        return;
    }
    uint64_t off = ring_alloc((void *)(uintptr_t)addr, size);
    if (vertex_stage) [e setVertexBuffer:ring offset:off atIndex:index];
    else [e setFragmentBuffer:ring offset:off atIndex:index];
}


















// Translates a program's two stages into pr (false: failed, logged). vaddr / faddr: the code's guest addresses
// (debug names; 0 when preparing from the game files)



// ------------------------------------------------------------------ prepared objects (shader_prep.h)
// Built before the game from the shipped variant list; found by content (code hashes + keys) at first use, as code
// addresses differ from run to run. Kept for the whole process (reboots included). g_prep_lock guards the tables.
Prog *renderer_prep_find_prog(uint64_t k) { return renderer_prep_prog(k); }
static dispatch_queue_t g_prep_queue;














// Shadow slots (depth2d, sample_compare) need a depth texture: with nothing usable bound they get a 1x1 Depth32Float
// cleared to 1.0 (lit under the usual less / less-equal compare, as an empty shadow map) instead of the RGBA8 dummy,
// which compared as 0 (in shadow) and is not a depth texture. The Vulkan side does the same (exec.c shadow_slot_check).




// The texture for a shadow slot: tex if it is a comparable depth texture, else the depth dummy (logged once per cause).


// NVN texture/view component swizzle (same enum values as MTLTextureSwizzle), applied as a view.


// Upload helper: a separate command buffer, committed now so it runs before the current pass.




// ---- render-target feedback: a draw that samples one of the pass's own attachments (e.g. depth
// bound for testing while the shader reads it) gets a snapshot copy, as sampling an attachment of the
// current Metal pass is undefined.
// Snapshots stay valid until their texture is written again (a draw writing that attachment, a clear, copy or
// compute). Soft particles (grass-cut leaves, smoke) sample the depth buffer they're tested against but don't
// write it, so a burst of them shares one snapshot instead of a pass break + full-screen copy per draw.




void renderer_td_flush_shader_textures(int stage, ShaderInfo *si) {
    if (!renderer_td_pending() || !si) return;
    for (int i = 0; i < si->ntex; i++) renderer_td_flush(renderer_shader_texture_object(stage, &si->tex[i]));
}

// The texture handle slots (ShaderKey buffer_tex) of a stage's textures that are bound to buffer textures now






// Shaders with more textures than Metal's sampler slots (MSL_MAX_SAMPLERS; issue #215: a fragment shader of the Ukuku
// Prairie cave with 17) take their samplers as an array of the slots, and a table of each texture's slot
// (maxwell/translate.c). Each distinct sampler state the draw uses goes in a slot of its own - textures sharing a state
// share its slot - so every texture keeps its own filtering, wrapping and compare mode. smp[i] / desc[i]: texture i's
// state and its descriptor (NULL: the default sampler); fills slot[] and map[], returns the slots used.


// (NXRT_DUMP_TRIGGER) each texture sampled in the captured frames, saved once: <dir>/<guest addr>_<w>x<h>_f<format>.png
// (the draw log's [dtex] lines name textures by the same address)
char g_capture_dir[512];









static MTLPrimitiveType prim_type(uint32_t mode, bool *ok) {
    *ok = true;
    switch (mode) {
    case 0: return MTLPrimitiveTypePoint;
    case 1: return MTLPrimitiveTypeLine;
    case 3: return MTLPrimitiveTypeLineStrip;
    case 4: return MTLPrimitiveTypeTriangle;
    case 5: return MTLPrimitiveTypeTriangleStrip;
    default: *ok = false; return MTLPrimitiveTypeTriangle;
    }
}

// Expand quads/fans/polygons into a triangle index list (32-bit) referencing the original vertices.
static id<MTLBuffer> expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first,
                                    uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {  // quads
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {  // fan / polygon
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    id<MTLBuffer> b = [dev newBufferWithBytes:out length:n * 4 + 4 options:MTLResourceStorageModeShared];
    free(out);
    return b;
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    static uint64_t dump_fs = 1;
    if (dump_fs == 1) { const char *e = getenv("NXRT_DUMP_UBO"); dump_fs = e ? strtoull(e, NULL, 16) : 0; }
    if (dump_fs && st.program && st.program->stages[1].code_addr == dump_fs) {
        for (int i = 0; i < 18; i++) {
            if (!st.ubuf[1][i].addr) continue;
            const float *f = (const float *)(uintptr_t)st.ubuf[1][i].addr;
            LOG("[ubo] f%d (%llu):", i, st.ubuf[1][i].size);
            for (uint64_t k = 0; k < st.ubuf[1][i].size / 4 && k < 1200; k++) LOG(" %g", f[k]);
            LOG("\n");
        }
    }
    static int dump_vs = -1;  // NXRT_DUMP_VSUBO: every vertex-stage UBO (first 48 floats) of debug-frame draws
    if (dump_vs < 0) dump_vs = getenv("NXRT_DUMP_VSUBO") != NULL;
    for (int i = 0; dump_vs && i < 18; i++) {
        if (!st.ubuf[0][i].addr) continue;
        const float *f = (const float *)(uintptr_t)st.ubuf[0][i].addr;
        LOG("[vsubo] v%d (%llu):", i, st.ubuf[0][i].size);
        for (uint64_t k = 0; k < st.ubuf[0][i].size / 4 && k < 48; k++) LOG(" %g", f[k]);
        LOG("\n");
    }
    if (st.sbuf[0][0].addr && st.sbuf[0][0].size >= 64 && st.sbuf[0][0].size <= 8192) {
        int recw = (st.sbuf[0][0].size % 96 == 0 && st.sbuf[0][0].size % 64 != 0) ? 24 : 16;
        const float *f = (const float *)(uintptr_t)st.sbuf[0][0].addr;
        const uint32_t *u = (const uint32_t *)f;
        for (int g = 0; g < (int)(st.sbuf[0][0].size / (recw * 4)) && g < 6; g++) {
            LOG("[ssbo] rec%d:", g);
            for (int k = 0; k < recw; k++) LOG(" %g(%08x)", f[g * recw + k], u[g * recw + k]);
            LOG("\n");
        }
    }
    LOG("[draw] %s inst %llu", indexed ? "e" : "a", indexed ? r->x[6] : r->x[4]);
    LOG(" %s mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | ubufs:",
        indexed ? "elems" : "arrays", r->x[0], r->x[2], st.program->stages[0].code_addr, st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask);
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 18; i++)
            if (st.ubuf[s][i].addr) LOG(" %c%d(%llx,%llu)", s ? 'f' : 'v', i, st.ubuf[s][i].addr, st.ubuf[s][i].size);
    LOG(" | fubo0:");
    for (int i = 0; i < 6; i++)
        if (st.ubuf[1][i].addr) { const float *f = (const float *)(uintptr_t)st.ubuf[1][i].addr; LOG(" f%d=[%g %g %g %g]", i, f[0], f[1], f[2], f[3]); }
    LOG(" | ssbo:");
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < 16; i++)
            if (st.sbuf[sg][i].addr) LOG(" %c%d(%llx,%llu)", sg ? 'f' : 'v', i, st.sbuf[sg][i].addr, st.sbuf[sg][i].size);
    LOG(" | tex:");
    for (int i = 0; i < pr->fi->ntex; i++) LOG(" off%u", pr->fi->tex[i].handle_offset);
    LOG(" bound:");
    for (int i = 0; i < 32; i++) {
        if (!st.tex[1][i]) continue;
        uint32_t tic = st.tex[1][i] & 0xFFFFF;
        NvnTexture *t = st.tpool && tic < (uint32_t)st.tpool->count ? ((NvnTexEntry *)st.tpool->entries)[tic].tex : NULL;
        if (t) LOG(" %d=[%ux%ux%u t%u f%#x fl%#x %s]", i, t->width, t->height, t->depth, t->target, t->format, t->flags, t->label);
        else LOG(" %d=%llx", i, st.tex[1][i]);
    }
    LOG(" | rt %d", st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i]) LOG(" %ux%u/f%#x@%llx", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format,
                                (unsigned long long)st.rt.color[i]->addr);
    if (st.rt.depth) LOG(" d%#x@%llx", st.rt.depth->format, (unsigned long long)st.rt.depth->addr);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x streams:", st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask);
    for (int i = 0; i < st.nattrib; i++) LOG(" a%d(f%#x s%u o%u)", i, st.attrib[i] & 0xFF, (st.attrib[i] >> 8) & 0x1F, st.attrib[i] >> 13);
    for (int i = 0; i < 12; i++) if (st.vbuf[i].addr) LOG(" vb%d(%llx,%llu,stride %u div %u)", i, st.vbuf[i].addr, st.vbuf[i].size, st.stream[i][0], st.stream[i][1]);
    if (st.ubuf[0][0].addr && st.ubuf[0][0].size == 560) {
        const float *f = (const float *)(uintptr_t)st.ubuf[0][0].addr;
        LOG("\n  vs ubo0:");
        for (int i = 0; i < 36; i++) LOG("%s[%d] %g %g %g %g", i % 2 ? " " : "\n   ", i, f[i*4], f[i*4+1], f[i*4+2], f[i*4+3]);
    }
    LOG(" blend %#x ds %02x%02x%02x poly %02x%02x scissor %d,%d %dx%d chmask %08x stencil %02x%02x%02x%02x%02x ref %u/%u wm %x vm %x bf %d,%d,%d,%d,%d,%d\n", st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], st.channel_mask, st.ds[3], st.ds[4], st.ds[5], st.ds[6],
        st.ds[7], st.stencil_ref[0], st.stencil_ref[1], st.stencil_mask[0], st.stencil_vmask[0], st.blend[0][1], st.blend[0][2],
        st.blend[0][3], st.blend[0][4], st.blend[0][5], st.blend[0][6]);
}


// ---- pipeline cache keyed by a hash of the raw state that shapes the pipeline (avoids building a
// descriptor and a string key per draw)
// Pipelines are keyed by their shader functions' addresses: a reboot frees the old functions, so keys carry the
// boot generation (a build still in flight can't land on a new function at a reused address).
static unsigned g_boot_gen;
static NSMutableDictionary *g_ready_pipes;

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}
#define PIPE_TAB 8192
static struct { uint64_t key; void *pipe; } pipe_tab[PIPE_TAB];

static uint64_t pipeline_state_hash(Prog *pr) {
    uint64_t h = 0xcbf29ce484222325ull;
    h = hmix(h, &pr, sizeof pr);
    h = hmix(h, &st.rt.ncolor, sizeof st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor && i < 8; i++) {
        NvnTexture *t = st.rt.color[i];
        uint32_t v[4] = {t ? t->format : 0, t ? t->samples : 0, st.rt.has_cview[i], st.rt.has_cview[i] ? st.rt.cview[i].format | (st.rt.cview[i].setmask << 16) : 0};
        h = hmix(h, v, sizeof v);
        h = hmix(h, st.blend[i], 8);
    }
    uint32_t dfmt = st.rt.depth ? st.rt.depth->format : 0;
    h = hmix(h, &dfmt, 4);
    h = hmix(h, &st.nattrib, sizeof st.nattrib);
    h = hmix(h, st.attrib, sizeof st.attrib[0] * (size_t)(st.nattrib < 32 ? st.nattrib : 32));
    h = hmix(h, &st.nstream, sizeof st.nstream);
    h = hmix(h, st.stream, sizeof st.stream[0] * (size_t)(st.nstream < 16 ? st.nstream : 16));
    h = hmix(h, &st.channel_mask, sizeof st.channel_mask);
    h = hmix(h, &st.color_state[0], sizeof st.color_state[0]);
    h = hmix(h, &st.ms[0xC], 1);
    return h ? h : 1;
}

// The draw's state as a variant record (shader_prep.h): what its program and pipeline depend on, in NVN terms
static void rec_from_state(ShaderRec *r, bool flip_x, uint32_t mode) {
    memset(r, 0, sizeof *r);
    r->kind = 'G';
    r->vs = shader_stage_hash(&st.program->stages[0]);
    r->fs = shader_stage_hash(&st.program->stages[1]);
    r->flags = (nvn_window_origin() == 0 ? SR_FLIP_Y : 0) | (nvn_depth_mode() == 0 ? SR_DEPTH_MINUS_ONE : 0) | (flip_x ? SR_FLIP_X : 0);
    r->nattrib = (uint8_t)(st.nattrib < 16 ? st.nattrib : 16);
    memcpy(r->attrib, st.attrib, sizeof r->attrib[0] * r->nattrib);
    r->nstream = (uint8_t)(st.nstream < 16 ? st.nstream : 16);
    memcpy(r->stream, st.stream, sizeof r->stream[0] * r->nstream);
    r->ncolor = (uint8_t)(st.rt.ncolor < 8 ? st.rt.ncolor : 8);
    r->samples = 1;
    for (int i = 0; i < r->ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        if (!t) continue;
        r->color[i] = t->format;
        if (st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4)) r->cview[i] = st.rt.cview[i].format;
        if (t->samples > 1) r->samples = t->samples;
        r->channel_mask |= st.channel_mask & (0xFu << (i * 4));
        if (((st.color_state[0] >> i) & 1) && !renderer_rt_kind(t->format)) {
            r->blend_on |= (uint8_t)(1u << i);
            memcpy(r->blend[i], st.blend[i], 8);
        }
    }
    if (st.rt.depth) {
        r->depth = st.rt.depth->format;
        if (!r->ncolor && st.rt.depth->samples > 1) r->samples = st.rt.depth->samples;
    }
    r->a2c = st.ms[0xC] != 0;
    r->topo = shader_topo_class(mode);
    shader_rec_normalize(r);
    r->poly_fill = st.poly[2] != 1;
}

// The pipeline a draw needs: everything but the attachment formats (attachment_formats / attachment_formats_rec),
// from the variant record of the draw's state (rec_from_state) or of the shipped list (renderer_prepare)
static void pipeline_desc(MTLRenderPipelineDescriptor *pd, Prog *pr, const ShaderRec *r) {
    pd.vertexFunction = pr->vs;
    pd.fragmentFunction = pr->fs;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    uint32_t used_streams = 0;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        uint32_t a = i < r->nattrib ? r->attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= r->nattrib || fmt == 0) {
            // unbound attribute: read zeros from a constant stream
            vd.attributes[i].format = MTLVertexFormatFloat4;
            vd.attributes[i].offset = 0;
            vd.attributes[i].bufferIndex = 30;
            vd.layouts[30].stepFunction = MTLVertexStepFunctionConstant;
            vd.layouts[30].stepRate = 0;
            vd.layouts[30].stride = 16;
            used_streams |= 1u << 31;
            continue;
        }
        vd.attributes[i].format = renderer_vertex_format(fmt);
        vd.attributes[i].offset = off;
        vd.attributes[i].bufferIndex = 18 + stream;
        used_streams |= 1u << stream;
    }
    for (int s = 0; s < 12; s++) {
        if (!(used_streams & (1u << s))) continue;
        uint32_t stride = s < r->nstream ? r->stream[s][0] : 0, div = s < r->nstream ? r->stream[s][1] : 0;
        if (stride == 0) { vd.layouts[18 + s].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[18 + s].stepRate = 0; vd.layouts[18 + s].stride = 16; }
        else {
            vd.layouts[18 + s].stride = (stride + 3) & ~3u;
            if (div) { vd.layouts[18 + s].stepFunction = MTLVertexStepFunctionPerInstance; vd.layouts[18 + s].stepRate = div; }
        }
    }
    pd.vertexDescriptor = vd;
    for (int i = 0; i < r->ncolor; i++) {
        if (!r->color[i]) continue;
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
        uint32_t m = (r->channel_mask >> (i * 4)) & 0xF;
        ca.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) |
                       (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
        if ((r->blend_on >> i) & 1) {  // (the record leaves out blending on integer targets)
            const uint8_t *b = r->blend[i];
            ca.blendingEnabled = YES;
            ca.sourceRGBBlendFactor = renderer_blend_factor(b[1]);
            ca.destinationRGBBlendFactor = renderer_blend_factor(b[2]);
            ca.sourceAlphaBlendFactor = renderer_blend_factor(b[3]);
            ca.destinationAlphaBlendFactor = renderer_blend_factor(b[4]);
            ca.rgbBlendOperation = renderer_blend_op(b[5]);
            ca.alphaBlendOperation = renderer_blend_op(b[6]);
        }
    }
    pd.alphaToCoverageEnabled = r->a2c != 0;
}

// What makes a pipeline (the descriptor's contents): the key of the pipeline caches. (Without the boot generation:
// draws prefix it, prepared pipelines' functions live as long as the process.)
static NSString *pipeline_key(MTLRenderPipelineDescriptor *pd, Prog *pr) {
    NSMutableString *pk = [NSMutableString stringWithFormat:@"%p %p %lu %lu", pr->vs, pr->fs,
                           (unsigned long)pd.depthAttachmentPixelFormat, (unsigned long)pd.stencilAttachmentPixelFormat];
    for (int i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
        [pk appendFormat:@" %lu/%lu/%d/%lu/%lu/%lu/%lu/%lu/%lu", (unsigned long)ca.pixelFormat, (unsigned long)ca.writeMask,
                         ca.blendingEnabled, (unsigned long)ca.sourceRGBBlendFactor, (unsigned long)ca.destinationRGBBlendFactor,
                         (unsigned long)ca.sourceAlphaBlendFactor, (unsigned long)ca.destinationAlphaBlendFactor,
                         (unsigned long)ca.rgbBlendOperation, (unsigned long)ca.alphaBlendOperation];
    }
    MTLVertexDescriptor *vd = pd.vertexDescriptor;
    for (int i = 0; i < 32; i++)
        if (pr->vi->attr_mask & (1u << i))
            [pk appendFormat:@" a%d:%lu/%lu/%lu", i, (unsigned long)vd.attributes[i].format, (unsigned long)vd.attributes[i].offset, (unsigned long)vd.attributes[i].bufferIndex];
    for (int s = 18; s < 31; s++)
        [pk appendFormat:@" l%lu/%lu/%lu", (unsigned long)vd.layouts[s].stride, (unsigned long)vd.layouts[s].stepFunction, (unsigned long)vd.layouts[s].stepRate];
    [pk appendFormat:@" s%lu a%d", (unsigned long)pd.rasterSampleCount, pd.alphaToCoverageEnabled];
    return pk;
}

static id prep_find_pipe(NSString *body) {
    return renderer_prep_pipe(body);
}

static NxDraw g_draw;                   // the current draw, for the game's hooks (runtime/nxgame.h)

static void draw(CmdRec *r, bool indexed) {
    draw_watched = false;
    if (!st.program) { skip_note(2589); return; }
    static bool logged_modes;
    if (!logged_modes) {
        logged_modes = true;
        const char *cb = getenv("NXRT_CBUF_BASE"), *tb = getenv("NXRT_TEX_BASE");
        if (cb) cbuf_base = atoi(cb);
        if (tb) tex_base = atoi(tb);
    }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)renderer_attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)renderer_rt_kind(st.rt.color[i]->format);
    size_t nf = 0; const ShaderFact *facts = renderer_facts(&nf);
    if (nf) {  // the same facts the preparation used, so the draw hits the prepared program
        NvnProgram *p = st.program;
        if (!p->vs_hash) p->vs_hash = shader_stage_hash(&p->stages[0]);
        if (!p->fs_hash) p->fs_hash = shader_stage_hash(&p->stages[1]);
        const ShaderFact *vf = shader_fact_find(facts, nf, p->vs_hash);
        const ShaderFact *ff = shader_fact_find(facts, nf, p->fs_hash);
        if (vf) shader_fact_apply(vf, vkey.attr_kind, fkey.rt_kind);
        if (ff) shader_fact_apply(ff, vkey.attr_kind, fkey.rt_kind);
    }
    Prog *pr = renderer_get_program(st.program, &vkey, &fkey);
    if (pr && pr->vi && pr->fi) {  // textures bound to buffer textures: the program's variant that reads them so
        uint32_t vb = renderer_buffer_tex_slots(0, pr->vi), fb = renderer_buffer_tex_slots(1, pr->fi);
        if (vb || fb) {
            vkey.buffer_tex = vb, fkey.buffer_tex = fb;
            pr = renderer_get_program(st.program, &vkey, &fkey);
        }
    }
    if (!pr || !pr->ready) {
        static uint64_t said[64];  // (each program once: its stages and why)
        static int nsaid;
        uint64_t k = st.program->stages[0].code_addr ^ (st.program->stages[1].code_addr << 1) ^ (pr ? 1 : 0);
        bool seen = false;
        for (int i = 0; i < nsaid; i++) seen |= said[i] == k;
        if (getenv("NXRT_LOG_SKIPS") && !seen && nsaid < 64) {
            said[nsaid++] = k;
            LOG("[skip] program vs %llx fs %llx: %s\n", st.program->stages[0].code_addr, st.program->stages[1].code_addr,
                !pr ? "none (translation failed or no program)" : "not compiled yet");
        }
        skip_note(2603);
        return;
    }
    // the draw as the game's hooks see it (runtime/nxgame.h NxDraw)
    for (int i = 0; i < 6; i++) {
        g_draw.ubo[i] = (const float *)(uintptr_t)st.ubuf[0][i].addr;
        g_draw.ubo_size[i] = (uint32_t)st.ubuf[0][i].size;
    }
    g_draw.mesh = st.vbuf[0].addr;
    g_draw.instances = indexed ? (uint32_t)r->x[6] : (uint32_t)r->x[4];
    g_draw.tintable = pr->fi->uses_tint;
    g_draw.ntex = 0;
    {   // mirror mode (runtime/hle/mirror.c): the scene's composite into the window image is drawn mirrored - the
        // unblended full-screen draw sampling the scene image (the window images also serve as scratch
        // targets earlier in the frame: those draws sample much smaller images; the HUD after it blends)

        bool composite = false;
        if (st.rt.ncolor && st.rt.color[0] && renderer_texture_presented(st.rt.color[0]) && !(st.color_state[0] & 1) &&
            r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
            st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height && pr->fi->ntex) {
            NvnTexture *t0 = renderer_shader_texture_object(1, &pr->fi->tex[0]);
            composite = t0 && t0->width == st.rt.color[0]->width && t0->height == st.rt.color[0]->height;

        }
        if (NX_HOOK(draw_mirror) && NX_HOOK(draw_mirror)(composite)) {  // (the vertex flip mirrors a composite drawn with uvs, the fragment one a
            vkey.flip_x = fkey.flip_x = true;  // composite reading pixel positions; get_program keys on vkey)
            pr = renderer_get_program(st.program, &vkey, &fkey);
            if (!pr || !pr->ready) { skip_note(2628); return; }
        }
    }
    {   // Interiors/dungeons draw a 16:9 backdrop quad over the still-rendered overworld: frames that
        // draw it are presented cropped to 16:9 (see backend_present).
        static uint64_t last_fs, last_hash;
        uint64_t fs = st.program->stages[1].code_addr;
        if (fs != last_fs) {
            last_fs = fs;
            last_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
        }
        static uint64_t log_hash = 1;
        if (log_hash == 1) log_hash = getenv("NXRT_LOG_FSHASH") ? 0 : 2;
        if (!log_hash && g_debug_frame) LOG("[fshash] fs=%llx hash %016llx\n", fs, last_hash);
        {   // NXRT_SKIP_FSHASH=h1,h2,...: drop draws using these fragment shaders (find which draw makes what)
            static uint64_t skip[16];
            static int nskip = -1;
            if (nskip < 0) {
                nskip = 0;
                for (const char *p = getenv("NXRT_SKIP_FSHASH"); p && *p && nskip < 16;) {
                    skip[nskip++] = strtoull(p, (char **)&p, 16);
                    while (*p == ',') p++;
                }
            }
            for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { skip_note(2652); return; }

        }
        renderer_set_cur_fs_hash(last_hash);
        g_draw.fs_hash = last_hash;
        if (NX_HOOK(draw_shader) && !NX_HOOK(draw_shader)(&g_draw)) { skip_note(2657); return; }


        if (last_hash == 0x71fe31bbf03c3515ull && getenv("NXRT_LOG_INDOOR")) {
            static uint64_t lf;
            extern uint64_t renderer_present_count(void);
            if (renderer_present_count() != lf) {
                lf = renderer_present_count();
                LOG("[backdrop] frame %llu count %llu rt %ux%u vp %.0f %.0f %.0f %.0f scissor %d %d %d %d\n", (unsigned long long)lf, r->x[2],
                    st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0,
                    st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3], st.scissor[0][0], st.scissor[0][1],
                    st.scissor[0][2], st.scissor[0][3]);
            }
        }
    }
    static int log_rt_w = -1, log_rt_h;
    if (log_rt_w < 0) { const char *e = getenv("NXRT_LOG_RT"); log_rt_w = 0; if (e) sscanf(e, "%dx%d", &log_rt_w, &log_rt_h); }
    static uint64_t log_rt_addr = 1;
    if (log_rt_addr == 1) { const char *e = getenv("NXRT_LOG_RT_ADDR"); log_rt_addr = e ? strtoull(e, NULL, 16) : 0; }
    bool addr_hit = false;
    for (int i = 0; log_rt_addr && i < st.rt.ncolor; i++)
        if (st.rt.color[i] && st.rt.color[i]->addr <= log_rt_addr + 0x40000 && st.rt.color[i]->addr + 0x40000 >= log_rt_addr) addr_hit = true;
    if (addr_hit) LOG("[rtaddr] rt0 addr %llx\n", st.rt.color[0] ? st.rt.color[0]->addr : 0);
    if (g_debug_frame || addr_hit || (log_rt_w && st.rt.ncolor && st.rt.color[0] && (int)st.rt.color[0]->width == log_rt_w &&
                          (int)st.rt.color[0]->height == log_rt_h))
        debug_draw(pr, r, indexed);
    if (renderer_td_pending()) { renderer_td_flush_shader_textures(0, pr->vi); renderer_td_flush_shader_textures(1, pr->fi); }
    renderer_feedback_resolve(pr);
    // this draw's writes make snapshots of its attachments stale

    if (st.rt.depth && st.ds[0] && st.ds[1]) renderer_snapshots_invalidate_tex(st.rt.depth, "draw-depthwrite");  // stencil-only writes: shaders sample depth

    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) renderer_snapshots_invalidate_tex(st.rt.color[i], "draw-colorwrite");
    renderer_begin_pass();

    // vertex streams this draw reads (also computed inside the pipeline build on a cache miss)
    uint32_t used_streams = 0;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        if (i >= st.nattrib || (a & 0xFF) == 0) used_streams |= 1u << 31;
        else used_streams |= 1u << ((a >> 8) & 0x1F);
    }
    // pipeline
    uint64_t phash = pipeline_state_hash(pr);
    uint32_t pslot = (uint32_t)(phash ^ (phash >> 29)) & (PIPE_TAB - 1);
    while (pipe_tab[pslot].key && pipe_tab[pslot].key != phash) pslot = (pslot + 1) & (PIPE_TAB - 1);
    id<MTLRenderPipelineState> pipe = pipe_tab[pslot].key ? (__bridge id<MTLRenderPipelineState>)pipe_tab[pslot].pipe : nil;
    if (!pipe) {
    ShaderRec rec;
    rec_from_state(&rec, vkey.flip_x, (uint32_t)r->x[0]);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    renderer_attachment_formats(pd);
    pipeline_desc(pd, pr, &rec);
    NSString *body = pipeline_key(pd, pr);
    NSString *pk = [NSString stringWithFormat:@"%u %@", g_boot_gen, body];
    id pobj = renderer_cache_get(pk);
    NSMutableDictionary *ready_pipes = g_ready_pipes;  // async pipeline builds finished on Metal's threads
    static id pending;
    if (!ready_pipes) { ready_pipes = g_ready_pipes = [NSMutableDictionary new]; pending = [NSObject new]; }
    if (pobj == pending) {
        @synchronized(ready_pipes) {
            id done = ready_pipes[pk];
            if (done) { [ready_pipes removeObjectForKey:pk]; renderer_cache_set(pk, done); pobj = done; }
        }
        if (pobj == pending) { skip_note(2723); return; }
    }
    if (!pobj) {
        if (shader_rec_recording()) shader_rec_note(&rec);
        pobj = prep_find_pipe(body);  // prepared before the game
        if (pobj) renderer_cache_set(pk, pobj);
        else shader_first_visit("pipeline", rec.vs, rec.fs, shader_code_hash((const uint8_t *)body.UTF8String, body.length));
    }
    if (!pobj) {
        if (renderer_sync_shaders()) {
            NSError *err;
            pobj = TIMED_COMPILE(renderer_archive_render_pipeline(pd, pr, &err));
            if (!pobj) {
                static int n;
                if (n++ < 10) LOG("[metal] pipeline failed: %s\n", err.localizedDescription.UTF8String);
                pobj = [NSNull null];
            }
            renderer_cache_set(pk, pobj);
        } else {
            renderer_cache_set(pk, pending);
            NSString *k = [pk copy];
            double t0 = now_ms();
            static dispatch_queue_t pq;  // (off the render thread; through the binary archive)
            if (!pq) pq = dispatch_queue_create("nxrt.pipelines", DISPATCH_QUEUE_CONCURRENT);
            dispatch_async(pq, ^{
                NSError *e = nil;
                id<MTLRenderPipelineState> ps = renderer_archive_render_pipeline(pd, pr, &e);
                if (!ps) { static int n; if (n++ < 10) LOG("[metal] pipeline failed: %s\n", e.localizedDescription.UTF8String); }
                @synchronized(ready_pipes) { ready_pipes[k] = ps ? (id)ps : (id)[NSNull null]; }
                @synchronized(cache) { stat_compile_ms += now_ms() - t0; }
                slow_note_build("pipeline", (uint64_t)((now_ms() - t0) * 1e6));
                __atomic_fetch_add(&stat_compiles, 1, __ATOMIC_RELAXED);
            });
            skip_note(2756);
            return;
        }
    }
    if (pobj == [NSNull null]) { skip_note(2760); return; }
    pipe = pobj;
    pipe_tab[pslot].key = phash;
    pipe_tab[pslot].pipe = (__bridge void *)pipe;  // retained by the cache dictionary
    }
    [enc setRenderPipelineState:pipe];

    // Decal colour passes (issue #52): the game draws floor decals twice with the same negative polygon offset - a
    // depth pre-pass, then the colour pass with depth test EQUAL and no depth writes. An M4 (macOS 26.4) lands the two
    // passes' biased depths a hair apart, so EQUAL failed: black decals. Those colour passes run as LESS_EQUAL with
    // NXRT_BIAS_HW_SCALE times the game's offset (default 2) plus NXRT_BIAS_HW_EPS units (default 4): on or in
    // front of their pre-pass. The margin is what the decal's colour may show through: anything in front of the decal
    // by less than it. Key Cavern's entrance threshold (a slab just above the floor's border decal) showed it from
    // 64 units / 4x the offset on; 256 units (the first try) drew the border over the slab. SCALE=1 EPS=0: as before.
    static int nobias = -1;  // NXRT_NO_DEPTH_BIAS: ignore the game's polygon offset (A/B tests)
    if (nobias < 0) nobias = getenv("NXRT_NO_DEPTH_BIAS") != NULL;
    static float hw_scale = -1, hw_eps;
    if (hw_scale < 0) {
        hw_scale = getenv("NXRT_BIAS_HW_SCALE") ? fmaxf(1, (float)atof(getenv("NXRT_BIAS_HW_SCALE"))) : 2;
        hw_eps = getenv("NXRT_BIAS_HW_EPS") ? fmaxf(0, (float)atof(getenv("NXRT_BIAS_HW_EPS"))) : 4;
    }
    bool decal_le = (hw_scale > 1 || hw_eps > 0) && (st.poly[3] & 4) && !nobias &&
                    (st.poly_offset[0] < 0 || st.poly_offset[1] < 0) && st.ds[0] && !st.ds[1] && st.ds[2] == 3;
    // depth / stencil
    static uint64_t last_dsk;
    static id<MTLDepthStencilState> last_dss;
    uint64_t dsk = hmix(hmix(hmix(0xcbf29ce484222325ull, st.ds, 8), st.stencil_mask, sizeof st.stencil_mask), st.stencil_vmask, sizeof st.stencil_vmask) ^ decal_le;
    id<MTLDepthStencilState> dss = dsk == last_dsk ? last_dss : nil;
    NSString *dk = nil;
    if (!dss) {
        dk = [NSString stringWithFormat:@"ds %02x%02x%02x%02x%02x%02x%02x%02x %x %x %x %x%s", st.ds[0], st.ds[1], st.ds[2], st.ds[3],
                    st.ds[4], st.ds[5], st.ds[6], st.ds[7], st.stencil_mask[0], st.stencil_vmask[0], st.stencil_mask[1], st.stencil_vmask[1],
                    decal_le ? " le" : ""];
        dss = renderer_cache_get(dk);
    }
    if (!dss) {
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        static int nodt = -1;
        if (nodt < 0) nodt = getenv("NXRT_NO_DEPTH_TEST") != NULL;
        if (st.ds[0] && !nodt) {
            dd.depthCompareFunction = decal_le ? MTLCompareFunctionLessEqual : renderer_compare_fn(st.ds[2]);
            dd.depthWriteEnabled = st.ds[1] != 0;
        }
        else { dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = NO; }
        if (st.ds[3]) {
            uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
            for (int f = 0; f < 2; f++) {
                MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
                sd.stencilCompareFunction = renderer_compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF);
                uint32_t o = f ? ops >> 12 : ops;
                sd.stencilFailureOperation = renderer_stencil_op(o & 0xF);
                sd.depthFailureOperation = renderer_stencil_op((o >> 4) & 0xF);
                sd.depthStencilPassOperation = renderer_stencil_op((o >> 8) & 0xF);
                sd.readMask = st.stencil_vmask[f] & 0xFF;
                sd.writeMask = st.stencil_mask[f] & 0xFF;
                if (f) dd.backFaceStencil = sd; else dd.frontFaceStencil = sd;
            }
        }
        dss = [dev newDepthStencilStateWithDescriptor:dd];
        renderer_cache_set(dk, dss);
    }
    [enc setDepthStencilState:dss];
    last_dsk = dsk;
    last_dss = dss;
    [enc setStencilFrontReferenceValue:st.stencil_ref[0] backReferenceValue:st.stencil_ref[1]];
    [enc setBlendColorRed:st.blend_color[0] green:st.blend_color[1] blue:st.blend_color[2] alpha:st.blend_color[3]];

    // rasterizer
    uint8_t cull = st.poly[0];
    if (vkey.flip_x) cull = 0;  // (mirror mode's composite: one full-screen quad, either winding)
    [enc setCullMode:cull == 1 ? MTLCullModeFront : cull == 2 ? MTLCullModeBack : MTLCullModeNone];
    bool ccw = st.poly[1] == 1;
    [enc setFrontFacingWinding:(ccw != vkey.flip_y) ? MTLWindingCounterClockwise : MTLWindingClockwise];
    [enc setTriangleFillMode:st.poly[2] == 1 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    {   // polygon offset (decals on floors: without it they z-fight). Enables: bit 2 = fill (NVN PolygonOffsetEnable)
        static float last[3] = {0, 0, 0};
        float f[3] = {0, 0, 0};
        if ((st.poly[3] & 4) && !nobias) memcpy(f, st.poly_offset, sizeof f);
        if (decal_le) {  // (EPS in float units, 2^-24 at depth 0.5..1; a D16 buffer's unit is 2^-16)
            f[0] *= hw_scale;
            f[1] = f[1] * hw_scale - (st.rt.depth && st.rt.depth->format == 0x32 ? hw_eps / 256 : hw_eps);
            f[2] *= hw_scale;
        }
        static int nlog;
        if ((st.poly[3] & 4) && nlog < 3 && getenv("NXRT_LOG_BIAS")) { nlog++; LOG("[bias] factor %g units %g clamp %g\n", st.poly_offset[0], st.poly_offset[1], st.poly_offset[2]); }
        if (memcmp(f, last, sizeof f) || enc != last_bias_enc) {
            [enc setDepthBias:f[1] slopeScale:f[0] clamp:f[2]];
            memcpy(last, f, sizeof f);
            last_bias_enc = enc;
        }
    }
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];

    // resources
    renderer_bind_cbufs(0, pr->vi);
    renderer_bind_cbufs(1, pr->fi);
    if (pr->vi->uses_gmem) [enc setVertexBuffer:g_guest_buffer offset:0 atIndex:GMEM_BUFFER_INDEX];
    static id<MTLBuffer> dbgvs;
    bool dbg_this = maxwell_dbg_vs && (const uint8_t *)(uintptr_t)st.program->stages[0].code_addr == maxwell_dbg_vs;
    if (dbg_this) {
        if (!dbgvs) dbgvs = [dev newBufferWithLength:2 * 64 * 8 * 16 options:MTLResourceStorageModeShared];
        memset(dbgvs.contents, 0xff, 2 * 64 * 8 * 16);
        [enc setVertexBuffer:dbgvs offset:0 atIndex:DBG_VS_BUFFER_INDEX];
    }
    if (pr->fi->uses_gmem) [enc setFragmentBuffer:g_guest_buffer offset:0 atIndex:GMEM_BUFFER_INDEX];
    renderer_bind_textures(0, pr->vi);
    renderer_bind_textures(1, pr->fi);
    {   // the textures this draw samples, as the game knows them (tex_identify)
        g_draw.ntex = 0;
        extern int renderer_texpatch_of(NvnTexture *t);
        for (int i = 0; i < pr->fi->ntex && i < 64 && g_draw.ntex < NX_DRAW_MAX_TEX; i++) {
            int id = renderer_texpatch_of(renderer_shader_texture_object(1, &pr->fi->tex[i]));
            if (id >= 0) g_draw.tex_ids[g_draw.ntex++] = id;
        }
        if (NX_HOOK(draw_textures) && !NX_HOOK(draw_textures)(&g_draw)) { skip_note(2875); return; }
    }
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image: draws that sample the full-size reshaped scene (the final
        // composite, blurred menu backgrounds) and untextured full-screen fades fill the window; everything
        // else (HUD, text, menus, movie frames) goes into a centred 16:9 box.
        bool reads_scene = false;  // samples a reshaped texture the size of the framebuffer (the scene itself)
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        // Full-screen overlays without textures (fades, flashes) cover the whole window too.
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        // A full-screen quad sampling a reshaped (screen-shaped) texture is a scene background too, even a
        // downscaled one: the blurred scene behind the pause menu reads a 192x108 copy.
        // (single-texture draws only: the menu's frosted panels sample the same blurred copy but are UI pieces)
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1))
            reads_scene = true;
        extern uint64_t renderer_present_count(void);
        static uint64_t wide_frame = ~0ull;  // the latest draw this frame filled the window (nothing boxed since)
        extern uint64_t renderer_exec_frame(void);  // (the frame being translated)
        if (reads_scene) wide_frame = renderer_exec_frame();
        // Black or white fills are fades and flashes: always the whole window. Coloured ones (the file-select
        // tint) stay boxed unless they directly follow the composite. Colour = first fragment cbuf (rgb).
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_exec_frame());
        if (!composite) wide_frame = ~0ull;
        static int log_fills = -1;  // NXRT_LOG_FILLS: every full-screen untextured fill (fs hash, colour cbuf, decision)
        if (log_fills < 0) log_fills = getenv("NXRT_LOG_FILLS") != NULL;
        if (log_fills && fullscreen_fill) {
            static uint64_t lastf; static int nthis;
            uint64_t f = renderer_present_count();
            if (f != lastf) { lastf = f; nthis = 0; }
            if (nthis++ < 4 && f % 6 == 0) {
                const float *c = st.ubuf[1][0].addr ? (const float *)(uintptr_t)st.ubuf[1][0].addr : NULL;
                LOG("[fill] frame %llu fs=%llx composite %d cb0 %g %g %g %g | %g %g %g %g\n", (unsigned long long)f,
                    st.program->stages[1].code_addr, composite, c ? c[0] : -1, c ? c[1] : -1, c ? c[2] : -1, c ? c[3] : -1,
                    c ? c[4] : -1, c ? c[5] : -1, c ? c[6] : -1, c ? c[7] : -1);
            }
        }
        if (g_debug_frame && getenv("NXRT_LOG_UI")) {
            LOG("[ui2] fs=%llx count %llu ntex %d reads_scene %d fill %d quad %d -> %d |", st.program->stages[1].code_addr, r->x[2],
                pr->fi->ntex, reads_scene, fullscreen_fill, fullscreen_quad, composite);
            for (int i = 0; i < pr->fi->ntex && i < 4; i++)
                LOG(" t%d %dx%d sc %.2f,%.2f", i, tex_wh[1][i][0], tex_wh[1][i][1], tex_sc[1][i][0], tex_sc[1][i][1]);
            LOG(" | rt %ux%u vp %.0f,%.0f %.0fx%.0f\n", st.rt.color[0]->width, st.rt.color[0]->height, st.viewport[0][0],
                st.viewport[0][1], st.viewport[0][2], st.viewport[0][3]);
        }
        if (!composite) {  // centred 16:9 region with a uniform scale
            int pw, ph;
            renderer_pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    {   // camera-attached effects sized for 16:9 are widened with the view (runtime/hle/widescreen.c)
        float k = NX_HOOK(draw_effect_stretch) ? NX_HOOK(draw_effect_stretch)(renderer_cur_fs_hash()) : 1.0f;
        double vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        if (k > 1.0f) { vx -= vw * (k - 1) / 2; vw *= k; }
        [enc setViewport:(MTLViewport){vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf}];
    }
    renderer_apply_scissor();
    if (pr->fi->uses_rscale) {  // [0] = render target scale, [1 + i] = scale of texture i
        float rs[66][4] = {{vp_sx, vp_sy, vp_ox, vp_oy}};  // [1 + i] = scale x, y, content part x, y of texture i
        for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
            rs[1 + i][0] = tex_sc[1][i][0]; rs[1 + i][1] = tex_sc[1][i][1]; rs[1 + i][2] = tex_fr[1][i][0]; rs[1 + i][3] = tex_fr[1][i][1];
        }
        if (fkey.flip_x) rs[65][0] = (float)st.rt.color[0]->width;  // (mirror mode: the composite's width, guest pixels)
        [enc setFragmentBytes:rs length:(NSUInteger)(fkey.flip_x ? 66 : 1 + pr->fi->ntex) * 16 atIndex:RSCALE_BUFFER_INDEX];
    }
    // (uniform buffer 2 = the model matrix - only when the vertex shader reads it: a binding left from an earlier draw
    // is another object's matrix)
    bool model_ubo = (pr->vi->cbuf_mask >> (cbuf_base + 2)) & 1;
    if (model_ubo && st.ubuf[0][2].addr && st.ubuf[0][2].size >= 48 && !(st.ubuf[0][2].addr & 3)) {  // cut bushes' items

    }
    if (ENV("NXRT_POT_PROBE") && st.ubuf[0][2].addr && st.ubuf[0][2].size >= 48) {  // (debug: bush / pot draws)
        uint32_t ninst = indexed ? (uint32_t)r->x[6] : (uint32_t)r->x[4];
        extern uint64_t renderer_present_count(void);
        static uint64_t pf; static int pn;
        if (renderer_present_count() != pf) { pf = renderer_present_count(); pn = 0; }
        if (ninst > 1 && pf % 300 == 0 && pn++ < 40) {
            const float *m = (const float *)(uintptr_t)st.ubuf[0][2].addr;
            LOG("[potprobe] instanced x%u count %u tint %d ubuf sizes %u %u %u %u %u %u  m2 t (%.2f %.2f %.2f)\n", ninst,
                (uint32_t)r->x[2], pr->fi->uses_tint, (unsigned)st.ubuf[0][0].size, (unsigned)st.ubuf[0][1].size,
                (unsigned)st.ubuf[0][2].size, (unsigned)st.ubuf[0][3].size, (unsigned)st.ubuf[0][4].size,
                (unsigned)st.ubuf[0][5].size, m[3], m[7], m[11]);
            for (int i = 0; i < st.nattrib; i++) LOG(" a%d(f%#x s%u o%u)", i, st.attrib[i] & 0xFF, (st.attrib[i] >> 8) & 0x1F, st.attrib[i] >> 13);
            for (int i = 0; i < 12; i++)
                if (st.vbuf[i].addr && i < st.nstream && st.stream[i][1]) {
                    const float *v = (const float *)(uintptr_t)st.vbuf[i].addr;
                    LOG(" vb%d(size %llu stride %u div %u: %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f)", i,
                        st.vbuf[i].size, st.stream[i][0], st.stream[i][1], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
                }
            LOG("\n");
            for (int i = 0; i < 4; i++)
                if (st.sbuf[0][i].addr) {
                    const float *f = (const float *)(uintptr_t)st.sbuf[0][i].addr;
                    LOG("   ssbo%d size %llu:", i, st.sbuf[0][i].size);
                    for (int k = 0; k < 48 && k * 4 < (int)st.sbuf[0][i].size; k++) LOG(" %.2f", f[k]);
                    LOG("\n");
                }
        }
    }
    if (pr->fi->uses_tint) {  // per-draw tint (the game's: highlights, recoloured items)
        float tint[RECOLOUR_TINT_FLOATS] = {0};
        if (NX_HOOK(draw_tint)) {
            static bool noted;
            if (!noted && getenv("NXRT_LOG_HOOKS")) noted = true, LOG("[hooks] draw_tint called\n");
            NX_HOOK(draw_tint)(&g_draw, tint);
            if (getenv("NXRT_NO_RECOLOUR") && tint[3] < 0.0f) memset(tint, 0, sizeof tint);  // (A/B: no recolour)
        }
        [enc setFragmentBytes:tint length:sizeof tint atIndex:19];
    }
    for (int s = 0; s < 12; s++) {
        if (!(used_streams & (1u << s))) continue;
        Range v = st.vbuf[s];
        if (v.addr) bind_guest(enc, true, v.addr, v.size ? v.size : 16, 18 + s);
        else { static const float zero[4]; [enc setVertexBytes:zero length:16 atIndex:18 + s]; }
    }
    if (used_streams & (1u << 31)) { static const float zero[4] = {0, 0, 0, 1}; [enc setVertexBytes:zero length:16 atIndex:30]; }

    // Instanced draws with some instances tinted by the game (each instance's model matrix in vertex storage buffer 0,
    // indexed by instance id): drawn as runs of instances, each run with its tint.
    // (Metal's instance_id includes the base instance, so a run [b + k, b + k + n) draws exactly those instances.)
    uint32_t inst_all = indexed ? (uint32_t)r->x[6] : (uint32_t)r->x[4], base_all = indexed ? (uint32_t)r->x[5] : (uint32_t)r->x[3];
    if (!inst_all) inst_all = 1;
    enum { MAX_RUNS = 64 };
    uint32_t run_at[MAX_RUNS], run_n[MAX_RUNS];
    bool run_gold[MAX_RUNS];
    float run_tint[MAX_RUNS][RECOLOUR_TINT_FLOATS];
    int nruns = 0;
    if (pr->fi->uses_tint && inst_all > 1 && st.sbuf[0][0].addr && NX_HOOK(instance_tint)) {
        uint64_t recs = st.sbuf[0][0].size / 64;
        if ((!NX_HOOK(instances_tinted) || NX_HOOK(instances_tinted)()) && (uint64_t)base_all + inst_all <= recs) {
            const float *m = (const float *)(uintptr_t)st.sbuf[0][0].addr;
            static bool noted_inst;
            if (!noted_inst && getenv("NXRT_LOG_HOOKS")) noted_inst = true, LOG("[hooks] instance_tint called\n");
            for (uint32_t k = 0; k < inst_all; k++) {
                float t[RECOLOUR_TINT_FLOATS] = {0};
                bool g = NX_HOOK(instance_tint)(m + (size_t)(base_all + k) * 16, t);
                if (nruns && run_gold[nruns - 1] == g) { run_n[nruns - 1]++; continue; }
                if (nruns == MAX_RUNS) { nruns = 0; break; }  // (too fragmented: draw as is)
                run_at[nruns] = k; run_n[nruns] = 1; run_gold[nruns] = g; memcpy(run_tint[nruns], t, sizeof t); nruns++;
            }
            if (nruns == 1 && !run_gold[0]) nruns = 0;
        }
    }
    if (!nruns) { run_at[0] = 0, run_n[0] = inst_all, nruns = 1, run_gold[0] = false; }
    for (int ri = 0; ri < nruns; ri++) {
    if (nruns > 1 || run_gold[0]) {  // (the first run's tint was set above: only overridden when it differs)
        float tint[RECOLOUR_TINT_FLOATS] = {0};
        if (run_gold[ri]) memcpy(tint, run_tint[ri], sizeof tint);
        [enc setFragmentBytes:tint length:sizeof tint atIndex:19];
    }
    // draw
    bool native;
    uint32_t mode = (uint32_t)r->x[0];
    MTLPrimitiveType pt = prim_type(mode, &native);
    if (!indexed) {
        uint32_t first = (uint32_t)r->x[1], count = (uint32_t)r->x[2], base_inst = base_all + run_at[ri], inst = run_n[ri];
        if (native) {
            [enc drawPrimitives:pt vertexStart:first vertexCount:count instanceCount:inst baseInstance:base_inst];
        } else if (mode == 6 || mode == 7 || mode == 9) {
            uint32_t n;
            id<MTLBuffer> ib = expand_indices(mode, count, NULL, 0, first, &n);
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:ib
                     indexBufferOffset:0 instanceCount:inst baseVertex:0 baseInstance:base_inst];
        } else { skip_note(3053); return; }
    } else {
        uint32_t itype = (uint32_t)r->x[1], count = (uint32_t)r->x[2];
        uint64_t iaddr = r->x[3];
        int32_t base_vertex = (int32_t)r->x[4];
        uint32_t base_inst = base_all + run_at[ri], inst = run_n[ri];
        if (native && itype != 0) {
            MTLIndexType it = itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
            uint32_t isz = itype == 1 ? 2 : 4;
            id<MTLBuffer> ib;
            uint64_t ioff;
            if (iaddr >= HEAP_BASE && iaddr + (uint64_t)count * isz <= HEAP_BASE + HEAP_MAX && (iaddr % isz) == 0) {
                ib = g_guest_buffer; ioff = iaddr - HEAP_BASE;
            } else {
                ioff = ring_alloc((void *)(uintptr_t)iaddr, (uint64_t)count * isz); ib = ring;
            }
            [enc drawIndexedPrimitives:pt indexCount:count indexType:it indexBuffer:ib indexBufferOffset:ioff
                         instanceCount:inst baseVertex:base_vertex baseInstance:base_inst];
        } else {
            uint32_t n;
            id<MTLBuffer> ib;
            if (native) {  // 8-bit indices: widen
                uint32_t *w = malloc(count * 4 + 4);
                for (uint32_t i = 0; i < count; i++) w[i] = ((const uint8_t *)(uintptr_t)iaddr)[i];
                ib = [dev newBufferWithBytes:w length:count * 4 + 4 options:MTLResourceStorageModeShared];
                free(w);
                n = count;
            } else {
                ib = expand_indices(mode, count, (const void *)(uintptr_t)iaddr, (int)itype, 0, &n);
                pt = MTLPrimitiveTypeTriangle;
            }
            [enc drawIndexedPrimitives:pt indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:0
                         instanceCount:inst baseVertex:base_vertex baseInstance:base_inst];
        }
    }
    }  // runs
    stat_draws++, gp_draws_all++;
    if (capture_first_frame() && (draw_watched || renderer_capture_watches(st.program->stages[1].code_addr))) {
        static NSMutableSet *msl_saved;
        if (!msl_saved) msl_saved = [NSMutableSet new];
        uint64_t fa = st.program->stages[1].code_addr, va = st.program->stages[0].code_addr;
        if (pr->fi && pr->fi->msl && ![msl_saved containsObject:@(fa)]) {
            [msl_saved addObject:@(fa)];
            char path[700];
            snprintf(path, sizeof path, "%s/%llx_fs.metal", g_capture_dir, (unsigned long long)fa);
            FILE *f = fopen(path, "w");
            if (f) fputs(pr->fi->msl, f), fclose(f);
            snprintf(path, sizeof path, "%s/%llx_vs.metal", g_capture_dir, (unsigned long long)va);
            f = pr->vi && pr->vi->msl ? fopen(path, "w") : NULL;
            if (f) fputs(pr->vi->msl, f), fclose(f);
        }
        LOG("[watch] draw %d vs=%llx fs=%llx\n", debug_draw_no, (unsigned long long)va, (unsigned long long)fa);
        draw_watched = true;
        renderer_end_pass();  // (snapshots the target as this draw left it; the next draw resumes the pass, loading)
        st.rt_dirty = true;
        debug_draw_no++;
    }
    draw_watched = false;
    if (dbg_this) {
        static int nlog;
        if (nlog++ < 4) {
            renderer_flush();
            const float *f = dbgvs.contents;
            LOG("[dbgvs] draw %d:", nlog);
            for (int i = 0; i < 2; i++)
                for (int v = 0; v < 4; v++) LOG(" i%d v%d (%g %g %g %g) uv(%g %g %g)", i, v, f[(i * 8 + v) * 4], f[(i * 8 + v) * 4 + 1], f[(i * 8 + v) * 4 + 2], f[(i * 8 + v) * 4 + 3],
                                                f[(512 + i * 8 + v) * 4], f[(512 + i * 8 + v) * 4 + 1], f[(512 + i * 8 + v) * 4 + 2]);
            LOG("\n");
        }
    }
}

// ------------------------------------------------------------------ executor
static void exec_list(uint64_t handle, int depth) {
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        if (r->flags & CMD_DEFERRED) {
            uint32_t len = r->size > (uint32_t)offsetof(CmdRec, extra) ? r->size - (uint32_t)offsetof(CmdRec, extra) : 0;
            if (r->src && len) memcpy(r->extra, (const void *)(uintptr_t)r->src, len);
        }
        void *x = r->extra;
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) exec_list(h[i], depth + 1);
            break;
        }
        case CMD_BIND_PROGRAM: bind_program((NvnProgram *)(uintptr_t)r->x[0], (uint32_t)r->x[1]); break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            {
                const uint64_t *tx = x;
                for (int i = 0; i < (int)r->x[0] && i < 8; i++) {
                    NvnTexture *t = (NvnTexture *)(uintptr_t)tx[i];
                    static int nlog;
                    if (t && (t->flags & 2) && nlog++ < 5) LOG("[metal] video texture %p used as render target\n", t);
                }
            }
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState old = st.rt;
            memset(&st.rt, 0, sizeof st.rt);
            int ncolor = (int)r->x[0];
            if (ncolor < 0 || ncolor > 8) {  // (NVN has 8 colour targets; anything else is a recording error)
                static int logged;
                if (!logged) logged = 1, LOG("[metal] SetRenderTargets names %d colour targets (clamped)\n", ncolor);
                ncolor = ncolor < 0 ? 0 : 8;
            }
            st.rt.ncolor = ncolor;
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            bool same = rt_same(&old, &st.rt);
            static int dedup = -1;
            if (dedup < 0) { const char *e = ENV("NXRT_MTL_RT_DEDUP"); dedup = !e || atoi(e); }
            if (dedup && same && enc && !st.rt_dirty) break;  // the targets already bound: the pass goes on
            if (!renderer_td_defer_on() && !(dedup && same)) {  // (off mode: the old targets' recorded clears become passes now)
                RtState new = st.rt;
                st.rt = old;
                renderer_td_flush_targets(&old);
                st.rt = new;
            }
            st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            renderer_snapshots_invalidate("clear-color");
            int index = (int)r->x[0];
            static int log_clears = -1;  // NXRT_LOG_CLEARS: every colour clear (frame, target size, mask, scissor)
            if (log_clears < 0) log_clears = getenv("NXRT_LOG_CLEARS") != NULL;
            if (log_clears && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x scissor %d,%d %dx%d\n", (unsigned long long)renderer_present_count(),
                    index, st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2],
                    st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
            else if (log_clears) LOG("[cclear] frame %llu idx %d: no target bound (ncolor %d)\n", (unsigned long long)renderer_present_count(), index, st.rt.ncolor);
            if (index < st.rt.ncolor && st.rt.color[index]) renderer_clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask) (runtime/nvn/cmdbuf.c clear_texture). Was never
            // executed: the game clears its text-box textures this way before drawing a new message, so the old
            // text stayed behind the new one (user 2026-10-08: "Load this file?" over "Please select a file.").
            // Done as a render-target clear: bind the texture alone, scissor = region, clear(), restore.
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            renderer_snapshots_invalidate("clear-texture");
            {   // the whole texture, every channel, not a target of the open pass: recorded for its next pass
                static int on = -1;
                if (on < 0) { const char *e = ENV("NXRT_MTL_LOAD_CLEAR"); on = !e || atoi(e); }
                const int32_t *rg = ct->region;
                bool whole = rg[3] <= 0 || rg[4] <= 0 || (rg[0] == 0 && rg[1] == 0 && rg[3] >= (int32_t)t->width && rg[4] >= (int32_t)t->height);
                bool open_here = enc && !st.rt_dirty && (st.rt.depth == t || (st.rt.ncolor > 0 && st.rt.color[0] == t) || (st.rt.ncolor > 1 && st.rt.color[1] == t) ||
                                                        (st.rt.ncolor > 2 && st.rt.color[2] == t) || (st.rt.ncolor > 3 && st.rt.color[3] == t));
                uint32_t mask = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF;
                if (on && whole && !ct->has_view && !open_here && mask == 0xF && !renderer_format_is_depth(t->format)) {
                    renderer_td_record(t, true, (int)r->x[6], ct->color, false, 0, false, 0);
                    if (!renderer_td_defer_on()) renderer_td_flush(t);
                    break;
                }
            }
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            renderer_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            static int nlog;
            if (nlog++ < 8 || g_debug_frame)
                LOG("[cleartex] %ux%u f%#x region %d,%d %dx%d color %g %g %g %g mask %#llx kind %llu\n", t->width, t->height, t->format,
                    rg[0], rg[1], rg[3], rg[4], ct->color[0], ct->color[1], ct->color[2], ct->color[3], r->x[4], r->x[6]);
            renderer_clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            renderer_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            renderer_snapshots_invalidate("clear-depth");
            if (st.rt.depth) {
                // (depth s0, depthMask x1, stencil x2, stencilMask x3)
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                renderer_clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1],
                      (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH:
            renderer_dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]);
            if (st.program) {
                static uint64_t seen[16];
                static int nseen;
                uint64_t cs = st.program->stages[5].code_addr;
                bool known = false;
                for (int i = 0; i < nseen; i++) known |= seen[i] == cs;
                if (!known && nseen < 16) {
                    seen[nseen++] = cs;
                    char path[64];
                    snprintf(path, sizeof path, "build/cs_%d.bin", nseen - 1);
                    FILE *f = fopen(path, "wb");  // (no build/ outside a source checkout: the app's data root)
                    if (f) fwrite((void *)(uintptr_t)cs, 1, 0x4000, f), fclose(f);
                    LOG("[dispatch] new compute program %d: cs %llx groups %llu %llu %llu images:", nseen - 1, cs, r->x[0], r->x[1], r->x[2]);
                    for (int i = 0; i < 8; i++) if (st.img[5][i]) LOG(" %d=%llx", i, st.img[5][i]);
                    LOG(" tex:");
                    for (int i = 0; i < 8; i++) if (st.tex[5][i]) LOG(" %d=%llx", i, st.tex[5][i]);
                    LOG(" ubuf:");
                    for (int i = 0; i < 8; i++) if (st.ubuf[5][i].addr) LOG(" %d=%llx/%llu", i, st.ubuf[5][i].addr, st.ubuf[5][i].size);
                    LOG(" ssbo:");
                    for (int i = 0; i < 8; i++) if (st.sbuf[5][i].addr) LOG(" %d=%llx/%llu", i, st.sbuf[5][i].addr, st.sbuf[5][i].size);
                    LOG("\n");
                }
            }
            if (g_debug_frame && st.program) {
                LOG("[dispatch] %llu %llu %llu prog %p cs %llx\n", r->x[0], r->x[1], r->x[2], st.program, st.program->stages[5].code_addr);
                FILE *f = fopen("build/cs_code.bin", "wb");
                if (f) fwrite((void *)(uintptr_t)st.program->stages[5].code_addr, 1, 0x4000, f), fclose(f);
                f = fopen("build/cs_ctrl.bin", "wb");
                if (f) fwrite(st.program->stages[5].control, 1, 0x900, f), fclose(f);
                LOG("[dispatch] images:");
                for (int i = 0; i < 8; i++) if (st.img[5][i]) LOG(" %d=%llx", i, st.img[5][i]);
                LOG(" tex:");
                for (int i = 0; i < 8; i++) if (st.tex[5][i]) LOG(" %d=%llx", i, st.tex[5][i]);
                LOG(" ubuf:");
                for (int i = 0; i < 8; i++) if (st.ubuf[5][i].addr) LOG(" %d=%llx/%llu", i, st.ubuf[5][i].addr, st.ubuf[5][i].size);
                LOG("\n");
            }
            break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            // (buffer, size, offset, dataSize, data): apply the update in command order
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            break;
        case CMD_FENCE_SYNC: {
            // The CPU may read GPU-written memory (compute results) once the fence is signaled.
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("NXRT_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_REPORT_COUNTER: {
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            static uint32_t seen;
            uint32_t type = (uint32_t)r->x[0];
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            // (Switch GPU ticks, 614.4 MHz: the game converts the difference to ns itself - see the Vulkan exec.c)
            if (!out) break;
            static int gpu = -1;
            if (gpu < 0) gpu = !(getenv("NXRT_MTL_COUNTER") && !strcmp(getenv("NXRT_MTL_COUNTER"), "cpu"));
            static uint64_t *prev_out;  // (frame pacing: the pair's start)
            out[0] = 0;
            out[1] = pace_end_counter(prev_out, out, ns_to_gpu_ticks(plat_now_ns()));
            if (gpu && type == 0) {  // (type 0: timestamp) the GPU's time at this point, once it's done
                renderer_submit();
                pthread_mutex_lock(&ctr_lock);
                if (nctr < CTR_MAX) {
                    ctr_pend[nctr].out = out, ctr_pend[nctr].start = prev_out;
                    ctr_pend[nctr].before = submit_seq, ctr_pend[nctr].after = submit_seq + 1;
                    nctr++;
                } else {
                    static int warned;
                    if (!warned++) LOG("[metal] timing counters: %d pending, this one keeps the host clock\n", CTR_MAX);
                }
                pthread_mutex_unlock(&ctr_lock);
            }
            prev_out = out;
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: {
            const RendererTexRef *t = x;
            renderer_copy_texture(&t[0], &t[1], r->x[6]);
            break;
        }
        case CMD_DOWNSAMPLE: {
            const RendererTexRef *t = x;
            renderer_copy_texture(&t[0], &t[1], 1);
            break;
        }
        case CMD_COPY_BUFFER_TO_TEXTURE: renderer_copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: renderer_copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        default: break;
        }
        p += r->size;
    }
}

// In-place reboot (runtime/nvn/metal.m backend_reset; GPU idle, the game's textures already destroyed): drop
// everything keyed by guest addresses or guest-owned objects - programs (by code address), pipelines (by program),
// views, snapshots, bound state - so the next boot can't hit a stale entry. Metal's own shader cache stays warm.
// ------------------------------------------------------------------ shader preparation (shader_prep.h)
// Each record: translated here (the translator isn't re-entrant), compiled and built on a few worker threads (at most
// one build per core so the start screen stays smooth), kept in the g_prep_* tables for the game.
static dispatch_semaphore_t g_prep_slots;

// NXRT_SHADER_STRICT=1: at exit, a first visit or a skipped draw on the route is a coverage failure.
void shader_strict_exit(void) {
    if (!getenv("NXRT_SHADER_STRICT")) return;
    int f = shader_strict_first_visits(), s = shader_strict_skips();
    LOG("[shader] strict: %d first visits, %d skipped draws\n", f, s);
    if (f || s) _exit(1);
}

bool renderer_prepare_begin(void) {
    static bool strict_armed;
    if (!strict_armed) { strict_armed = true; atexit(shader_strict_exit); }
    if (!dev) {
        extern void *ui_metal_device(void);
        dev = (__bridge id<MTLDevice>)ui_metal_device();
        if (!dev) return false;
    }
    plat_sleep_watch(sleep_flush);
    renderer_prep_init();
    if (!g_prep_queue) {
        g_prep_queue = dispatch_queue_create("nxrt.prepare", DISPATCH_QUEUE_CONCURRENT);
        long n = (long)NSProcessInfo.processInfo.activeProcessorCount - 2;
        g_prep_slots = dispatch_semaphore_create(n < 2 ? 2 : n > 12 ? 12 : n);
    }
    return true;
}

int renderer_prepare_pending(void) { return renderer_prep_pending(); }
void renderer_prepare_end(void) {  // (the binary archive: saved now, off this thread - a PLAY waiting on the
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ renderer_archive_save(false); });  // preparation doesn't wait for it)
    LOG("[metal] shader archive: %d pipelines loaded from it, %d added; ready after %.0f ms; library compiles %.0f ms, "
        "pipeline builds %.0f ms (summed over threads)\n", renderer_archive_hits(), renderer_archive_adds(), renderer_archive_open_us() / 1e3,
        renderer_archive_library_us_total() / 1e3, renderer_archive_pipeline_us_total() / 1e3);
}

static void prep_run(void (^job)(void)) {
    dispatch_semaphore_wait(g_prep_slots, DISPATCH_TIME_FOREVER);
    renderer_prep_pending_add(1);
    dispatch_async(g_prep_queue, ^{
        @autoreleasepool { job(); }
        renderer_prep_pending_add(-1);
        dispatch_semaphore_signal(g_prep_slots);
    });
}

bool renderer_prepare(const ShaderRec *r, const ShaderBlob *vsb, const ShaderBlob *fsb) {
    if (r->kind == 'C') {
        uint64_t ck = renderer_compute_content_key(r->vs, r->local);
        bool have = renderer_prep_cs(ck) != nil;
        if (have) return true;
        CsProg *cp = calloc(1, sizeof *cp);
        memcpy(cp->local, r->local, sizeof cp->local);
        if (!renderer_compute_translate(cp, vsb->code, vsb->const_offset ? vsb->const_offset : 0x10000, 0)) return false;
        renderer_prep_put_cs(ck, [NSValue valueWithPointer:cp]);
        prep_run(^{ renderer_compute_compile(cp); });
        return true;
    }
    ShaderRec norm = *r;  // canonical: unused targets / blend factors cannot make a second record
    shader_rec_normalize(&norm);
    r = &norm;
    // the keys the draw path makes for this state (draw)
    ShaderKey vkey = {0}, fkey = {0};
    vkey.flip_y = fkey.flip_y = (r->flags & SR_FLIP_Y) != 0;
    vkey.depth_minus_one = (r->flags & SR_DEPTH_MINUS_ONE) != 0;
    vkey.flip_x = fkey.flip_x = (r->flags & SR_FLIP_X) != 0;
    for (int i = 0; i < r->nattrib; i++) vkey.attr_kind[i] = (uint8_t)renderer_attr_kind(r->attrib[i] & 0xFF);
    for (int i = 0; i < r->ncolor && i < 8; i++) if (r->color[i]) fkey.rt_kind[i] = (uint8_t)renderer_rt_kind(r->color[i]);
    size_t nf = 0; const ShaderFact *facts = renderer_facts(&nf);
    if (nf) {  // a hand-written fact wins over the format guess
        const ShaderFact *vf = shader_fact_find(facts, nf, r->vs);
        const ShaderFact *ff = shader_fact_find(facts, nf, r->fs);
        if (vf) shader_fact_apply(vf, vkey.attr_kind, fkey.rt_kind);
        if (ff) shader_fact_apply(ff, vkey.attr_kind, fkey.rt_kind);
    }
    uint64_t ck = renderer_prog_content_key(r->vs, r->fs, &vkey, &fkey);
    Prog *pr = renderer_prep_prog(ck);
    if (!pr) {
        pr = [Prog new];
        renderer_translate_program(pr, vsb->code, vsb->const_offset ? vsb->const_offset : 0x10000, fsb->code,
                          fsb->const_offset ? fsb->const_offset : 0x10000, &vkey, &fkey, 0, 0);
        renderer_prep_put_prog(ck, pr);  // (failed ones too: the game then skips them as it would its own)
    }
    if (pr->failed) return false;
    ShaderRec rec = *r;
    prep_run(^{
        @synchronized(pr) { if (!pr->ready && !pr->failed) renderer_compile_program(pr); }
        if (!pr->ready) return;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        renderer_attachment_formats_rec(pd, &rec);
        pipeline_desc(pd, pr, &rec);
        NSString *body = pipeline_key(pd, pr);
        if (prep_find_pipe(body)) return;
        NSError *err = nil;
        id<MTLRenderPipelineState> ps = renderer_archive_render_pipeline(pd, pr, &err);
        if (!ps) { static int n; if (n++ < 10) LOG("[shader] prepared pipeline failed: %s\n", err.localizedDescription.UTF8String); return; }
        renderer_prep_put_pipe(body, ps);
    });
    return true;
}

void renderer_reset(void) {
    renderer_td_reset();
    renderer_flush();
    pthread_mutex_lock(&gone_lock);
    ngone = 0;
    pthread_mutex_unlock(&gone_lock);
    renderer_views_reset();
    renderer_programs_reset();
    memset(pipe_tab, 0, sizeof pipe_tab);
    renderer_cache_clear();
    renderer_compute_reset();
    @synchronized(g_ready_pipes) { [g_ready_pipes removeAllObjects]; }
    g_boot_gen++;
    renderer_feedback_reset();
    renderer_snapshots_reset();
    renderer_feedback_reset();
    renderer_set_cur_fs_hash(0);
    last_bias_enc = nil;
    pthread_mutex_lock(&ctr_lock);
    nctr = 0;  // (pending timing counters point into the old boot's memory)
    pthread_mutex_unlock(&ctr_lock);
    pthread_mutex_lock(&cb_lock);
    last_cb = nil;
    for (int i = 0; i < CB_HIST; i++) cb_hist[i] = nil;  // (they retain the old guest heap buffer)
    pthread_mutex_unlock(&cb_lock);
    st_defaults();
}

uint64_t g_debug_exec_frame, g_debug_exec_end;  // (NXRT_DUMP_TRIGGER) the frames to dump [start, end), as the render
                                                 // thread translates them (0: none)
extern char g_capture_dir[512];  // (NXRT_DUMP_TRIGGER) where the textures sampled in those frames are saved
void backend_execute(uint64_t handle) {
    @autoreleasepool {
        // a trigger's debug frame is the frame whose commands run here: the presenting thread's flag covers only the
        // commands that happen to run while it presents (a partial frame)
        if (g_debug_exec_frame) {
            uint64_t f = renderer_exec_frame();
            g_debug_frame = f >= g_debug_exec_frame && f < (g_debug_exec_end ? g_debug_exec_end : g_debug_exec_frame + 1);
        }
        exec_list(handle, 0);
        static int sync_submit = -1;
        if (sync_submit < 0) sync_submit = getenv("NXRT_SYNC_SUBMIT") ? 1 : 0;
        if (sync_submit) renderer_flush();  // old behaviour: finish the GPU work before returning
        else renderer_submit();  // the guest fences (nvnSyncWait) before reusing memory the GPU reads
        if (counters_want_next()) {  // (a timing counter ended the list: its buffer after it, empty)
            (void)cb();
            renderer_submit();
        }
    }
}
