// Entry: load modules, set up the kernel, run nn::init::Start on the main thread.
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "paths.h"
#include "settings.h"
#include "hle.h"
#include "kernel.h"
#include "nxgame.h"
#include "nx_app.h"
#include "nx_input.h"
#include "nx_save.h"
#include "nx_ui.h"

const char *g_romfs_dir;
int g_start_screen;                  // (runtime/nxgame.h) the start screen is up: nothing of the game is loaded
char g_window_shot_request[256];     // (runtime/nxgame.h) test hook: save the next presented frame here

// Generated code at a variant site (gen.py --game: the game's VARIANTS): the instruction word to run, 0 = original
uint32_t nx_code_variant(uint64_t addr) { return NX_HOOK(code_variant) ? NX_HOOK(code_variant)(addr) : 0; }

const char *nx_gamelib_name(void) {
    static char name[64];
    if (!name[0]) snprintf(name, sizeof name, "%sgame.dylib", nx_game.info.id);
    return name;
}

// rtld normally passes these to nn::init::Start: run every module's init/fini arrays.
static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}

// nn::init::Start (SDK 16.2: Start(ulong, ulong, init_sdk, init, fini)) runs _init_libc0, nnosInitialize, x2,
// _init_libc1, nninitInitializeSdkModule, nninitStartup, _init_libc2, x3, nnMain, x4, nninitFinalizeSdkModule,
// nnosQuickExit: SDK constructors run early, the other modules' once nninitStartup has set up the game's heap.
// rtld runs a module's DT_INIT, which registers it (and its TLS template) with musl via
// __nnmusl_init_dso and then runs its init_array; fall back to the array if there is no DT_INIT.
// The SDK allocates each thread's TLS block through these two function pointers (nn::os::
// SetMemoryAllocatorForThreadLocal). rtld normally sets them before any module's constructors; nxrt does not run
// rtld, so a game whose SDK constructors allocate (Unity: iostreams) needs them set first. Guest-visible memory.
static void hle_tls_alloc(Cpu *c) {
    uint64_t size = c->x[0], align = c->x[1] ? c->x[1] : 16;
    if (align < 16) align = 16;
    uint64_t p = (uint64_t)hle_alloc(size + align);
    c->x[0] = (p + align - 1) & ~(align - 1);
}
static void hle_tls_free(Cpu *c) { (void)c; c->x[0] = 0; }  // (TLS blocks live as long as the process)

static bool skip_init(const char *name) {
    const char *list = getenv("NXRT_SKIP_INIT");  // test: a comma list of module names whose init/fini arrays to skip
    if (!list) return false;
    size_t n = strlen(name);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && !strncmp(p, name, n)) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

static void module_init(Cpu *c, Module *m) {
    if (skip_init(m->name)) { LOG("init %s skipped (NXRT_SKIP_INIT)\n", m->name); return; }
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n", m->name, m->init, m->init_array_sz / 8);
    if (m->init) guest_call(c, m->init);
    else run_array(c, m->init_array, m->init_array_sz);
}

// Early (x2): the SDK's own constructors. Late (x3, once its heap is up): the other modules'. By default every module
// but main runs early; games whose other modules need the SDK's heap (nx_game hooks late_module_init) run them late,
// main last (it uses the others).
static void vamm_init(Cpu *c) {  // rtld's job: create the virtual-address memory manager before the game allocates from it
    if (!getenv("NXRT_VAMM_INIT")) return;
    uint64_t f = loader_find_symbol("_ZN2nn2os30InitializeVirtualAddressMemoryEv");
    if (f) { LOG("vamm: InitializeVirtualAddressMemory %llx\n", (unsigned long long)f); guest_call(c, f); }
    else LOG("vamm: InitializeVirtualAddressMemory not found\n");
}
static void rtld_init_sdk(Cpu *c) {
    vamm_init(c);
    if (getenv("NXRT_SDK_INIT_LATE")) return;  // test: the sdk's constructors run in x3, after _init_libc1
    if (NX_HOOK(late_module_init)) {
        Module *sdk = loader_module("sdk");
        if (sdk) module_init(c, sdk);
        return;
    }
    for (int i = 1; i < g_nmodules; i++) module_init(c, &g_modules[i]);
}
static void rtld_init_main(Cpu *c) {
    if (!NX_HOOK(late_module_init)) { module_init(c, &g_modules[0]); return; }
    bool late_sdk = getenv("NXRT_SDK_INIT_LATE") != NULL;
    for (int i = g_nmodules - 1; i >= 0; i--)
        if (late_sdk || strcmp(g_modules[i].name, "sdk")) module_init(c, &g_modules[i]);
}
// After nnMain returns (x4, the newer SDKs' third callback): the game is quitting (nnosQuickExit follows); destructors
// aren't worth running.
static void rtld_fini(Cpu *c) { (void)c; LOG("nnMain returned\n"); }

// Claim the guest address range before anything else can map there. The host allocator places
// regions at random addresses in the same window; on a collision, re-exec for a fresh layout.
// Android: no re-exec (the app is forked from the zygote, whose layout it keeps): main() reports it.
static bool g_guest_reserved;
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    if ((g_guest_reserved = plat_reserve_guest(GUEST_LO, GUEST_HI))) return;
    const char *tries = getenv("NXRT_REEXEC");
    int n = tries ? atoi(tries) : 0;
    if (n >= 20) {
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
        exit(1);
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("NXRT_REEXEC", buf, 1);
    plat_exec(NULL);
    perror("re-exec");
    exit(1);
}

// ------------------------------------------------------------------ freeze watchdog
// A thread that notices when no frame was presented for 15 s and logs every guest thread (where it is, what it waits
// for): a freeze report then says what the game was doing. Long loads and shader compiles can trip it too.
static _Atomic uint64_t diagnostic_progress_ns, diagnostic_frame;
static _Atomic bool diagnostic_running;
static pthread_t diagnostic_thread;
static _Atomic uint64_t diagnostic_report_ns;

static bool diagnostic_stalled(uint64_t now) {
    uint64_t last = diagnostic_progress_ns;
    if (!last || now < last || now - last < 15000000000ull) return false;
    if (diagnostic_report_ns >= last && now - diagnostic_report_ns < 30000000000ull) return false;
    diagnostic_report_ns = now;
    return true;
}

static void *diagnostic_watchdog(void *arg) {
    (void)arg;
    while (diagnostic_running) {
        plat_sleep_until_ns(plat_now_ns() + 1000000000ull);
        uint64_t now = plat_now_ns();
        if (!diagnostic_running || !diagnostic_stalled(now)) continue;
        uint64_t last = diagnostic_progress_ns;
        LOG("[diagnostics] possible freeze: no presented frame for %llus (last frame %llu). "
            "Long loading or shader compilation can also cause this.\n",
            (unsigned long long)((now > last ? now - last : 0) / 1000000000ull), (unsigned long long)diagnostic_frame);
        if (!kernel_try_dump_threads()) LOG("[diagnostics] kernel lock busy; thread dump skipped to avoid blocking\n");
        fflush(stderr);
    }
    return NULL;
}

static void diagnostics_start(void) {
    diagnostic_progress_ns = plat_now_ns(), diagnostic_report_ns = 0;
    diagnostic_running = true;
    int error = pthread_create(&diagnostic_thread, NULL, diagnostic_watchdog, NULL);
    if (error) {
        diagnostic_running = false;
        LOG("[diagnostics] watchdog could not start: %s\n", strerror(error));
    } else LOG("[diagnostics] freeze watchdog enabled (15 seconds without a frame)\n");
}

// ------------------------------------------------------------------ boot / in-place reboot
// Going back to the start screen (or booting again) reboots the game inside this process: the window,
// GPU device, settings and shader caches stay. rt_request_reboot (menu, any thread) makes ui_run return on the
// main thread, which then: stops every guest thread (kernel_stop_threads: each unwinds at its next svc / HLE call /
// wait), resets the renderer (textures, address-keyed caches) and all per-boot host state (RT_ON_RESET hooks:
// services, HLE features, audio), empties the kernel, wipes guest memory, and boots again.
static _Atomic int g_reboot;
void rt_request_reboot(int kind) {
    LOG("[reboot] requested (%s)\n", kind == RT_REBOOT_TITLE ? "start screen" : "boot");
    g_reboot = kind;
}
int rt_reboot_requested(void) { return g_reboot; }

// NXRT_REBOOT_AT=<frame>:<profile | vanilla | title>[,...] (test): at that presented frame, switch to the game's
// boot profile (e.g. a randomizer seed) / vanilla in place, or go back to the start screen (with
// NXRT_LAUNCHER_AUTOPLAY it plays the saved choice).
// Frames count on across reboots. Called by the presenter every frame.
void reboot_test_frame(uint64_t frame) {
    diagnostic_frame = frame;  // (the presenter, every frame: the watchdog's progress)
    diagnostic_progress_ns = plat_now_ns();
    static const char *next;
    static int init;
    if (!init) { init = 1; next = getenv("NXRT_REBOOT_AT"); }
    long at;
    char what[128];
    if (!next || !*next || sscanf(next, "%ld:%127[^,]", &at, what) != 2 || (long)frame < at) return;
    next = strchr(next, ',');
    if (next) next++;
    if (!strcmp(what, "title")) { rt_request_reboot(RT_REBOOT_TITLE); return; }
    // (a test run: the next boot's profile without saving it - the settings file is the player's choice)
    if (NX_HOOK(profile_set)) NX_HOOK(profile_set)(strcmp(what, "vanilla") ? what : "");
    rt_request_reboot(RT_REBOOT_GAME);
}

extern const ResetFn __start_resets[] __asm__("section$start$__DATA$__rt_reset");
extern const ResetFn __stop_resets[] __asm__("section$end$__DATA$__rt_reset");

void backend_reset(void);             // renderer: GPU idle, game textures and address-keyed caches dropped
void backend_guest_memory_reset(void);  // renderer: guest memory was replaced (re-wrap the heap)
void backend_clear_window(void);      // a black frame while the game is down

static void reboot_idle(void) {  // (main thread, while guest threads stop) keep the window alive
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); exit(0); }
}

static void teardown(void) {
    uint64_t t0 = plat_now_ns();
    if (atomic_exchange(&diagnostic_running, false)) pthread_join(diagnostic_thread, NULL);
    kernel_stop_threads(reboot_idle);
    backend_clear_window();
    backend_reset();
    const ResetFn *start = __start_resets, *stop = __stop_resets;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    int n = 0;
    for (const ResetFn *p = start; p < stop; p++)
        if (*p) { (*p)(); n++; }
    kernel_reset();
    if (!plat_reset_guest(GUEST_LO, GUEST_HI)) FATAL("cannot reset guest memory\n");
    backend_guest_memory_reset();
    g_nmodules = 0;
    g_reboot = RT_REBOOT_NONE;
    LOG("[reboot] game torn down in %llums (%d reset hooks)\n", (unsigned long long)((plat_now_ns() - t0) / 1000000), n);
}

static void boot(const char *root) {
    char exefs[1024], romfs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    free((void *)g_romfs_dir);  // (each boot: the game folder may have moved since - start screen setup)
    g_romfs_dir = strdup(romfs);

    char why[512];
    if (!gamelib_load(root, why, sizeof why)) FATAL("%s\n", why);
    loader_load(exefs);
    if (NX_HOOK(modules_loaded)) NX_HOOK(modules_loaded)();  // (e.g. a seed's data patches into the fresh modules)
    { extern void settings_load(void); settings_load(); }
    settings_game_started();  // (the language it starts in: a later change asks for a restart)
    { extern void custom_tex_init(void); custom_tex_init(); }  // texture packs (nvn/custom_tex.c)
    kernel_init();
    dispatch_init();

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            stack_size = *(uint32_t *)(meta + 0x1C);
        }
        fclose(f);
    }
    LOG("main thread: prio %d, stack 0x%x\n", prio, stack_size);

    // (newer SDKs take a third callback: nn::init::Start(..., void (*)(), void (*)(), void (*)()))
    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    if (!start) start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) FATAL("nn::init::Start not found\n");
    uint64_t init_fn = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint64_t main_fn = hle_register("<rtld_init_main>", rtld_init_main);
    uint64_t fini_fn = hle_register("<rtld_fini>", rtld_fini);
    diagnostics_start();
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    if (getenv("NXRT_RO_STUB")) {
        // nn::ro keeps its entry points behind pointers the loader normally points at a table; give them a writable
        // slot so nn::ro::Initialize{,WithPortName} can install the implementation instead of storing to NULL.
        static const char *const ro[] = {"_ZN2nn2ro6detail36g_pLookupGlobalManualFunctionPointerE",
                                         "_ZN2nn2ro6detail34g_pLookupGlobalAutoFunctionPointerE"};
        for (int i = 0; i < 2; i++) {
            uint64_t slot = loader_find_symbol(ro[i]);
            if (!slot) { LOG("ro: %s not found\n", ro[i]); continue; }
            *(uint64_t *)slot = (uint64_t)hle_alloc(0x100);
            LOG("ro: %s = %#llx\n", ro[i], (unsigned long long)*(uint64_t *)slot);
        }
    }
    if (getenv("NXRT_TLS_ALLOC")) {
        uint64_t setter = loader_find_symbol("_ZN2nn2os32SetMemoryAllocatorForThreadLocalEPFPvmmEPFvS1_mE");
        if (setter) {
            t->cpu.x[0] = hle_register("<tls_alloc>", hle_tls_alloc);
            t->cpu.x[1] = hle_register("<tls_free>", hle_tls_free);
            guest_call(&t->cpu, setter);
            LOG("tls allocator set (alloc %#llx free %#llx)\n", (unsigned long long)t->cpu.x[0], (unsigned long long)t->cpu.x[1]);
        } else LOG("tls allocator: SetMemoryAllocatorForThreadLocal not found\n");
    }
    // rtld normally passes the process heap (svcSetHeapSize) as Start's two u64 arguments; NXRT_START_HEAP=<bytes>
    // does the same (the SDK's heap, which its own constructors allocate from; LAS's never do this early).
    const char *heap_env = getenv("NXRT_START_HEAP");
    const char *arg1_env = getenv("NXRT_START_ARG1");
    if ((heap_env && *heap_env) || (arg1_env && *arg1_env)) {
        uint64_t size = heap_env && *heap_env ? strtoull(heap_env, NULL, 0) : 0x40000000;
        kernel_set_heap(size);
        uint64_t arg1 = arg1_env && *arg1_env ? strtoull(arg1_env, NULL, 0) : size;
        LOG("main heap: %#llx bytes at %#llx (Start arg1 %#llx)\n", (unsigned long long)kernel_heap_size(),
            (unsigned long long)HEAP_BASE, (unsigned long long)arg1);
        const char *arg0_env = getenv("NXRT_START_ARG0");
        uint64_t arg0 = arg0_env && *arg0_env ? strtoull(arg0_env, NULL, 0) : t->handle;
        kernel_run_main(t, arg0, arg1, init_fn, main_fn, fini_fn);
    } else
        kernel_run_main(t, t->handle, 0, init_fn, main_fn, fini_fn);  // nn::init::Start(thread, args, init_sdk, init, fini)
}

// The layers under the runtime (nx-platform, -settings, -input, -save, -ui): who the app is, the runtime as their host.
static void layers_init(void) {
    const NxGameInfo *g = &nx_game.info;
    NxAppInfo app = {g->id, g->exe, g->title, g->bundle_id, g->data_dir, g->ini, g->version};
    nx_app_init(&app);
    plat_set_fault_handlers(rt_fault_watch, rt_fault_report);
    plat_set_crash_explain(rt_crash_explain);  // (the platform shows the plain-words message; the runtime can explain)
    plat_set_symbolizer(rt_host_fn_name);
    char lib_id[80];
    snprintf(lib_id, sizeof lib_id, "%s.id", nx_gamelib_name());
    rt_game_files_add(nx_gamelib_name());  // (the game code library is built into extracted/: the setup may delete it)
    rt_game_files_add(lib_id);
    // settings sections, in the order the file lists them
    rt_settings_register();
    nxin_init();
    nxui_init();
    nx_save_init();
    nxin_set_stick_deadzone(NX_HOOK(stick_deadzone));
    if (NX_HOOK(keyboard_prompts)) g_input_prompt_styles = 5;
    nx_save_set_profile_dir(NX_HOOK(content_dir));
    if (NX_HOOK(save_read)) nx_save_on_read(NX_HOOK(save_read));
    if (NX_HOOK(save_written)) nx_save_on_write(NX_HOOK(save_written));
    extern const NxUiBackend g_ui_backend_metal;
    nxui_set_backend(&g_ui_backend_metal);
}

int main(int argc, char **argv) {
    nx_log_init();  // NXRT_LOG / NXRT_LOG_FILE / NXRT_RUN_ID (docs/LOGGING.md)
    // --describe: the game, as JSON, and exit (a manager reads it: nxrt-frontend); --status: the game files and code too
    extern void rt_print_describe(void), rt_print_status(void);
    extern int rt_run_setup(const char *src, const char *mode);
    bool status_only = false;
    const char *setup_src = NULL, *setup_mode = "all";  // --setup <folder or package> ("": the game code only) ...
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--describe")) { rt_print_describe(); return 0; }
        if (!strcmp(argv[i], "--status")) status_only = true;
    }
    layers_init();
    {   // debug: NXRT_WATCH_MEM=lo,hi protects the page and attributes writes to the host function
        const char *wm = getenv("NXRT_WATCH_MEM");
        if (wm && *wm) {
            extern uint64_t g_watch_lo, g_watch_hi;
            extern void watch_arm(void);
            unsigned long long lo = 0, hi = 0;
            if (sscanf(wm, "%llx,%llx", &lo, &hi) >= 1) {
                g_watch_lo = lo;
                g_watch_hi = hi ? hi : lo + 8;
                watch_arm();
                fprintf(stderr, "[watch] armed %#llx-%#llx\n", lo, hi ? hi : lo + 8);
            }
        }
    }
    plat_init();
    plat_thread_name("nx-main");
    setvbuf(stderr, NULL, _IOLBF, 0);
    const char *root = "extracted";  // (in the data root)
    bool skip_launcher = getenv("NXRT_NO_LAUNCHER") || getenv("NXRT_INPUT");  // tests boot straight in
    static char arg_root[4096];  // (realpath: PATH_MAX)
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) skip_launcher = true;
        else if (!strcmp(argv[i], "--status")) {}
        else if (!strcmp(argv[i], "--setup")) setup_src = i + 1 < argc ? argv[++i] : "";
        else if (!strcmp(argv[i], "--setup-files")) setup_src = i + 1 < argc ? argv[++i] : "", setup_mode = "files";
        else if (!strcmp(argv[i], "--generate")) setup_src = "", setup_mode = "generate";
        else if (!strcmp(argv[i], "--compile")) setup_src = "", setup_mode = "compile";
        else if (argv[i][0] != '-' && !realpath(argv[i], arg_root)) root = argv[i];  // (relative to where we started)
        else if (argv[i][0] != '-') root = arg_root;
    }
    rt_paths_init();  // the data root becomes the working directory (nx-platform paths.h)
    // the game folder: a path on the command line, else the one chosen at setup (settings: e.g. a portable drive),
    // else extracted/ in the data root
    NX_LOGD(NX_LOG_APP, "mode: %s", status_only ? "status" : setup_src ? "setup" : skip_launcher ? "game" : "start screen");
    settings_load();
    if (root == arg_root || strcmp(root, "extracted")) rt_game_dir_set(root, false);  // (a folder of the player's: only what
                                                                                     //  the app builds there may go)
    else if (g_settings.game_dir[0]) rt_game_dir_set(g_settings.game_dir, !g_settings.game_in_place);
    { extern void rt_ui_register(void); rt_ui_register(); }
    if (status_only) { rt_print_status(); return 0; }
    if (setup_src) return rt_run_setup(setup_src, setup_mode);
    for (bool title = !skip_launcher;;) {
        if (title) {  // start screen (nx-ui launcher.mm): game files setup, profile choice, settings
            extern void ui_window_init(void);
            ui_window_init();
            g_start_screen = 1;
            launcher_run();
            g_start_screen = 0;
        }
        boot(rt_game_dir());
        int kind = ui_run();  // window and input on the main thread; returns when a reboot is requested
        teardown();
        title = kind == RT_REBOOT_TITLE;
    }
}
