// The runtime as nx-ui's host (nx_ui.h): its pages (Video, Audio, the system language on Controls), the game's pages
// and overlays (nx_game hooks menu_*), the start screen's game files (copied from the player's own and recompiled:
// runtime/gamebuild.c), its Prelaunch tab and first-play question, and the name dialog for the game (hle/swkbd.c).
#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <atomic>

#include "imgui.h"
#include "imgui_internal.h"
#include "option_rows.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include "gamebuild.h"
#include "gamelib.h"
#include "hle.h"
#include "menu.h"
#include "nx_ui.h"
#include "nxgame.h"
#include "nxpkg.h"
#include "nvn/shader_prep.h"
#include "paths.h"
#include "platform.h"
#include "romfs_build.h"
#include "settings.h"
#include "nvn/custom_tex.h"
SDL_Window *ui_window(void);
void renderer_get_scale(float *sx, float *sy);
double output_scale_dynamic_factor(void);
}

static std::string prep_romfs(void);  // (the start screen's shader preparation: defined below, used by the setup thread)

// OneDrive: is the data root inside a synced folder (Windows)? Decided once, remembered in the settings; the size is
// measured in the background.
static std::atomic<double> g_od_gb{-1};
static int g_od_state = -1;
static void *od_size_thread(void *) {
    double gb = (double)rt_tree_size(rt_data_root()) / 1e9;
    if (gb < 0.5) gb = 6.7;  // (nothing set up yet: game files and the built code)
    g_od_gb = gb;
    return NULL;
}
static int od_warn(double *gb) {
    if (g_od_state < 0) {
        g_od_state = !g_settings.onedrive_kept && (getenv("NXRT_TEST_ONEDRIVE") || plat_onedrive_synced(rt_data_root()));
        if (g_od_state) {
            pthread_t th;
            if (pthread_create(&th, NULL, od_size_thread, NULL) == 0) pthread_detach(th);
        }
    }
    *gb = g_od_gb;
    return g_od_state;
}
static void od_keep(void) {
    g_settings.onedrive_kept = true;
    settings_save();
    fprintf(stderr, "[launcher] OneDrive warning: kept here\n");
}

// ------------------------------------------------------------------ pages: Video, Audio
static bool tab_display(float) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##video", scale);
    opt_section("Display");
    const char *aspects[] = {"Unlocked", "Original 16:9"};
    opt_choice("Aspect ratio", &g_settings.aspect, aspects, 2, ASPECT_UNLOCKED,
               "Unlocked: the view fills the window, so wider (or taller) windows show more of the world - up to "
               "ultrawide, where the game supports it. Original 16:9: the game's own framing, with bars.");
    bool fs = g_ui_settings.fullscreen;
    if (opt_toggle("Fullscreen", &fs, false, "Fills the screen. Also: F11, Alt+Enter or Cmd+Ctrl+F.")) nxui_set_fullscreen(fs);
    opt_toggle("Show FPS", &g_ui_settings.show_fps, false, "Shows the frame rate in the top right corner.");
    if (NX_HOOK(max_frame_rate) >= 120) {  // (games whose timing follows rt_frame_rate)
        const char *rates[] = {"60 FPS", "120 FPS (Experimental)"};
        int rate = g_settings.frame_rate == 120;
        if (opt_choice("Frame rate", &rate, rates, 2, 0,
                       "Sets the rendering target while preserving game speed. 120 FPS requires faster hardware and a "
                       "high refresh rate display."))
            g_settings.frame_rate = rate ? 120 : 60;
    }

    opt_section("Rendering");
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    static const char *res[] = {"25%", "50%", "75%", "Match display", "Dynamic", "1x", "2x", "3x", "4x"};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_MATCH, RES_DYNAMIC, 1, 2, 3, 4};
    enum { RES_MATCH_AT = 3 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT;
        for (int i = 0; i < NRES; i++) if (res_mode[i] == g_settings.res_mode) ri = i;
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[400];
        int n = snprintf(d, sizeof d, "How sharp the 3D image is. Match display: your screen's own resolution. Dynamic: "
                                      "as sharp as your GPU keeps at full speed. 1x-4x: the Switch's 1080p times that. "
                                      "75%% / 50%% / 25%%: a share of the display, for slower machines.\n\nRendering at "
                                      "%.0f x %.0f.", 1920.0 * sx, 1080.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC)
            snprintf(d + n, sizeof d - n, " Dynamic: %.0f%% of the display right now.", output_scale_dynamic_factor() * 100);
        if (opt_choice("Internal resolution", &ri, res, NRES, RES_MATCH_AT, d, false)) g_settings.res_mode = res_mode[ri];
    }
    {   // each OS's own upscaler next to bilinear (UPSCALE_SHARP: MetalFX on macOS, AMD FSR 1 on Windows)
        static const char *ups[] = {"Bilinear", "MetalFX"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. MetalFX: Apple's "
                         "upscaler, sharper. Bilinear: plain and soft.";
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : g_settings.upscaler == UPSCALE_NIS ? 2 : 1;
        char udm[512];
        if (g_settings.res_mode == RES_MATCH) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, "%s\n\nNot used at Match display: the game already renders at your display's resolution.", ud);
            ud = udm;
            opt_disable_next();
        }
        if (opt_choice("Upscaler", &ui, ups, 2, 1, ud)) g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR;
    }
    static const char *aa[] = {"Off", "SMAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. Most useful at a lower internal resolution.");

    opt_end();
    return false;
}

static bool tab_audio(float) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Sound");
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    opt_toggle("Room reverb", &g_settings.reverb, true,
               "The game's echo in caves, dungeons and big rooms.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's movies (the opening and the ending). Applies the next time a movie starts.");
    opt_end();
    return false;
}


// ------------------------------------------------------------------ Mods page (rt_mods_ui.mm)
void tab_mods(float scale);
static bool tab_mods_page(float scale) { tab_mods(scale); return false; }

// ------------------------------------------------------------------ Controls page: the system language
// LiveSplit: the run clock's client (runtime/livesplit.c). The settings are the runtime's, so the rows are here
// (registered on the Controls tab, next to the system language).
static void livesplit_rows(void) {
    menu_header("LiveSplit");
    if (menu_toggle("Send the run clock to LiveSplit", &g_settings.livesplit)) settings_save();
    if (g_settings.livesplit) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Server");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##lshost", g_settings.livesplit_host, sizeof g_settings.livesplit_host)) settings_save();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Port");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputInt("##lsport", &g_settings.livesplit_port, 0, 0)) settings_save();
        menu_hint("LiveSplit's Server component (Tools > Start Server), default port 16834.");
    }
}

static void language_rows(void) {
    menu_header("System language");
    {
        // what the game is told the console's language is: its text follows this (runtime/rt_settings.c)
        char autol[64];
        snprintf(autol, sizeof autol, "Auto (%s)", settings_language_name());
        const char *names[N_LANGUAGES];
        names[0] = autol;
        for (int i = 1; i < N_LANGUAGES; i++) names[i] = g_language_names[i];
        if (menu_choice("Language", &g_settings.language, names, N_LANGUAGES)) settings_save();
    }
    if (settings_language_pending()) {  // (changed while the game runs: its text reloads only on a restart)
        menu_hint("Language change pending. The game must restart to reload its text. Save your progress first.");
        if (ImGui::Button("Apply language and restart...")) ImGui::OpenPopup("Restart for language");
    } else {
        menu_hint("The game's text language. Changes made before Play apply when the game starts.");
    }
    if (ImGui::BeginPopupModal("Restart for language", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            ImGui::CloseCurrentPopup();
        ImGui::TextUnformatted("Restart the game to apply the selected language?\nProgress since your last save will be lost.");
        if (ImGui::Button("Restart game")) {
            settings_save();
            rt_request_reboot(RT_REBOOT_GAME);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Spacing();
}

// ------------------------------------------------------------------ game files
static std::string g_root;  // the game folder (nx-platform rt_game_dir): <root>/exefs, <root>/romfs, what's built there

// Where Set up puts the game (the "Keep the game" row): KEEP_APP copies it into extracted/ in the data root;
// KEEP_IN_PLACE uses the player's own extract where it is (nothing copied: the data image and the game code are built
// next to it, e.g. on a portable drive); KEEP_FOLDER copies it into "<title> game" in a folder the player chose.
enum { KEEP_APP, KEEP_IN_PLACE, KEEP_FOLDER };
static int g_keep;              // (g_lock)
static std::string g_keep_dir;  // KEEP_FOLDER: the chosen folder (g_lock)
static void use_game_dir(const std::string &dir, bool owns) {
    g_root = dir;
    rt_game_dir_set(dir.c_str(), owns);
}
static bool is_abs(const std::string &p) { return !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':')); }

static bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// The copied game files alone ("" = there), without the game code built from them
static std::string check_copied_files(const std::string &root) {
    struct stat st;
    std::string parent = root.substr(0, root.find_last_of("/\\"));
    if (is_abs(root) && stat(root.c_str(), &st) != 0 && !parent.empty() && stat(parent.c_str(), &st) != 0)
        return "Game folder not found: " + root + " (is the drive connected?)";  // (a chosen folder on a drive that's away)
    FILE *f = fopen((root + "/exefs/main").c_str(), "rb");
    if (!f) return "No game files";
    uint8_t id[8] = {0};
    fseek(f, 0x40, SEEK_SET);
    size_t n = fread(id, 1, 8, f);
    fclose(f);
    if (n != 8 || memcmp(id, nx_game.info.build_id, 8)) return std::string("Wrong game version (needs v") + nx_game.info.game_version + ")";
    if (file_exists(root + "/romfs.src")) return "";  // (the RomFS read from the game package in place)
    if (!file_exists(root + "/romfs/" + nx_game.info.romfs_probe)) return "Game data missing";
    return "";
}

// "" = ready to play, else what is wrong
static std::string check_game_files(const std::string &root) {
    std::string why = check_copied_files(root);
    if (why.empty() && !gamelib_present(root.c_str())) return "Game code not built yet";
    return why;
}

// ------------------------------------------------------------------ setup (background thread)
// The player brings their own game files: a folder holding exefs/ and romfs/. They're copied into the data root (APFS
// clones / hard links where the file system allows) and the data image the game reads is built from romfs/
// (runtime/romfs_build.c).
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_src;           // the picked folder
static std::string g_job;           // current setup step / result for the status line
static std::string g_job_hint;      // a failure: what to do about it (shown under it)
static bool g_busy, g_job_failed;

static void set_job(const std::string &s, bool failed = false, const std::string &hint = "") {
    pthread_mutex_lock(&g_lock);
    g_job = s;
    g_job_failed = failed;
    g_job_hint = hint;
    pthread_mutex_unlock(&g_lock);
}

// Setup failures: a short headline and what to do next (also NXRT_LAUNCHER_SHOT=<f>:fail<1-8>, screenshots)
enum { FAIL_NOT_FOUND = 1, FAIL_UNUSABLE, FAIL_COPY, FAIL_VERSION, FAIL_MISSING, FAIL_BUILD, FAIL_IN_PLACE, FAIL_WRITE };
static void set_failure(int k) {
    static const char *const head[] = {"", "No game files found in that folder", "Those files can't be used",
                                       "Setup didn't finish", "Wrong version of the game", "Game files incomplete",
                                       "The game code didn't build", "The game can't be kept with those files",
                                       "Can't write to that folder"};
    static std::string version_hint = std::string(nx_game.info.title) + " needs version " + nx_game.info.game_version +
                                      " of the game, without updates.";
    const char *const hint[] = {
        "",
        "Choose the folder that holds the game files (exefs and romfs folders, or the game's .nsp / .xci with your "
        "prod.keys beside it).",
        "Compressed packages (.nsz / .xcz) can't be read: decompress them to .nsp / .xci first.",
        "Make sure there's about 12 GB free and try again.",
        version_hint.c_str(),
        "Some files are missing. Choose the folder again; if it happens again, copy your extracted game files again.",
        "Make sure there's about 2 GB free and try again. If it keeps happening, report it on GitHub.",
        "Keeping the game with your files needs the exefs and romfs folders side by side in one folder. Choose that "
        "folder, or keep the game in the app's folder or another one.",
        "Choose a folder you can write to (a drive formatted for macOS / Windows, not read-only), with room for the game "
        "data image and the game code."};
    if (k >= 1 && k <= 8) set_job(head[k], true, hint[k]);
}

static bool is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// exefs: the folder with main and main.npdm; romfs: the folder with the game's romfs_probe (its data root). Found
// in the picked folder or a few levels below it (some folder layouts nest them). Else package: the biggest NSP / XCI /
// NCA (the base game, not an update or DLC), read in place with keys (the player's prod.keys, beside it: nxpkg.h).
static void find_game(const std::string &dir, int depth, std::string &exefs, std::string &romfs, std::string &package,
                      uint64_t &package_size, std::string &keys, bool &unusable) {
    if (depth > 4 || (!exefs.empty() && !romfs.empty())) return;
    if (exefs.empty() && file_exists(dir + "/main") && file_exists(dir + "/main.npdm")) exefs = dir;
    if (romfs.empty() && file_exists(dir + "/" + nx_game.info.romfs_probe)) romfs = dir;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> sub;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n[0] == '.') continue;
        std::string ext = n.size() > 4 ? n.substr(n.size() - 4) : "";
        for (auto &ch : ext) ch = (char)tolower((unsigned char)ch);
        if (ext == ".nsz" || ext == ".xcz") unusable = true;  // (compressed packages: decompress them first)
        struct stat st;
        if ((ext == ".xci" || ext == ".nsp" || ext == ".nca") && !stat((dir + "/" + n).c_str(), &st) &&
            !S_ISDIR(st.st_mode) && (uint64_t)st.st_size > package_size) {
            package_size = (uint64_t)st.st_size;
            package = dir + "/" + n;
        }
        std::string lower = n;
        for (auto &ch : lower) ch = (char)tolower((unsigned char)ch);
        if (lower == "prod.keys" && keys.empty()) keys = dir + "/" + n;
        if (n != nx_game.info.romfs_probe && is_dir(dir + "/" + n)) sub.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(sub.begin(), sub.end());
    for (auto &s : sub) find_game(s, depth + 1, exefs, romfs, package, package_size, keys, unusable);
}

static bool build_id_ok(const std::string &main_path) {
    FILE *f = fopen(main_path.c_str(), "rb");
    if (!f) return false;
    uint8_t id[8] = {0};
    fseek(f, 0x40, SEEK_SET);
    size_t n = fread(id, 1, 8, f);
    fclose(f);
    return n == 8 && !memcmp(id, nx_game.info.build_id, 8);
}

// progress (the overlay's bar): the romfs copy, then the image build - each about the size of romfs/
static std::atomic<unsigned long long> g_done, g_total;
// The bar: copying and the data image are the first tenth, building the game code (runtime/gamebuild.c) the rest
static unsigned long long g_phase_base;
static std::atomic<float> g_build_frac{-1};  // -1: not building
static std::atomic<bool> g_code_failed{false};  // the last build failed: the files aren't ready, whatever is on disk
static bool g_copy_skipped;                  // (only the game code to build: the bar is all of that)
static void setup_progress(uint64_t done, uint64_t) { g_done = g_phase_base + done; }
static void build_progress(double f, const char *step) {
    g_build_frac = (float)f;
    static const char *last;
    if (step != last) last = step, set_job(step);
}
static float extract_progress(void) {
    unsigned long long t = g_total, d = g_done;
    float b = g_build_frac, f = b >= 0 ? (g_copy_skipped ? b : 0.1f + 0.9f * b) : t ? 0.1f * (float)((double)d / (double)t) : 0;
    return f < 0 ? 0 : f > 0.99f ? 0.99f : f;
}

// Builds the game code from the copied files (a few minutes); false = failed (the job line says so)
static bool build_game_code(void) {
    int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
    GameBuildOpts o = {build_progress, cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1};
    char why[256] = "";
    g_build_frac = 0;
    bool ok = gamebuild_run(g_root.c_str(), &o, why, sizeof why);
    g_code_failed = !ok;
    if (!ok) {
        fprintf(stderr, "[launcher] game code: %s\n", why);
        set_failure(FAIL_BUILD);
    }
    return ok;
}

static bool writable(const std::string &dir) {
    std::string t = dir + "/.write_test";
    FILE *f = fopen(t.c_str(), "wb");
    if (!f) return false;
    fclose(f);
    remove(t.c_str());
    return true;
}

// Setup from a game package (runtime/nxpkg.c): its ExeFS written out, the RomFS described in romfs.src and read from
// the package while playing (no data image to build). Kept with the player's files: a folder beside the package.
static bool package_setup(const std::string &package, const std::string &keys) {
    if (keys.empty()) {
        set_job("No prod.keys beside the game package", true, "Put the keys file from your own Switch (prod.keys) in "
                "the same folder as the game and choose it again.");
        return false;
    }
    pthread_mutex_lock(&g_lock);
    int keep = g_keep;
    std::string keep_dir = g_keep_dir;
    pthread_mutex_unlock(&g_lock);
    std::string dest = "extracted";
    if (keep == KEEP_IN_PLACE) dest = package.substr(0, package.find_last_of("/\\")) + "/" + nx_game.info.title + " game";
    else if (keep == KEEP_FOLDER) {
        if (keep_dir.empty()) { set_failure(FAIL_WRITE); return false; }
        dest = keep_dir + "/" + nx_game.info.title + " game";
    }
    use_game_dir(dest, true);
rt_remove_game_files(g_root.c_str());  // (an earlier or interrupted setup's)
    mkdir(g_root.c_str(), 0755);
    if (!writable(g_root)) { set_failure(FAIL_WRITE); return false; }
    LOG("[launcher] game folder: %s (from the package %s)\n", g_root.c_str(), package.c_str());
    set_job("Reading the game package...");
    char why[256] = "";
    uint64_t tid = 0;
    if (!nx_setup(package.c_str(), keys.c_str(), g_root.c_str(), &tid, why, sizeof why)) {
        fprintf(stderr, "[launcher] %s: %s\n", package.c_str(), why);
        why[0] = (char)toupper((unsigned char)why[0]);
        set_job("Those files can't be used", true, why);
        return false;
    }
    if (tid != nx_game.info.title_id || !build_id_ok(g_root + "/exefs/main")) {
        LOG("[launcher] package: title %016llx (wants %016llx)\n", (unsigned long long)tid,
            (unsigned long long)nx_game.info.title_id);
        rt_remove_game_files(g_root.c_str());
        set_failure(FAIL_VERSION);
        return false;
    }
    return true;
}

static void *extract_thread(void *) {
    std::string src;
    pthread_mutex_lock(&g_lock);
    src = g_src;
    pthread_mutex_unlock(&g_lock);
    g_done = 0, g_total = 0, g_phase_base = 0, g_build_frac = -1, g_copy_skipped = false, g_code_failed = false;
    std::string exefs, romfs, package, keys;
    uint64_t package_size = 0;
    bool unusable = false;  // (compressed game packages)
    if (src.empty() && check_copied_files(g_root).empty()) {  // copied already (an update needs new game code)
        g_copy_skipped = true;
        if (!build_game_code()) goto done;
        goto check;
    }
    set_job("Looking for the game files...");
    find_game(src, 0, exefs, romfs, package, package_size, keys, unusable);
    if ((exefs.empty() || romfs.empty()) && !package.empty()) {  // the game package: ExeFS out, RomFS read in place
        if (!package_setup(package, keys)) goto done;
        if (!build_game_code()) goto done;
        goto check;
    }
    if (exefs.empty() || romfs.empty()) { set_failure(unusable ? FAIL_UNUSABLE : FAIL_NOT_FOUND); goto done; }
    if (!build_id_ok(exefs + "/main")) { set_failure(FAIL_VERSION); goto done; }
    {
        pthread_mutex_lock(&g_lock);
        int keep = g_keep;
        std::string keep_dir = g_keep_dir;
        pthread_mutex_unlock(&g_lock);
        // where the game goes: its folder, and whether the app fills it (copies) or it is the player's extract
        std::string dest = "extracted";
        bool owns = keep != KEEP_IN_PLACE;
        if (keep == KEEP_IN_PLACE) {
            std::string parent = exefs.substr(0, exefs.find_last_of("/\\"));
            if (romfs.substr(0, romfs.find_last_of("/\\")) != parent || !file_exists(parent + "/exefs/main") ||
                !file_exists(parent + "/romfs/" + nx_game.info.romfs_probe)) {
                set_failure(FAIL_IN_PLACE);
                goto done;
            }
            dest = parent;
        } else if (keep == KEEP_FOLDER) {
            if (keep_dir.empty()) { set_failure(FAIL_WRITE); goto done; }
            dest = keep_dir + "/" + nx_game.info.title + " game";
        }
        mkdir(dest.c_str(), 0755);
        if (!writable(dest)) { set_failure(FAIL_WRITE); goto done; }
        use_game_dir(dest, owns);
        LOG("[launcher] game folder: %s (%s)\n", dest.c_str(), owns ? "copied there" : "the player's own files, kept");
        unsigned long long rbytes = romfs_dir_bytes(romfs.c_str());
        if (owns) {
            // (the picked folder can't be the app's own copy: setting up again deletes that first)
            char a[4096], b[4096];
            if (realpath(exefs.c_str(), a) && realpath(g_root.c_str(), b) && !strncmp(a, b, strlen(b)) &&
                (a[strlen(b)] == '/' || a[strlen(b)] == '\\' || !a[strlen(b)])) {
                set_failure(FAIL_NOT_FOUND);
                goto done;
            }
rt_remove_game_files(g_root.c_str());  // (an earlier or interrupted setup's; removes the folder too)
            mkdir(g_root.c_str(), 0755);
            g_total = 2 * rbytes;
            set_job("Copying the game files...");
            if (!romfs_copy_tree(exefs.c_str(), (g_root + "/exefs").c_str(), NULL) ||
                !romfs_copy_tree(romfs.c_str(), (g_root + "/romfs").c_str(), setup_progress)) {
                set_failure(FAIL_COPY);
                goto done;
            }
            g_phase_base = rbytes, g_done = rbytes;
        } else {
rt_remove_game_files(g_root.c_str());  // (what an earlier setup built here: the player's files stay)
            g_total = rbytes;
        }
        // (no data image: runtime/services/fs.c serves the RomFS from the folder, so the game's data isn't kept twice)
        if (!build_game_code()) goto done;
    }
check:
    {
        std::string why = check_game_files(g_root);
        if (why.empty()) {
            if (!prep_romfs().empty()) {
                set_job("Preparing shaders (Skip to play now)...");
                nxui_shader_prep_run();
            }
            set_job("Game files ready.");
            // remembered: the next start uses this game folder ("" = the default, extracted/ in the data root)
            bool chosen = g_root != "extracted";
            snprintf(g_settings.game_dir, sizeof g_settings.game_dir, "%s", chosen ? g_root.c_str() : "");
            g_settings.game_in_place = chosen && !rt_game_dir_owns_source();
            settings_save();
        }
        else set_failure(why.find("version") != std::string::npos ? FAIL_VERSION : FAIL_MISSING);
    }
done:
    pthread_mutex_lock(&g_lock);
    g_busy = false;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

// ------------------------------------------------------------------ the user's own Nintendo fonts (not shipped)
// runtime/hle/pl.c loads fonts/ first, else a close substitute (Hiragino Sans). Import copies recognized files only.
static const char *FONT_STD[] = {"nintendo_udsg-r_std_003.bfttf", "nintendo_udsg-r_std_003.ttf",
                                 "nintendo_udsgr_std_003.ttf", "nintendo_udsgr_std_003.bfttf"};
static const char *FONT_EXT[] = {"nintendo_ext_003.bfttf", "nintendo_ext_003.ttf"};
static std::vector<std::string> g_font_picks;
static std::string g_font_msg;

static bool have_font(const char *const *names, int n) {
    for (int i = 0; i < n; i++)
        if (file_exists(std::string("fonts/") + names[i])) return true;
    return false;
}

static void SDLCALL picked_fonts(void *, const char *const *files, int) {
    if (!files) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; files[i]; i++) g_font_picks.push_back(files[i]);
    pthread_mutex_unlock(&g_lock);
}

static void import_fonts() {
    pthread_mutex_lock(&g_lock);
    std::vector<std::string> picks;
    picks.swap(g_font_picks);
    pthread_mutex_unlock(&g_lock);
    if (picks.empty()) return;
    mkdir("fonts", 0755);
    int ok = 0, skipped = 0;
    for (auto &src : picks) {
        std::string base = src.substr(src.find_last_of('/') + 1), low = base;
        for (auto &c : low) c = (char)tolower(c);
        bool known = false;
        for (auto n : FONT_STD) known |= low == n;
        for (auto n : FONT_EXT) known |= low == n;
        if (!known) { skipped++; continue; }
        FILE *in = fopen(src.c_str(), "rb"), *out = fopen(("fonts/" + low).c_str(), "wb");
        if (in && out) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
            ok++;
        }
        if (in) fclose(in);
        if (out) fclose(out);
    }
    g_font_msg = std::to_string(ok) + " font file(s) imported" + (skipped ? ", " + std::to_string(skipped) + " not recognized" : "") +
                 ". Used from the next game start.";
}

static void remove_fonts() {
    for (auto n : FONT_STD) unlink((std::string("fonts/") + n).c_str());
    for (auto n : FONT_EXT) unlink((std::string("fonts/") + n).c_str());
    g_font_msg = "Imported fonts removed: the substitute font is used.";
}


// ------------------------------------------------------------------ background: a still of the game's title movie
static bool load_background(uint8_t **out, int *ow, int *oh) {
    if (!nx_game.info.title_movie) return false;
    std::string path = g_root + "/romfs/" + nx_game.info.title_movie;
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path.c_str(), NULL, NULL) < 0) return false;
    bool got = false;
    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (si >= 0 && avformat_find_stream_info(fmt, NULL) >= 0) {
        AVStream *st = fmt->streams[si];
        const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
        AVCodecContext *cc = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(cc, st->codecpar);
        if (codec && avcodec_open2(cc, codec, NULL) == 0) {
            AVPacket *pkt = av_packet_alloc();
            AVFrame *fr = av_frame_alloc();
            int64_t want = (int64_t)(6.0 / av_q2d(st->time_base));  // a few seconds in: the island, not black
            av_seek_frame(fmt, si, want, AVSEEK_FLAG_BACKWARD);
            while (!got && av_read_frame(fmt, pkt) >= 0) {
                if (pkt->stream_index == si && avcodec_send_packet(cc, pkt) == 0)
                    while (avcodec_receive_frame(cc, fr) == 0)
                        if (fr->best_effort_timestamp >= want) { got = true; break; }
                av_packet_unref(pkt);
            }
            if (got) {
                int w = fr->width, h = fr->height;
                uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
                SwsContext *sws = sws_getContext(w, h, (AVPixelFormat)fr->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
                uint8_t *dst[1] = {rgba};
                int dst_ls[1] = {w * 4};
                sws_scale(sws, fr->data, fr->linesize, 0, h, dst, dst_ls);
                sws_freeContext(sws);
                *out = rgba, *ow = w, *oh = h;
            }
            av_frame_free(&fr);
            av_packet_free(&pkt);
        }
        avcodec_free_context(&cc);
    }
    avformat_close_input(&fmt);
    return got;
}

static void reveal_folder(const std::string &dir) {
    std::string path = dir;
    bool abs = !path.empty() && (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'));
    if (!abs) path = std::string(rt_data_root()) + "/" + path;
    std::string url = "file://";
    if (path[0] != '/') url += "/";  // (Windows: file:///C:/...)
    for (char ch : path) {
        if (ch == '\\') ch = '/';
        if (ch == ' ') url += "%20";
        else if (ch == '%') url += "%25";
        else if (ch == '#') url += "%23";
        else url += ch;
    }
    SDL_OpenURL(url.c_str());
}

static bool g_files_deleted;  // set here: the start screen goes back to setting the game files up (files_deleted)

// ---- shader preparation (runtime/nvn/shader_prep.h): after setup, and in the background on later starts
static std::atomic<bool> g_prep_busy{false}, g_prep_started{false};
static std::atomic<float> g_prep_frac{-1.f};
static volatile int g_prep_skip;  // (beta_5's shader_prep_run takes a volatile cancel flag)
static bool prep_enabled(void) {
    const char *e = getenv("NXRT_SHADER_PREP");  // (0: off; the NXRT_SHADER_PREP=1 test path in objects.c is separate)
    return !e || atoi(e) != 0;
}
static std::string prep_romfs(void) {
    std::string root = g_root.empty() ? "extracted" : g_root;
    if (!(root[0] == '/' || (root.size() > 1 && root[1] == ':'))) root = std::string(rt_data_root()) + "/" + root;
    struct stat sb;
    return stat((root + "/romfs").c_str(), &sb) == 0 && S_ISDIR(sb.st_mode) ? root + "/romfs" : "";
}
static void prep_progress(int done, int total) { g_prep_frac.store(total ? (float)done / (float)total : 0.f); }
extern "C" void nxui_shader_prep_run(void) {
    if (!prep_enabled()) return;
    std::string romfs = prep_romfs();
    if (romfs.empty()) return;  // (a package: its bytes are read through the runtime, not a folder to scan)
    g_prep_skip = 0;
    g_prep_frac.store(0.f);
    g_prep_busy.store(true);
    if (!shader_prep_run(romfs.c_str(), prep_progress, &g_prep_skip))
        LOG("[launcher] shader preparation could not start\n");
    g_prep_frac.store(-1.f);
    g_prep_busy.store(false);
}
static void *prep_thread(void *) {
    nxui_shader_prep_run();
    return NULL;
}
extern "C" void nxui_shader_prep_start(void) {
    if (!prep_enabled() || g_prep_started.exchange(true)) return;
    g_prep_busy.store(true);  // (before the thread runs: the launcher must see it)
    pthread_t t;
    if (pthread_create(&t, NULL, prep_thread, NULL) == 0) pthread_detach(t);
    else g_prep_started.store(false), g_prep_busy.store(false);
}
extern "C" bool nxui_shader_prep_busy(void) { return g_prep_busy.load(); }
extern "C" float nxui_shader_prep_progress(void) {
    float p = g_prep_frac.load();
    return p < 0 ? 0.f : p > 1 ? 1.f : p;
}
extern "C" void nxui_shader_prep_skip(void) { g_prep_skip = 1; }
static std::string g_src_desc, g_setup_desc;  // the Game Files rows' explanations (the game's name and version: nxui_init)

// (defined with the rest of the game-files callbacks, below)
static bool files_busy(void);
static float files_progress(void);
static bool files_copied(void);
static void files_pick(const char *folder);
static void files_picked(char *out, size_t cap);
static void files_job(char *line, size_t n, char *hint, size_t hn, bool *failed);
static void files_start(void);
static bool files_remove(void);
static double files_size_gb(void);
static void setup_rows(float scale, bool busy);

static void SDLCALL picked_source(void *, const char *const *files, int) {
    if (files && files[0]) files_pick(files[0]);
}

static void prelaunch_tab(float sc) {
    const ImVec4 gold(0.93f, 0.76f, 0.33f, 1);
    auto heading = [&](const char *t) {
        ImGui::PushFont((ImFont *)nxui_start_font(1));
        ImGui::TextColored(gold, "%s", t);
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
    };
    char srcb[1100] = "", jobb[400] = "", hintb[400] = "";
    bool failed = false;
    files_picked(srcb, sizeof srcb);
    files_job(jobb, sizeof jobb, hintb, sizeof hintb, &failed);
    std::string src = srcb, job = jobb, hint = hintb;
    const bool busy = files_busy();
    const std::string status = check_game_files(g_root);
    heading("Game files");
    opt_begin("##gfiles", sc);  // (the rows column, with each row's explanation beside it: the same as the system tabs)
    if (!status.empty() || busy) {  // not set up yet (or setting up now): the Game Files page's own rows
        opt_section("Your copy of the game");
        if (busy) opt_disable_next();
        if (opt_action("Game files folder", src.empty() ? "Choose..." : "Selected", g_src_desc.c_str(), true))
            SDL_ShowOpenFolderDialog(picked_source, NULL, ui_window(), NULL, false);
        setup_rows(sc, busy);
        opt_section("Set up");
        if (busy || (src.empty() && !files_copied())) opt_disable_next();
        if (opt_action("Set up game files", busy ? "Working..." : "", g_setup_desc.c_str(), true) && !busy) files_start();
        if (busy) {
            ImGui::ProgressBar(files_progress(), ImVec2(-1, 0));
            if (nxui_shader_prep_busy() && ImGui::Button("Skip preparing shaders")) nxui_shader_prep_skip();
        }
    } else {
    {   // where they are kept (the Game Files page's "Keep the game")
        std::string where = is_abs(g_root) ? g_root : std::string(rt_data_root()) + "/" + g_root;
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Kept in %s%s", where.c_str(),
                            rt_game_dir_owns_source() ? "" : " (your own game files: only what the app built there is deleted)");
        ImGui::PopTextWrapPos();
    }
    static double gb = -1;
    if (gb < 0) gb = rt_game_files_size(g_root.c_str()) / 1e9;
    if (ImGui::Button("Show in Finder")) reveal_folder(g_root);
    ImGui::SameLine();
    static bool shot_del = getenv("NXRT_LAUNCHER_SHOT") && strstr(getenv("NXRT_LAUNCHER_SHOT"), ":askdelete");
    static int del_opened = -1;  // the frame the popup opened (ImGui can activate a default-focused button on that frame)
    if (ImGui::Button("Delete game files") || shot_del) ImGui::OpenPopup("##delfiles"), del_opened = ImGui::GetFrameCount(), shot_del = false;  // (test: :askdelete)
    if (ImGui::BeginPopupModal("##delfiles", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushFont((ImFont *)nxui_start_font(1));
        ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), "Are you sure?");
        ImGui::PopFont();
        if (rt_game_dir_owns_source()) ImGui::Text("This deletes %s's copy of the game files (%.1f GB).", nx_game.info.title, gb);
        else ImGui::Text("This deletes what %s built next to your game files (%.1f GB).", nx_game.info.title, gb);
        ImGui::TextDisabled("Saves and settings are kept. Your own game files aren't touched.");
        if (rt_dev_mode()) {  // (a source checkout: the files are the project's own extracted/)
            bool abs = !g_root.empty() && (g_root[0] == '/' || (g_root.size() > 1 && g_root[1] == ':'));
            std::string where = abs ? g_root : std::string(rt_data_root()) + "/" + g_root;
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Dev checkout: this is %s", where.c_str());
        }
        ImGui::Spacing();
        // a press on the frame the popup opened is not the player's: ImGui focuses this button and can activate it
        // while the popup is still being opened (it deleted on its own, every start, before this guard)
        bool armed = del_opened >= 0 && ImGui::GetFrameCount() > del_opened + 1;
        if (armed && ImGui::Button("Delete", ImVec2(220 * sc, 0))) {
            bool ok = rt_remove_game_files(g_root.c_str());  // (runtime/paths.c: only what the extraction made)
            set_job(ok ? "Game files deleted." : "Could not delete the game files (not in the app's data folder?).", !ok);
            g_files_deleted = true, gb = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button("Cancel", ImVec2(220 * sc, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    }  // (set up: what's there and how to free it)
    if (!job.empty()) {  // the step that ran / its result (the same line the start screen showed)
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(failed ? ImVec4(1, 0.5f, 0.4f, 1) : busy ? ImVec4(0.93f, 0.76f, 0.33f, 1)
                                  : ImVec4(0.45f, 0.95f, 0.55f, 1), "%s", job.c_str());
        if (failed && !hint.empty()) ImGui::TextColored(ImVec4(0.86f, 0.85f, 0.80f, 1), "%s", hint.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    ImGui::Spacing();
    heading("Game font");
    import_fonts();
    bool fs = have_font(FONT_STD, 4), fe = have_font(FONT_EXT, 2);
    ImGui::Text("Text: %s    Button icons: %s", fs ? "Nintendo font (imported)" : "substitute (Hiragino Sans)",
                fe ? "imported" : "substitute");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Fonts aren't included. For the original look, import your own copies of: "
                        "nintendo_udsg-r_std_003.bfttf (text) and nintendo_ext_003.bfttf (button icons). "
                        ".ttf versions with the same names work too.");
    ImGui::PopTextWrapPos();
    if (!g_font_msg.empty()) ImGui::Text("%s", g_font_msg.c_str());
    static const SDL_DialogFileFilter font_f[] = {{"Switch fonts", "bfttf;ttf"}};
    if (ImGui::Button("Import fonts")) SDL_ShowOpenFileDialog(picked_fonts, NULL, ui_window(), font_f, 1, NULL, true);
    if (fs || fe) {
        ImGui::SameLine();
        if (ImGui::Button("Use substitute font")) remove_fonts();
    }
    opt_end();
}


// ------------------------------------------------------------------ first PLAY: the aspect ratio
// First PLAY ever, before the menu note: authentic 16:9 or the unlocked (fill the window) aspect ratio.
static bool aspect_prompt(float s) {  // (nx_ui.h NxUiFirstPlay: the "##firstplay" popup)
    bool done = false;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fminf(ds.x * 0.8f, 760 * s), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * s, 28 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16 * s);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.03f, 0.04f, 0.05f, 0.94f));
    bool open = ImGui::BeginPopupModal("##firstplay", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (open) {
        ImGui::SetWindowFontScale(1.25f);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("How would you like to play?");
        ImGui::Spacing();
        ImGui::TextDisabled("You can change this any time in Settings.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14 * s));
        float bw = 300 * s, gap = 20 * s;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 2 * bw - gap) * 0.5f);
        int pick = -1;
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button("Authentic 16:9", ImVec2(bw, 44 * s))) pick = ASPECT_ORIGINAL;
        ImGui::SameLine(0, gap);
        if (ImGui::Button("Unlocked (fill the screen)", ImVec2(bw, 44 * s))) pick = ASPECT_UNLOCKED;
        if (pick >= 0) {
            g_settings.aspect = pick;
            g_settings.aspect_asked = true;
            settings_save();
            ImGui::CloseCurrentPopup();
            done = true;
        }
        ImGui::EndPopup();
    }
    return done;
}


// ------------------------------------------------------------------ Game Files page: where the game goes
static void SDLCALL picked_keep(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_lock);
    g_keep_dir = files[0];
    pthread_mutex_unlock(&g_lock);
}

static void setup_rows(float, bool busy) {
    pthread_mutex_lock(&g_lock);
    int keep = g_keep;
    std::string dir = g_keep_dir;
    pthread_mutex_unlock(&g_lock);
    opt_section("Where to keep the game");
    static const char *const names[] = {"In the app's folder", "With my game files", "In another folder"};
    static std::string desc = std::string(
        "Where the game files and the recompiled game go.\n\n"
        "In the app's folder: a copy, in ") + rt_data_root() + "/extracted.\n\n"
        "With my game files: nothing is copied. The game data image and the recompiled game are written into the "
        "folder that holds your exefs and romfs folders (for example on a portable drive), which stay as they are. "
        "The drive has to be connected to play.\n\n"
        "In another folder: a copy, in a folder you choose (for example on a portable drive).";
    if (busy) opt_disable_next();
    if (opt_choice("Keep the game", &keep, names, 3, KEEP_APP, desc.c_str())) {
        pthread_mutex_lock(&g_lock);
        g_keep = keep;
        pthread_mutex_unlock(&g_lock);
    }
    if (keep == KEEP_FOLDER) {
        std::string d = std::string("The game goes into \"") + nx_game.info.title + " game\" in this folder.";
        if (!dir.empty()) d += "\n\nChosen: " + dir;
        std::string v = dir.empty() ? "Choose..." : dir.substr(dir.find_last_of("/\\") + 1);
        if (busy) opt_disable_next();
        if (opt_action("Folder", v.c_str(), d.c_str(), false)) SDL_ShowOpenFolderDialog(picked_keep, NULL, ui_window(), NULL, false);
    }
}

static bool aspect_pending(void) { return !g_settings.aspect_asked; }
static void aspect_reset(void) { g_settings.aspect_asked = false; }

// ------------------------------------------------------------------ the game files, for the start screen
// "" = ready to play. A failed build is not ready even when an older library is still beside the exe (its id file can
// match while the code the player needs isn't the one that was built) - the job line says what went wrong.
static void files_status(char *out, size_t cap) {
    std::string why = check_game_files(g_root), job;
    if (g_code_failed) {
        pthread_mutex_lock(&g_lock);
        job = g_job;
        pthread_mutex_unlock(&g_lock);
        why = job.empty() ? "The game code didn't build" : job;
    }
    snprintf(out, cap, "%s", why.c_str());
}
static bool files_copied(void) { return check_copied_files(g_root).empty(); }
static bool files_busy(void) {
    pthread_mutex_lock(&g_lock);
    bool b = g_busy;
    pthread_mutex_unlock(&g_lock);
    return b || g_prep_busy.load();  // (the shader preparation runs inside the same "working" state)
}
static float files_progress(void) {
    if (g_prep_busy.load()) return nxui_shader_prep_progress();  // (after the copy/build: its own bar)
    return extract_progress();
}
static bool files_recompiling(void) { return g_build_frac >= 0; }
static void files_job(char *line, size_t n, char *hint, size_t hn, bool *failed) {
    pthread_mutex_lock(&g_lock);
    snprintf(line, n, "%s", g_job.c_str());
    snprintf(hint, hn, "%s", g_job_hint.c_str());
    *failed = g_job_failed;
    bool idle = g_job.empty() && !g_busy;
    pthread_mutex_unlock(&g_lock);
    if (idle) {  // nothing set up this session: say why the game isn't ready when its folder is away (a drive)
        std::string st = check_copied_files(g_root);
        if (st.find("not found") != std::string::npos) {
            snprintf(line, n, "%s", st.c_str());
            snprintf(hint, hn, "Connect the drive and come back to this page, or set the game up again here.");
            *failed = true;
        }
    }
}
static void files_set_job(const char *line, bool failed, const char *hint) { set_job(line, failed, hint ? hint : ""); }
static void files_pick(const char *folder) {
    pthread_mutex_lock(&g_lock);
    g_src = folder;
    pthread_mutex_unlock(&g_lock);
}
static void files_picked(char *out, size_t cap) {
    pthread_mutex_lock(&g_lock);
    snprintf(out, cap, "%s", g_src.c_str());
    pthread_mutex_unlock(&g_lock);
}
static void files_start(void) {
    pthread_mutex_lock(&g_lock);
    g_busy = true;
    pthread_mutex_unlock(&g_lock);
    pthread_t t;
    pthread_create(&t, NULL, extract_thread, NULL);
    pthread_detach(t);
}
static double files_size_gb(void) { return rt_game_files_size(g_root.c_str()) / 1e9; }
static bool files_remove(void) {
    g_code_failed = false;  // (a fresh start: the status is about the files again, whatever failed before)
    return rt_remove_game_files(g_root.c_str());
}
static bool files_deleted(void) {
    bool d = g_files_deleted;
    g_files_deleted = false;
    return d;
}
static void files_test_extract(const char *folder) {
    g_src = folder;
    g_busy = true;
    extract_thread(NULL);
}

// ------------------------------------------------------------------ the game's pages and overlays (nx_game hooks)
static NxUiTab g_game_tabs[16];
static bool game_tab_draw(int i, float scale) { return NX_HOOK(menu_pages)[i].draw(scale); }
template <int I> static bool game_tab(float scale) { return game_tab_draw(I, scale); }
static bool (*const g_game_tab_fns[16])(float) = {game_tab<0>, game_tab<1>, game_tab<2>, game_tab<3>, game_tab<4>,
    game_tab<5>, game_tab<6>, game_tab<7>, game_tab<8>, game_tab<9>, game_tab<10>, game_tab<11>, game_tab<12>,
    game_tab<13>, game_tab<14>, game_tab<15>};

static void quit_to_start(void) { rt_request_reboot(RT_REBOOT_TITLE); }

// The game thread (hle/swkbd.c): the name dialog, re-presenting the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    menu_keyboard_open(initial, max_len);
    while (menu_keyboard_state() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    return menu_keyboard_close(out, cap);
}

// Reboot (runtime/main.c): the game is gone - menu and name dialog close
RT_ON_RESET(rt_menu_reset) { menu_reset(); }

// ------------------------------------------------------------------ registration (runtime/main.c, once)
extern "C" void rt_ui_register(void) {
    g_root = rt_game_dir();
    g_keep = !g_settings.game_dir[0] ? KEEP_APP : g_settings.game_in_place ? KEEP_IN_PLACE : KEEP_FOLDER;
    if (g_keep == KEEP_FOLDER) {  // (the parent of "<title> game")
        std::string d = g_settings.game_dir;
        g_keep_dir = d.substr(0, d.find_last_of("/\\"));
    }
    if (const char *k = getenv("NXRT_GAME_KEEP")) {  // (test) app | inplace | folder:<dir>: the "Keep the game" choice
        g_keep = !strcmp(k, "inplace") ? KEEP_IN_PLACE : !strncmp(k, "folder:", 7) ? KEEP_FOLDER : KEEP_APP;
        if (g_keep == KEEP_FOLDER) g_keep_dir = k + 7;
    }
    static const NxUiTab video = {"Video", 0, 10, false, false, tab_display};
    static const NxUiTab audio = {"Audio", 1, 20, false, false, tab_audio};
    static const NxUiTab mods = {"Mods", 11, 35, false, true, tab_mods_page};  // (LayeredFS: runtime/mods.c)
    nxui_add_tab(&video);
    nxui_add_tab(&audio);
    nxui_add_tab(&mods);
    // the game's tab: its pages (everything game-specific; system tabs stay the same in every game)
    nxui_set_game_tab(nx_game.info.menu_name ? nx_game.info.menu_name : nx_game.info.game_name);
    for (int i = 0; i < NX_HOOK(menu_npages) && i < 16; i++) {
        const NxMenuTab *t = &NX_HOOK(menu_pages)[i];
        g_game_tabs[i] = (NxUiTab){t->name, t->id, i, t->needs_game, t->plain, g_game_tab_fns[i]};
        nxui_add_game_page(&g_game_tabs[i]);
    }
    nxui_add_rows(NXUI_ROWS_CONTROLS, language_rows);
    nxui_add_rows(NXUI_ROWS_CONTROLS, livesplit_rows);
    for (int i = 0; i < NX_HOOK(noverlays); i++)
        if (NX_HOOK(overlays)[i].wanted && NX_HOOK(overlays)[i].draw)
            nxui_add_overlay(NX_HOOK(overlays)[i].wanted, NX_HOOK(overlays)[i].draw);
    if (NX_HOOK(overlay_wanted) && NX_HOOK(overlay_draw)) nxui_add_overlay(NX_HOOK(overlay_wanted), NX_HOOK(overlay_draw));
    if (NX_HOOK(menu_opened)) nxui_on_menu_open(NX_HOOK(menu_opened));
    nxui_set_quit_to_start(quit_to_start);

    g_src_desc = std::string("The folder with your own game folders (exefs/romfs), or its .nsp / .xci with your prod.keys beside it, of ") + nx_game.info.game_name +
                 " for Nintendo Switch, version " + nx_game.info.game_version + ". The game isn't included with " +
                 nx_game.info.title + ".";
    g_setup_desc = std::string("Copies the game files ") + nx_game.info.title + " needs and recompiles the game "
                   "from them. This only needs to be done once and may take a while. Your own files aren't changed.";
    static std::string ready = std::string("Nintendo Switch  \xc2\xb7  v") + nx_game.info.game_version;
    NxUiGameFiles g = {};
    g.status = files_status, g.copied = files_copied, g.busy = files_busy, g.progress = files_progress;
    g.recompiling = files_recompiling, g.job = files_job, g.set_job = files_set_job, g.pick = files_pick;
    g.picked = files_picked, g.start = files_start, g.size_gb = files_size_gb, g.remove = files_remove;
    g.background = load_background, g.prelaunch = prelaunch_tab, g.files_deleted = files_deleted;
    g.onedrive_warn = od_warn, g.onedrive_keep = od_keep;
    g.setup_rows = setup_rows;
    g.source_desc = g_src_desc.c_str(), g.setup_desc = g_setup_desc.c_str(), g.ready_note = ready.c_str();
    g.test_fail = set_failure, g.test_extract = files_test_extract;
    nxui_set_game_files(&g);
    NxUiProfiles p = {NX_HOOK(profiles_list), NX_HOOK(profile_get), NX_HOOK(profile_set), NX_HOOK(profile_label)};
    nxui_set_profiles(&p);
    NxUiFirstPlay q = {aspect_pending, aspect_prompt, aspect_reset};
    nxui_set_first_play(&q);
    NxUiStartInfo si = {nx_game.info.headline, nx_game.info.releases_api, nx_game.info.release_page,
                        nxui_shader_prep_busy, nxui_shader_prep_progress, nxui_shader_prep_start, nxui_shader_prep_skip};
    nxui_set_start_info(&si);
}
