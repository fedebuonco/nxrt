// Start screen (nx-ui), shown before the game boots (user 2026-10-08, styled after Dusklight's launcher): PLAY (with
// ‹ Vanilla / profile › picker: the host's boot profiles, e.g. randomizer seeds), SETTINGS, QUIT, game-files status at
// the lower left. The game files come from the host (nx_ui.h NxUiGameFiles: the runtime sets them up from the player's
// own copy); PLAY stays disabled until they are ready. The background is a still the host provides.
//
// Skipped with --play, NXRT_NO_LAUNCHER=1, or any scripted test run (NXRT_INPUT): tests boot straight in.
// The in-game menu's "Title screen" comes back here without leaving the process (the runtime reboots the game).
#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <atomic>

#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner)
#include "option_rows.h"
#include "osk.h"
#include "imgui_impl_sdl3.h"

extern "C" {
#include "input_bind.h"
#include "menu.h"
#include "nx_app.h"
#include "nx_settings.h"
#include "nx_ui.h"
#include "paths.h"
#include "platform.h"
void controls_bindings_draw(float scale);  // controls_ui.mm
}

// ------------------------------------------------------------------ what the host set (nx_ui.h)
static const NxUiBackend *backend(void) {
    extern const NxUiBackend *nxui_backend(void);  // (menu.mm)
    return nxui_backend();
}
static NxUiGameFiles g_files;
static NxUiProfiles g_profiles;
static NxUiFirstPlay g_first;
static NxUiStartInfo g_info;
void nxui_set_game_files(const NxUiGameFiles *g) { g_files = *g; }
void nxui_set_profiles(const NxUiProfiles *p) { g_profiles = *p; }
void nxui_set_first_play(const NxUiFirstPlay *q) { g_first = *q; }
void nxui_set_start_info(const NxUiStartInfo *i) { g_info = *i; }
static std::string files_status(void) {
    char b[512] = "";
    if (g_files.status) g_files.status(b, sizeof b);
    return b;
}
static bool files_busy(void) { return g_files.busy && g_files.busy(); }
static void set_job(const std::string &s, bool failed = false, const std::string &hint = "") {
    if (g_files.set_job) g_files.set_job(s.c_str(), failed, hint.c_str());
}
static bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// ------------------------------------------------------------------ folder dialog (SDL calls back on any thread)
static void SDLCALL picked(void *, const char *const *files, int) {
    if (files && files[0] && g_files.pick) g_files.pick(files[0]);
}

// ------------------------------------------------------------------ background: the host's still
static uint64_t g_bg;  // ImGui texture id
#define BG_TEXID ((ImTextureID)g_bg)
static int g_bg_w, g_bg_h;
static void load_background(void) {
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (!g_files.background || !g_files.background(&rgba, &w, &h)) return;
    g_bg = backend()->texture_rgba(rgba, w, h);
    g_bg_w = w, g_bg_h = h;
    free(rgba);
}

// The release tag this build is (nx_app.version): beta_<n> until 1.0, then v<major>.<minor>.<patch>.
#define NXRT_VERSION nx_app.version

// ------------------------------------------------------------------ update check
// The start screen asks GitHub for the project's releases once per launch (a background curl, which macOS and
// Windows 10+ ship) and offers the best one if it outranks this build: any v1.0.0+ above every beta, betas by
// number; other tags (the alpha_* ones) are ignored. Offline / no curl / anything unexpected: nothing is shown.
#define RELEASES_URL g_info.releases_api
#define RELEASE_PAGE std::string(g_info.release_page)

static long long tag_rank(const std::string &t) {
    int n, a, b, c;
    char end;
    // (alphas below every beta, betas below every v1.0.0+)
    if (sscanf(t.c_str(), "alpha_%d%c", &n, &end) == 1 && n > 0 && n < (1 << 24)) return n;
    if (sscanf(t.c_str(), "beta_%d%c", &n, &end) == 1 && n > 0 && n < (1 << 24)) return (1LL << 24) + n;
    if (sscanf(t.c_str(), "v%d.%d.%d%c", &a, &b, &c, &end) == 3 && a >= 1 && b >= 0 && c >= 0 && b < 4096 && c < 4096)
        return (1LL << 48) + ((long long)a << 24) + ((long long)b << 12) + c;
    return -1;
}

// "Alpha 1" / "Beta 2" / "1.0.0" (with_word: "Version 1.0.0")
static std::string tag_label(const std::string &t, bool with_word) {
    int n;
    char end;
    if (sscanf(t.c_str(), "alpha_%d%c", &n, &end) == 1) return "Alpha " + std::to_string(n);
    if (sscanf(t.c_str(), "beta_%d%c", &n, &end) == 1) return "Beta " + std::to_string(n);
    if (!t.compare(0, 5, "test-")) return "Test build " + t.substr(5);
    std::string v = t.size() > 1 && t[0] == 'v' ? t.substr(1) : t;
    return with_word ? "Version " + v : v;
}

static std::string g_update_tag;  // a newer release's tag (g_update_lock)
static pthread_mutex_t g_update_lock = PTHREAD_MUTEX_INITIALIZER;

static void *update_thread(void *) {
    std::string body;
    if (const char *t = getenv("NXRT_UPDATE_TAGS")) {  // (test) "beta_2,v1.0.0": these releases, no network
        for (const char *p = t; *p;) {
            const char *c = strchr(p, ',');
            body += "\"tag_name\": \"" + std::string(p, c ? c - p : strlen(p)) + "\"\n";
            p = c ? c + 1 : p + strlen(p);
        }
    } else {
#ifdef _WIN32
        const char *sr = getenv("SystemRoot");
        std::string curl = std::string(sr ? sr : "C:\\Windows") + "\\System32\\curl.exe";
#else
        std::string curl = "/usr/bin/curl";
#endif
        std::string out = std::string(rt_data_root()) + "/cache/releases.json";
        mkdir((std::string(rt_data_root()) + "/cache").c_str(), 0755);
        if (!RELEASES_URL) return NULL;  // (the game has no release list)
        std::string agent = std::string(nx_app.exe) + "/" + NXRT_VERSION;
        const char *av[] = {curl.c_str(), "-fsSL", "--max-time", "15", "-H", "Accept: application/vnd.github+json",
                            "-A", agent.c_str(), RELEASES_URL, NULL};
        PlatProc p = plat_spawn(av, out.c_str(), false);
        if (!p || plat_proc_wait(p, true) != 0) { remove(out.c_str()); return NULL; }
        if (FILE *f = fopen(out.c_str(), "rb")) {
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0 && body.size() < (8u << 20)) body.append(buf, n);
            fclose(f);
        }
        remove(out.c_str());
    }
    std::string best;
    long long best_rank = tag_rank(NXRT_VERSION);
    for (size_t at = 0; (at = body.find("\"tag_name\"", at)) != std::string::npos;) {
        size_t q1 = body.find('"', body.find(':', at) + 1), q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string tag = body.substr(q1 + 1, q2 - q1 - 1);
        if (tag_rank(tag) > best_rank) best_rank = tag_rank(tag), best = tag;
        at = q2;
    }
    if (!best.empty()) fprintf(stderr, "[launcher] update available: %s (this build %s)\n", best.c_str(), NXRT_VERSION);
    pthread_mutex_lock(&g_update_lock);
    g_update_tag = best;
    pthread_mutex_unlock(&g_update_lock);
    return NULL;
}

// ------------------------------------------------------------------ boot profiles (the host's: e.g. randomizer seeds)
static std::vector<std::string> list_seeds() {
    std::vector<std::string> out;
    if (!g_profiles.list) return out;
    static char names[256][128];
    int n = g_profiles.list(names, 256);
    for (int i = 0; i < n; i++) out.push_back(names[i]);
    return out;
}
static const char *profile_now(void) { return g_profiles.get ? g_profiles.get() : ""; }
static void profile_choose(const std::string &p) { if (g_profiles.set) g_profiles.set(p.c_str()); }

// ------------------------------------------------------------------ screen
static ImFont *g_font_title, *g_font_menu, *g_font_small;
enum Page { MAIN, SETTINGS };  // SETTINGS: the in-game menu's tabs (runtime/ui/menu.mm), its Prelaunch tab for the game files
static int g_refocus = MAIN;  // back on the main page: select the item of the page we came from (controllers)

// A big menu line: gold + a marker when focused / hovered (keyboard, gamepad or mouse), grey when disabled
static bool g_menu_small;  // sub pages: buttons in the small font
// A game page's Play on the start screen (e.g. a Randomizer tab): start the game with that profile
static std::string g_play_seed;
static bool g_play_seed_req, g_play_pending;
extern "C" void launcher_play_profile(const char *seed) {
    g_play_seed = seed ? seed : "";
    g_play_seed_req = true;
}
static float g_menu_gap = 0.35f;  // space under each item, in line heights

// The first PLAY ever (settings.menu_tip_seen): a note on opening the app's menu in game (it isn't obvious),
// OK starts the game. Returns true once OK is pressed.
static bool g_tip_test;  // (NXRT_LAUNCHER_SHOT=<frame>:tip)
static bool menu_tip(float s) {
    bool go = false;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fminf(ds.x * 0.8f, 760 * s), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * s, 28 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16 * s);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.03f, 0.04f, 0.05f, 0.94f));
    bool open = ImGui::BeginPopupModal("##menutip", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (open) {
        ImGui::SetWindowFontScale(1.25f);
        // the controller's home button first (what players reach for: the PS button on a DualSense), the combo second
        static const char *const home[] = {"Home button", "Xbox button", "PlayStation button"};  // (bind_label_family)
        static const char *const pad[] = {"Minus + Plus", "View + Menu", "Create + Options"};
        ImGui::PushTextWrapPos(0);
        ImGui::Text("%s has its own menu for settings and more.", nx_app.title);
        ImGui::Spacing();
        int fam = bind_label_family() % 3;
        ImGui::Text("Open it in game with F1 or Esc, or on a controller with the %s (or %s together).", home[fam], pad[fam]);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14 * s));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 180 * s) * 0.5f);
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button("OK", ImVec2(180 * s, 44 * s)) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
            g_ui_settings.menu_tip_seen = true;
            settings_save();
            ImGui::CloseCurrentPopup();
            go = true;
        }
        ImGui::EndPopup();
    }
    return go;
}

// The game files page's example: a Finder-style window (the same on every platform) with the folder to choose -
// "Game files", opened: exefs in it opened (its files), romfs closed. All drawn here (shapes and the page's font, sized from the font: sharp at
// any scale), up to w wide, at the cursor; it takes its space with a Dummy.
enum { FOLDER_EXAMPLE_ROWS = 9 };
static float folder_example_height(void) {  // (its window, without the Dummy's gap under it)
    float fs = ImGui::GetFontSize();
    return fs * 1.45f + fs * 0.35f * 2 + FOLDER_EXAMPLE_ROWS * fs * 1.12f;
}
static void draw_folder_example(float w) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), ts = fs * 0.82f;  // (the rows' text a little under the page's)
    float rh = fs * 1.12f, bar = fs * 1.45f, pad = fs * 0.35f, r = fs * 0.45f;
    struct Item { const char *name; int depth; int kind; };  // kind: 0 file, 1 folder opened, 2 folder closed
    static const Item items[] = {{"Game files", 0, 1}, {"exefs", 1, 1}, {"main", 2, 0}, {"main.npdm", 2, 0},
                                 {"rtld", 2, 0}, {"sdk", 2, 0}, {"subsdk0", 2, 0}, {"subsdk1", 2, 0}, {"romfs", 1, 2}};
    const int n = FOLDER_EXAMPLE_ROWS;
    static_assert(sizeof items / sizeof items[0] == FOLDER_EXAMPLE_ROWS, "one row per item");
    w = fminf(w, fs * 15);
    float h = folder_example_height();
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    const ImU32 body = IM_COL32(24, 28, 27, 255), title = IM_COL32(40, 45, 43, 255), edge = IM_COL32(255, 255, 255, 38),
                text = IM_COL32(220, 216, 205, 255), dim = IM_COL32(150, 152, 144, 255),
                gold = IM_COL32(236, 194, 84, 255), gold_tab = IM_COL32(200, 160, 62, 255),
                paper = IM_COL32(226, 224, 216, 255), fold = IM_COL32(160, 158, 150, 255),
                select = IM_COL32(32, 84, 176, 255), text_sel = IM_COL32(250, 250, 255, 255);
    dl->AddRectFilled(ImVec2(a.x, a.y + fs * 0.12f), ImVec2(b.x, b.y + fs * 0.22f), IM_COL32(0, 0, 0, 70), r);  // shadow
    dl->AddRectFilled(a, b, body, r);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + bar), title, r, ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(a.x, a.y + bar), ImVec2(b.x, a.y + bar), IM_COL32(0, 0, 0, 90), 1.0f);
    dl->AddRect(a, b, edge, r, 0, 1.0f);
    // the title bar: three dots on the left, the folder it's in (a typical one) in the middle
    static const ImU32 dots[] = {IM_COL32(255, 95, 87, 255), IM_COL32(254, 188, 46, 255), IM_COL32(40, 200, 64, 255)};
    float dr = fs * 0.21f, cy = a.y + bar * 0.5f;
    for (int i = 0; i < 3; i++) dl->AddCircleFilled(ImVec2(a.x + fs * 0.65f + i * fs * 0.62f, cy), dr, dots[i]);
    const char *t = "Documents";
    ImVec2 tsz = font->CalcTextSizeA(ts, FLT_MAX, 0, t);
    dl->AddText(font, ts, ImVec2(a.x + (w - tsz.x) * 0.5f, cy - tsz.y * 0.5f), text, t);
    // the list: a disclosure triangle on folders, the folder / file glyph, the name
    float g = fs * 0.78f;  // (glyph size)
    for (int i = 0; i < n; i++) {
        const Item &it = items[i];
        float y = a.y + bar + pad + i * rh, my = y + rh * 0.5f;
        if (i == 0)  // the folder to choose: selected, as Finder shows it
            dl->AddRectFilled(ImVec2(a.x + fs * 0.25f, y), ImVec2(b.x - fs * 0.25f, y + rh), select, fs * 0.25f);
        else if (i & 1) dl->AddRectFilled(ImVec2(a.x + 1, y), ImVec2(b.x - 1, y + rh), IM_COL32(255, 255, 255, 9));
        float x = a.x + fs * 0.5f + it.depth * fs * 1.25f;
        if (it.kind) {  // ▼ opened, ▶ closed
            float u = fs * 0.2f, cx = x + u;
            ImU32 tc = i == 0 ? text_sel : dim;
            if (it.kind == 1)
                dl->AddTriangleFilled(ImVec2(cx - u, my - u * 0.6f), ImVec2(cx + u, my - u * 0.6f),
                                      ImVec2(cx, my + u * 0.75f), tc);
            else
                dl->AddTriangleFilled(ImVec2(cx - u * 0.6f, my - u), ImVec2(cx - u * 0.6f, my + u),
                                      ImVec2(cx + u * 0.75f, my), tc);
        }
        x += fs * 0.6f;
        float gx = x, gy = my - g * 0.5f;
        if (it.kind) {  // folder: a tab, then the body over it
            dl->AddRectFilled(ImVec2(gx, gy + g * 0.12f), ImVec2(gx + g * 0.46f, gy + g * 0.42f), gold_tab, g * 0.1f);
            dl->AddRectFilled(ImVec2(gx, gy + g * 0.26f), ImVec2(gx + g, gy + g * 0.9f), gold, g * 0.1f);
        } else {  // file: a page with its top right corner folded
            float l = gx + g * 0.16f, rr = gx + g * 0.84f, tp = gy + g * 0.02f, bt = gy + g * 0.98f, c = g * 0.24f;
            ImVec2 pts[] = {ImVec2(l, tp), ImVec2(rr - c, tp), ImVec2(rr, tp + c), ImVec2(rr, bt), ImVec2(l, bt)};
            dl->AddConvexPolyFilled(pts, 5, paper);
            dl->AddTriangleFilled(ImVec2(rr - c, tp), ImVec2(rr - c, tp + c), ImVec2(rr, tp + c), fold);
        }
        ImVec2 nsz = font->CalcTextSizeA(ts, FLT_MAX, 0, it.name);
        float nx = x + g + fs * 0.4f;
        dl->AddText(font, ts, ImVec2(nx, my - nsz.y * 0.5f), i == 0 ? text_sel : text, it.name);
        if (i == 0) {  // an arrow pointing back at the name, then what to do (smaller when the row is narrow)
            const char *hint = "Choose this one";
            float ax = nx + nsz.x + fs * 0.5f, aw = fs * 1.1f, gap = fs * 0.3f, room = b.x - fs * 0.6f - ax;
            float hs = ts, hw = font->CalcTextSizeA(hs, FLT_MAX, 0, hint).x;
            if (aw + gap + hw > room) hs *= fmaxf(0.7f, (room - aw - gap) / hw);
            float hx = ax + aw + gap, th = fs * 0.09f, hd = fs * 0.26f;
            dl->AddLine(ImVec2(ax + th, my), ImVec2(ax + aw, my), gold, th);
            dl->AddTriangleFilled(ImVec2(ax - th, my), ImVec2(ax + hd, my - hd), ImVec2(ax + hd, my + hd), gold);
            dl->AddText(font, hs, ImVec2(hx, my - font->CalcTextSizeA(hs, FLT_MAX, 0, hint).y * 0.5f), gold, hint);
        }
    }
    ImGui::Dummy(ImVec2(w, h));
}

// Start-screen text: a soft drop shadow, then the text with a top-to-bottom colour gradient (top == bottom: flat)
static void text_fx(ImDrawList *dl, ImFont *font, float size, ImVec2 p, ImU32 top, ImU32 bottom, const char *t,
                    float shadow) {
    if (shadow > 0)
        for (float d : {shadow, shadow * 0.5f}) dl->AddText(font, size, ImVec2(p.x + d, p.y + d), IM_COL32(4, 8, 30, 110), t);
    int v0 = dl->VtxBuffer.Size;
    dl->AddText(font, size, p, top, t);
    if (top == bottom) return;
    float y0 = p.y + size * 0.2f, y1 = p.y + size * 0.85f;
    for (int i = v0; i < dl->VtxBuffer.Size; i++) {
        float f = (dl->VtxBuffer[i].pos.y - y0) / (y1 - y0);
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        ImVec4 a = ImGui::ColorConvertU32ToFloat4(top), b = ImGui::ColorConvertU32ToFloat4(bottom);
        dl->VtxBuffer[i].col = ImGui::ColorConvertFloat4ToU32(ImVec4(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f,
                                                                     a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f));
    }
}

// A four-point sparkle (PLAY's twinkle): k = 0..1 grows and fades it
static void sparkle(ImDrawList *dl, ImVec2 c, float r, float k, float spin) {
    if (k <= 0) return;
    ImVec2 pts[8];
    for (int i = 0; i < 8; i++) {
        float a = spin + i * 3.14159265f / 4, rr = (i & 1 ? r * 0.22f : r) * k;
        pts[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
    }
    dl->AddCircleFilled(c, r * 0.55f * k, IM_COL32(255, 230, 150, (int)(60 * k)));  // soft glow
    dl->AddConvexPolyFilled(pts, 8, IM_COL32(255, 248, 215, (int)(235 * k)));
}

// split .. split_end: that part of the label in its own colour (PLAY's choice: Vanilla in vanilla cream)
static bool menu_item(const char *label, bool enabled, bool *hovered = NULL, const char *split = NULL, ImU32 col2 = 0,
                      const char *split_end = NULL) {
    ImGui::PushFont(g_menu_small ? g_font_small : g_font_menu);
    ImGui::BeginDisabled(!enabled);
    ImVec2 at = ImGui::GetCursorScreenPos();
    // (the ID from the label's fixed part: PLAY's changes with the seed shown, and a new ID loses the selection)
    ImGui::PushID(label, split ? split : label + strlen(label));
    ImVec2 sz = ImGui::CalcTextSize(label);
    // (EnableNav: otherwise an InvisibleButton can't be reached with the d-pad / arrow keys - ImGui 1.91)
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the hover band shows the selection, no box)
    bool clicked = ImGui::InvisibleButton("##b", ImVec2(sz.x + 40, sz.y + 6), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool hot = ImGui::IsItemFocused() || ImGui::IsItemHovered();
    ImGui::PopID();
    if (hovered) *hovered = hot;
    ImU32 col = !enabled ? IM_COL32(170, 175, 200, 140) : hot ? IM_COL32(255, 214, 64, 255) : IM_COL32(250, 250, 255, 255);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hot && enabled) {  // a shadow band behind the item (like the version corner's), feathered, with a warm glow
        float x0 = ImGui::GetWindowPos().x, x1 = at.x + sz.x * 0.75f, x2 = at.x + sz.x + sz.y * 3.5f;
        float f = sz.y * 0.22f, y0 = at.y - f * 0.4f, y1 = y0 + f, y3 = at.y + sz.y + f * 0.2f, y2 = y3 - f;
        ImU32 c = IM_COL32(6, 10, 34, 150), g = IM_COL32(255, 190, 80, 34), n = IM_COL32(6, 10, 34, 0), ng = IM_COL32(255, 190, 80, 0);
        for (int pass = 0; pass < 2; pass++) {  // dark band, then the glow over it
            ImU32 a = pass ? g : c, z = pass ? ng : n;
            dl->AddRectFilledMultiColor(ImVec2(x0, y1), ImVec2(x1, y2), a, a, a, a);
            dl->AddRectFilledMultiColor(ImVec2(x1, y1), ImVec2(x2, y2), a, z, z, a);
            dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1), z, z, a, a);  // (feathered top / bottom)
            dl->AddRectFilledMultiColor(ImVec2(x1, y0), ImVec2(x2, y1), z, z, z, a);
            dl->AddRectFilledMultiColor(ImVec2(x0, y2), ImVec2(x1, y3), a, a, z, z);
            dl->AddRectFilledMultiColor(ImVec2(x1, y2), ImVec2(x2, y3), a, z, z, z);
        }
    }
    ImU32 col_b = hot && enabled ? IM_COL32(255, 160, 50, 255) : col;
    if (split && enabled) {
        std::string head(label, split - label);
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), at, col, col_b, head.c_str(), sz.y * 0.05f);
        std::string mid = split_end ? std::string(split, split_end - split) : std::string(split);
        ImVec2 p2(at.x + ImGui::CalcTextSize(label, split).x, at.y);
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), p2, col2, col2, mid.c_str(), sz.y * 0.05f);
        if (split_end)
            text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + ImGui::CalcTextSize(label, split_end).x, at.y),
                    col, col, split_end, sz.y * 0.05f);
    } else
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), at, col, col_b, label, enabled ? sz.y * 0.05f : 0);
    if (g_menu_gap >= 0) ImGui::Dummy(ImVec2(0, sz.y * g_menu_gap));  // line spacing
    else ImGui::SetCursorPosY(ImGui::GetCursorPosY() + sz.y * g_menu_gap);  // (tighter than the font's line height)
    ImGui::EndDisabled();
    ImGui::PopFont();
    return clicked;
}

// Controllers, as in the in-game menu (runtime/ui/menu.mm): read straight from SDL, confirm / back as the Confirm
// button setting says (pad_nav.h: the game's layout, or the controller's convention); d-pad / left stick move.
// (ImGui's own SDL gamepad handling is switched off: per the user, it didn't work on the start screen.)

static void launcher_pad_open_all(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

// true when the event was a controller event (handled here)
static bool launcher_pad_event(const SDL_Event &e) {
    ImGuiIO &io = ImGui::GetIO();
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        SDL_OpenGamepad(e.gdevice.which);
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        ImGuiKey k = ui_pad_key(e.gbutton.which, e.gbutton.button, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        float v = e.gaxis.value / 32767.0f;
        // the left stick also works as the d-pad (ImGui moves between items with the d-pad only; the stick scrolls):
        // pressed past half way, released under 0.3
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            static bool held[4];  // left, right, up, down
            bool x = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX;
            int neg = x ? 0 : 2, pos = neg + 1;
            const ImGuiKey keys[4] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadDpadUp,
                                      ImGuiKey_GamepadDpadDown};
            for (int i : {neg, pos}) {
                float m = i == neg ? -v : v;
                bool now = held[i] ? m > 0.3f : m > 0.5f;
                if (now != held[i]) { held[i] = now; io.AddKeyEvent(keys[i], now); }
            }
        }
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, v < -0.3f, v < -0.3f ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, v > 0.3f, v > 0.3f ? v : 0);
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, v < -0.3f, v < -0.3f ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, v > 0.3f, v > 0.3f ? v : 0);
        }
        return true;
    }
    default: return false;
    }
}

// Returns when PLAY is chosen (the profile set with the game's profile_set); QUIT exits.
// Settings' first tab, on the start screen only: the extracted game files (size, delete) and the user's own Switch
// fonts (import / back to the substitute)
// Open a folder in Finder / Explorer (relative paths are in the data root, the working directory)
void *nxui_start_font(int which) { return which == 2 ? g_font_title : which == 1 ? g_font_menu : g_font_small; }

extern "C" void launcher_run(void) {
    static bool update_checked;  // (once per launch, not again after a "Title screen" reboot)
    if (!update_checked && !getenv("NXRT_NO_UPDATE_CHECK")) {
        update_checked = true;
        pthread_t t;
        if (pthread_create(&t, NULL, update_thread, NULL) == 0) pthread_detach(t);
    }
    SDL_Window *win = backend()->window();
    IMGUI_CHECKVERSION();
    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();  // the in-game menu's, when back here after a reboot
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;
    // fonts belong to this context: back here via "Title screen" (in-place reboot) the previous run's are gone with
    // its context, so load them again (a stale pointer here aborted in PushFont - user, 0.1 RC)
    g_font_small = g_font_menu = g_font_title = NULL;
    // Baloo 2 SemiBold (OFL, shipped: third_party/fonts/Baloo2) - rounded, game-like; it runs small, hence the sizes
    char baloo[1024];
    std::string own = rt_resource("Baloo2-SemiBold.ttf", NXRT_DEV "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo) ? baloo : "";
    const char *fonts[] = {getenv("NXRT_LAUNCHER_FONT") ? getenv("NXRT_LAUNCHER_FONT") : "",  // (test)
                           own.c_str(), "fonts/nintendo_udsgr_std_003.ttf", "/System/Library/Fonts/Avenir Next.ttc",
                           "/System/Library/Fonts/Helvetica.ttc", "C:/Windows/Fonts/segoeui.ttf"};
    static const ImWchar ranges[] = {0x20, 0xFF, 0x2039, 0x203A, 0x2192, 0x2192, 0x2713, 0x2713, 0x25B8, 0x25B8, 0};  // + ‹ › → ✓ ▸
    for (const char *p : fonts) {
        if (g_font_menu || !*p || !file_exists(p)) continue;
        bool b2 = p == own.c_str() || getenv("NXRT_LAUNCHER_FONT");
        g_font_small = io.Fonts->AddFontFromFileTTF(p, 30 * (b2 ? 1.75f : 1), NULL, ranges);
        g_font_menu = io.Fonts->AddFontFromFileTTF(p, 52 * (b2 ? 1.85f : 1), NULL, ranges);
        g_font_title = io.Fonts->AddFontFromFileTTF(p, 92 * (b2 ? 2.3f : 1), NULL, ranges);
    }
    if (!g_font_menu) g_font_small = g_font_menu = g_font_title = io.Fonts->AddFontDefault();
    ImGuiStyle &st = ImGui::GetStyle();
    st.Colors[ImGuiCol_Header] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.08f);
    st.Colors[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.14f);
    st.Colors[ImGuiCol_NavCursor] = ImVec4(0.95f, 0.78f, 0.35f, 1);
    st.FrameRounding = st.WindowRounding = 10;
    ImGui_ImplSDL3_InitForOther(win);
    backend()->init();
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, NULL, 0);  // (controllers: launcher_pad_event)
    launcher_pad_open_all();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    std::string status = files_status();
    if (status.empty()) load_background();
    std::vector<std::string> seeds = list_seeds();
    std::vector<std::string> choices = {"Vanilla"};
    choices.insert(choices.end(), seeds.begin(), seeds.end());
    int choice = 0;
    for (size_t i = 0; i < seeds.size(); i++)
        if (seeds[i] == profile_now()) choice = (int)i + 1;
    Page page = MAIN;  // (the game files are Settings' Prelaunch tab; PLAY stays disabled until they're ready)
    bool play = false;
    // NXRT_LAUNCHER_SHOT=<frame>[:<page>] (test): save build/launcher.png at that frame and exit; page: settings[:<tab>] /
    // askdelete / setup / fail<1-8> (all of them: Settings' Prelaunch tab, where the game files live)
    long shot_at = getenv("NXRT_LAUNCHER_SHOT") ? atol(getenv("NXRT_LAUNCHER_SHOT")) : 0, frame = 0;
    if (shot_at) {
        const char *pg = strchr(getenv("NXRT_LAUNCHER_SHOT"), ':');
        if (pg && (!strncmp(pg + 1, "settings", 8) || !strcmp(pg + 1, "askdelete"))) page = SETTINGS, menu_tabs_focus(true);
        if (pg && !strcmp(pg + 1, "setup")) page = SETTINGS, menu_tabs_focus(true), g_tab_request_ext = -1;
        if (pg && !strncmp(pg + 1, "fail", 4) && g_files.test_fail)
            page = SETTINGS, menu_tabs_focus(true), g_tab_request_ext = -1, g_files.test_fail(atoi(pg + 5));  // (failure messages)
        if (pg && !strncmp(pg + 1, "settings", 8) && pg[9] == ':') g_tab_request_ext = atoi(pg + 10);  // settings:<tab>
        if (pg && !strcmp(pg + 1, "delete")) page = SETTINGS, menu_tabs_focus(true), g_tab_request_ext = -1;
        if (pg && !strcmp(pg + 1, "tip")) g_ui_settings.menu_tip_seen = false, g_tip_test = true;
        if (pg && !strcmp(pg + 1, "aspect")) {
            if (g_first.test_reset) g_first.test_reset();
            g_ui_settings.menu_tip_seen = false, g_tip_test = true;
        }
    }

    static int runs;  // (test: =once - only the first time, so a "Title screen" reboot lands on the start screen)
    const char *ap = getenv("NXRT_LAUNCHER_AUTOPLAY");
    if (ap && status.empty() && !(runs && !strcmp(ap, "once"))) play = true;  // (test: PLAY at once)
    if (getenv("NXRT_LAUNCHER_PREPARE") && status.empty() && !runs) {  // (test) the setup's shader preparation
        g_busy = true;                                                      // alone, then PLAY with AUTOPLAY
        page = SETUP, play = false;
        pthread_t th;
        pthread_create(&th, NULL, [](void *) -> void * {
            prepare_shaders();
            set_job("Game files ready.");
            pthread_mutex_lock(&g_lock);
            g_busy = false;
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }, NULL);
        pthread_detach(th);
    }
    runs++;
    if (const char *t = getenv("NXRT_LAUNCHER_EXTRACT")) {  // (test) <folder>: set the game files up, log, exit
        if (g_files.test_extract) g_files.test_extract(t);
        char job[400] = "", hint[400];
        bool failed;
        if (g_files.job) g_files.job(job, sizeof job, hint, sizeof hint, &failed);
        fprintf(stderr, "[launcher] setup: %s -> %s\n", job, files_status().empty() ? "READY" : "NOT READY");
        exit(0);
    }
    if (const char *t = getenv("NXRT_LAUNCHER_SETUP")) {  // (test) <folder>: Set up started on the start screen
        if (g_files.pick) g_files.pick(t);
        if (g_files.start) g_files.start();
        page = SETTINGS, menu_tabs_focus(true), g_tab_request_ext = -1;
    }
    if (getenv("NXRT_LAUNCHER_DELETE") && g_files.remove) {  // (test) what DELETE GAME FILES does, logged; then the start screen
        double gb = g_files.size_gb ? g_files.size_gb() : 0;
        bool ok = g_files.remove();
        status = files_status();
        fprintf(stderr, "[launcher] delete %.1f GB: %s -> %s\n", gb, ok ? "ok" : "refused", status.c_str());
        if (!status.empty()) { g_bg = 0; page = SETTINGS, menu_tabs_focus(true), g_tab_request_ext = -1; }
    }
    if (g_onedrive < 0) {  // (once per launch: a "Title screen" reboot keeps it)
        const char *t = getenv("NXRT_TEST_ONEDRIVE");
        g_onedrive = t ? atoi(t) != 0 : plat_onedrive_synced(rt_data_root());
        if (g_onedrive) fprintf(stderr, "[launcher] the data folder is synced by OneDrive%s: %s\n",
                                g_settings.onedrive_kept ? " (kept there)" : "", rt_data_root());
    }
    // the shader preparation of every later start: in the background (first setups and updates do theirs above)
    if (status.empty() && !g_busy && !getenv("NXRT_LAUNCHER_PREPARE")) start_background_prep();
    // (test) NXRT_LAUNCHER_PLAY_AT=<s>: PLAY that many seconds in; NXRT_LAUNCHER_SKIP_AT=<s>: Skip then
    const char *play_at_env = getenv("NXRT_LAUNCHER_PLAY_AT"), *skip_at_env = getenv("NXRT_LAUNCHER_SKIP_AT");
    uint64_t loop_t0 = SDL_GetTicks(), last_frame = loop_t0, bg_gap = 0, bg_frames = 0;
    bool bg_logged = false;
    for (;;) {
        uint64_t now = SDL_GetTicks();
        if (g_bg_prep) bg_frames++, bg_gap = std::max(bg_gap, now - last_frame);
        else if (!bg_logged && bg_frames) {  // (how the start screen kept up meanwhile)
            bg_logged = true;
            fprintf(stderr, "[launcher] background shader preparation: %llu frames meanwhile, longest %llu ms\n",
                    (unsigned long long)bg_frames, (unsigned long long)bg_gap);
        }
        last_frame = now;
        if (play_at_env && status.empty() && now - loop_t0 >= (uint64_t)(atof(play_at_env) * 1000)) play = true, play_at_env = NULL;
        if (skip_at_env && g_prep_wait && now - loop_t0 >= (uint64_t)(atof(skip_at_env) * 1000)) g_prep_skip = 1, skip_at_env = NULL;
        // PLAY while that still runs: its bar (with Skip) until it's done
        if (play && g_bg_prep) {
            play = false, g_prep_wait = true;
            fprintf(stderr, "[launcher] PLAY: waiting for the shader preparation\n");
        }
        if (g_prep_wait && !g_bg_prep) {
            g_prep_wait = false, play = true;
            fprintf(stderr, "[launcher] shader preparation %s: playing\n", g_prep_skip ? "skipped" : "done");
        }
        if (play) break;
        menu_apply_window_requests(win);  // (the Video tab's Fullscreen)
        // the profile PLAY would start, kept current (the game's pages may depend on it)
        profile_choose(choice ? choices[choice] : "");
        if (g_files.files_deleted && g_files.files_deleted()) {  // (Settings > Prelaunch deleted them)
            status = files_status();
            if (!status.empty()) { g_bg = 0; bind_capture_cancel(); settings_save(); page = MAIN; }
        }
        if (g_play_seed_req) {  // the Randomizer tab's Play: that seed, then PLAY as if pressed (first-launch questions)
            g_play_seed_req = false;
            seeds = list_seeds();
            choices = {"Vanilla"};
            choices.insert(choices.end(), seeds.begin(), seeds.end());
            choice = 0;
            for (size_t i = 0; i < seeds.size(); i++)
                if (seeds[i] == g_play_seed) choice = (int)i + 1;
            bind_capture_cancel();
            settings_save();
            page = MAIN;
            g_play_pending = true;
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            input_cursor_event(&e);  // (pad / key: the cursor hides; ImGui's backend leaves it so, below)
            if (page == SETTINGS && bind_capture_event(&e)) continue;  // (waiting for a new binding: controls_ui.mm)
            if (!launcher_pad_event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) exit(0);
        }
        // extraction finished: re-check, load the background
        bool busy = files_busy();
        static bool was_busy;
        if (was_busy && !busy) {
            status = files_status();
            if (status.empty()) { load_background(); page = MAIN; }
        }
        was_busy = busy;

        {
            static const float clear_col[4] = {0.04f, 0.05f, 0.06f, 1};
            void *frame_ = backend()->frame_begin(clear_col);
            if (!frame_) { SDL_Delay(16); continue; }
            backend()->new_frame(frame_);
            ImGui_ImplSDL3_NewFrame();
            // the backend (manual mode, no gamepads of its own) clears HasGamepad every frame, and ImGui then drops the
            // controller presses launcher_pad_event fed in and turns gamepad navigation off: set it again while a
            // controller is connected
            if (SDL_HasGamepad()) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            ImGui::NewFrame();

            ImVec2 ds = io.DisplaySize;
            float s = ds.y / 720.0f;  // layout scale (fonts are built for ~2x)
            ImGui::GetIO().FontGlobalScale = s * 0.5f;
            ImDrawList *bg = ImGui::GetBackgroundDrawList();
            // an update by itself: only its bar, on the start screen's background (gray without it; a failure or
            // Cancel then shows the Game Files page)
            const bool update_only = g_auto_update && busy;
            if (g_bg && (status.empty() || update_only)) {  // cover the window, keep the aspect
                float ia = (float)g_bg_w / g_bg_h, wa = ds.x / ds.y;
                ImVec2 uv0(0, 0), uv1(1, 1);
                if (wa > ia) { float v = ia / wa; uv0.y = (1 - v) / 2; uv1.y = 1 - uv0.y; }
                else { float u = wa / ia; uv0.x = (1 - u) / 2; uv1.x = 1 - uv0.x; }
                bg->AddImage(BG_TEXID, ImVec2(0, 0), ds, uv0, uv1);
            } else if (update_only) {
                bg->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(40, 40, 40, 255));
            } else {
                bg->AddRectFilledMultiColor(ImVec2(0, 0), ds, IM_COL32(18, 34, 40, 255), IM_COL32(40, 70, 64, 255),
                                            IM_COL32(12, 20, 24, 255), IM_COL32(8, 12, 16, 255));
            }
            if (!update_only) {  // a shadow banner fading in from the left behind the menu, and one from the right behind the version
                const float stops[][2] = {{0.0f, 225}, {0.3f, 215}, {0.44f, 165}, {0.55f, 95}, {0.64f, 35}, {0.74f, 0}};
                for (int i = 0; i + 1 < (int)(sizeof stops / sizeof stops[0]); i++) {
                    ImU32 a = IM_COL32(6, 10, 34, (int)stops[i][1]), b = IM_COL32(6, 10, 34, (int)stops[i + 1][1]);
                    bg->AddRectFilledMultiColor(ImVec2(ds.x * stops[i][0], 0), ImVec2(ds.x * stops[i + 1][0], ds.y), a, b, b, a);
                }
                float y0 = ds.y - 190 * s, y1 = ds.y - 135 * s, y2 = ds.y - 40 * s, y3 = ds.y, x0r = ds.x * 0.5f;
                ImU32 c0 = IM_COL32(6, 10, 34, 0), c1 = IM_COL32(6, 10, 34, 235);
                bg->AddRectFilledMultiColor(ImVec2(x0r, y0), ImVec2(ds.x, y1), c0, c0, c1, c0);  // (feathered top)
                bg->AddRectFilledMultiColor(ImVec2(x0r, y1), ImVec2(ds.x, y2), c0, c1, c1, c0);
                bg->AddRectFilledMultiColor(ImVec2(x0r, y2), ImVec2(ds.x, y3), c0, c1, c0, c0);  // (feathered bottom)
            }

            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ds);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            ImGui::Begin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
            const bool waiting = g_prep_wait;  // (the menu waits under the bar)
            ImGui::BeginDisabled(waiting);
            ImGui::SetScrollX(0);  // (the start screen never scrolls; a long seed name or a stray nav move mustn't shift it)
            ImGui::SetScrollY(0);
            float x0 = 90 * s;
            ImGui::SetCursorPos(ImVec2(x0, 120 * s));
            ImGui::PushFont(g_font_title);
            if (page == MAIN) {  // gold to orange, shadowed (not behind the Settings / Game Files windows)
                const char *t = g_info.headline ? g_info.headline : nx_app.title;
                ImVec2 p = ImGui::GetCursorScreenPos();
                text_fx(ImGui::GetWindowDrawList(), ImGui::GetFont(), ImGui::GetFontSize(), p, IM_COL32(255, 232, 120, 255),
                        IM_COL32(255, 138, 48, 255), t, 3 * s);
                ImGui::Dummy(ImGui::CalcTextSize(t));
            }
            ImGui::PopFont();
            ImGui::SetCursorPos(ImVec2(x0, 285 * s));
            // typing in a text field / the on-screen keyboard (osk.h): B / Esc are for it, not "back" (from the frame
            // before too, so the Esc that ends the typing doesn't also leave the page)
            static bool typed_before;
            bool typing = typed_before || ui_text_busy();

            if (page == MAIN) {
                {  // the four items close together, the block anchored where QUIT always was (the gap to the status
                   // line stays)
                    ImGui::PushFont(g_font_menu);
                    float h = ImGui::CalcTextSize("A").y, sp = ImGui::GetStyle().ItemSpacing.y;
                    ImGui::PopFont();
                    auto pitch = [&](float gap) { return h + 6 + gap * h + (gap >= 0 ? 2 : 1) * sp; };
                    g_menu_gap = -0.18f;
                    ImGui::SetCursorPosY(285 * s + 3 * pitch(0.35f) - 3 * pitch(g_menu_gap));
                }
                bool hov = false;
                char label[192];
                char prefix[64] = "";
                if (choice) snprintf(prefix, sizeof prefix, "%s: ", g_profiles.label ? g_profiles.label : "Profile");
                snprintf(label, sizeof label, "PLAY    \xe2\x80\xb9 %s%s \xe2\x80\xba", prefix,
                         choices[choice].c_str());  // (choice 0 = Vanilla)
                ImGui::SetCursorPosX(x0);
                // a long seed name may run past the other items (to 80% of the window); beyond that the line shrinks
                ImGui::PushFont(g_font_menu);
                float fit = (ds.x * 0.8f - x0) / ImGui::CalcTextSize(label).x;
                ImGui::PopFont();
                if (fit < 1) ImGui::SetWindowFontScale(fit < 0.55f ? 0.55f : fit);
                // the ‹ › arrows are clickable with the mouse: they cycle Vanilla / seeds instead of starting the game
                int arrow = 0;
                {
                    ImGui::PushFont(g_font_menu);
                    ImVec2 at = ImGui::GetCursorScreenPos();
                    const char *lt = strstr(label, "\xe2\x80\xb9"), *rt = strstr(label, "\xe2\x80\xba");
                    float h = ImGui::CalcTextSize(label).y, pad = h * 0.35f;
                    float lx = at.x + ImGui::CalcTextSize(label, lt).x, rx = at.x + ImGui::CalcTextSize(label, rt).x;
                    float aw = ImGui::CalcTextSize("\xe2\x80\xb9").x;
                    ImVec2 m = ImGui::GetIO().MousePos;
                    bool in_y = m.y >= at.y - pad && m.y <= at.y + h + pad;
                    if (in_y && m.x >= lx - pad && m.x <= lx + aw + pad) arrow = -1;
                    if (in_y && m.x >= rx - pad && m.x <= rx + aw + pad) arrow = 1;
                    ImGui::PopFont();
                }
                static bool focused_once;
                if (!focused_once) { ImGui::SetKeyboardFocusHere(); focused_once = true; }
                ImVec2 play_at = ImGui::GetCursorScreenPos();
                const char *name = strstr(label, "\xe2\x80\xb9") + 4, *name_end = strstr(label, " \xe2\x80\xba");
                bool pressed = menu_item(label, status.empty(), &hov, name,
                                         choice ? IM_COL32(250, 250, 255, 255) : IM_COL32(246, 232, 176, 255),
                                         name_end);  // (the choice keeps its colour when hovered: "Rando: <seed>" white,
                                                     //  "Vanilla" vanilla cream; the arrows stay white)
                if (status.empty()) {  // PLAY twinkles now and then while the game is ready
                    ImGui::PushFont(g_font_menu);
                    float w = ImGui::CalcTextSize("PLAY").x, h = ImGui::CalcTextSize("PLAY").y;
                    ImGui::PopFont();
                    float t = (float)ImGui::GetTime(), ph = fmodf(t, 3.6f), ph2 = fmodf(t + 0.45f, 3.6f);
                    float k1 = ph < 0.9f ? sinf(ph / 0.9f * 3.14159265f) : 0, k2 = ph2 < 0.7f ? sinf(ph2 / 0.7f * 3.14159265f) : 0;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    sparkle(dl, ImVec2(play_at.x + w + h * 0.05f, play_at.y + h * 0.22f), h * 0.2f, k1, t * 0.6f);
                    sparkle(dl, ImVec2(play_at.x + w - h * 0.32f, play_at.y + h * 0.08f), h * 0.11f, k2 * 0.8f, -t * 0.8f);
                }
                ImGui::SetWindowFontScale(1.0f);
                if (pressed && arrow && ImGui::IsMouseReleased(0)) {  // (a mouse click on an arrow: cycle)
                    choice = (choice + arrow + (int)choices.size()) % (int)choices.size();
                    pressed = false;
                }
                // first PLAY ever: the aspect ratio question, then how to reach our menu, then the game
                if (g_play_pending && status.empty()) pressed = true, g_play_pending = false;
                if (pressed) {
                    if (g_first.pending && g_first.pending()) ImGui::OpenPopup("##firstplay");
                    else if (!g_ui_settings.menu_tip_seen) ImGui::OpenPopup("##menutip");
                    else play = true;
                }
                if (g_tip_test) ImGui::OpenPopup(g_first.pending && g_first.pending() ? "##firstplay" : "##menutip"), g_tip_test = false;
                if (g_first.draw && g_first.draw(s)) {
                    if (g_ui_settings.menu_tip_seen) play = true;
                    else ImGui::OpenPopup("##menutip");
                }
                if (menu_tip(s)) play = true;
                if (hov && status.empty()) {
                    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft))
                        choice = (choice + (int)choices.size() - 1) % (int)choices.size();
                    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight))
                        choice = (choice + 1) % (int)choices.size();
                    // left / right are ours while PLAY is selected: ImGui's navigation would otherwise take them too,
                    // find nothing beside PLAY and scroll the window (PLAY loses its highlight, the cycling stops)
                    ImGuiID own = ImGui::GetID("##play_lr");
                    for (ImGuiKey k : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight})
                        ImGui::SetKeyOwner(k, own);
                }
                ImGui::SetCursorPosX(x0);
                if (g_refocus == SETTINGS) ImGui::SetKeyboardFocusHere(), g_refocus = MAIN;  // (back from Settings: it
                if (menu_item("SETTINGS", true)) {                                             //  stays selected)
                    static bool opened;
                    menu_tabs_focus(!opened);  // (first time this session: Prelaunch; later: the selected tab)
                    opened = true;
                    page = SETTINGS;
                }
                // (the same items in every game: the game files are Settings' Prelaunch tab, so this is a shortcut there)
                ImGui::SetCursorPosX(x0);
                if (menu_item("SETUP GAME FILES", g_files.prelaunch != NULL)) {
                    menu_tabs_focus(true);   // (its first tab, whatever was selected last)
                    g_tab_request_ext = -1;  // (Prelaunch)
                    page = SETTINGS;
                }
                ImGui::SetCursorPosX(x0);
                if (menu_item("EXIT", true)) exit(0);
                g_menu_gap = 0.35f;
            } else if (page == SETTINGS) {
                // the in-game menu's tabs (runtime/ui/menu.mm), in a window like the in-game one; tabs that need a
                // loaded game say so
                float sc = ds.y / 1080.0f, w = fminf(ds.x * 0.9f, 1500 * sc), h = ds.y - 2 * 18 * sc;  // (all the height: a slim margin)
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ds.x,
                                                          ImGui::GetWindowPos().y + ds.y), IM_COL32(0, 0, 0, 110));
                ImVec2 wp = ImGui::GetWindowPos();
                ImGui::SetCursorScreenPos(ImVec2(wp.x + (ds.x - w) * 0.5f, wp.y + (ds.y - h) * 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.09f, 0.08f, 0.96f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * sc);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * sc, 24 * sc));
                ImGui::BeginChild("##settings", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NavFlattened);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                ImGui::PushFont(g_font_small);
                ImGui::PushFont(g_font_menu);
                ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "Settings");
                ImGui::PopFont();
                bool back;
                {  // close: a drawn X in the corner (B / Esc too)
                    float bx = 44 * sc;
                    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
                    back = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
                    ImGui::PopStyleColor();
                    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
                    ImU32 col = hot ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255);
                    float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * sc * 1.5f;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * sc);
                    dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
                    dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
                }
                bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);  // (before the pages run, like the menu)
                menu_tabs_draw(sc, g_font_small, g_font_menu, false, g_files.prelaunch);
                int cap_dev;
                bind_capture_active(&cap_dev, NULL, NULL);
                if (!popup && !typing && cap_dev < 0 && !controls_ui_took_back() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                    back = true;
                ImGui::PopFont();
                ImGui::EndChild();
                if (back) {
                    bind_capture_cancel();
                    settings_save();
                    page = MAIN, g_refocus = SETTINGS;
                    std::string cur = choices[choice];  // seeds made / renamed / deleted in the Randomizer tab
                    seeds = list_seeds();
                    choices = {"Vanilla"};
                    choices.insert(choices.end(), seeds.begin(), seeds.end());
                    choice = 0;
                    for (size_t i = 0; i < choices.size(); i++)
                        if (choices[i] == cur) choice = (int)i;
                }
            }

            // status (lower left) and version (lower right) - not under the Settings / Game Files windows
            if (page == MAIN) {
            ImGui::PushFont(g_font_small);
            ImGui::SetCursorPos(ImVec2(x0, ds.y - 118 * s));
            if (status.empty()) {
                {  // a drawn check mark (the Switch font has no ✓)
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float u = ImGui::GetTextLineHeight();
                    ImU32 g = IM_COL32(140, 230, 140, 255);
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + u * 0.1f, p.y + u * 0.55f), ImVec2(p.x + u * 0.35f, p.y + u * 0.8f), g, u * 0.12f);
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + u * 0.35f, p.y + u * 0.8f), ImVec2(p.x + u * 0.85f, p.y + u * 0.2f), g, u * 0.12f);
                    ImGui::SetCursorPosX(x0 + u * 1.3f);
                }
                ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1), "Game files ready.");
                ImGui::SetCursorPosX(x0);
                if (g_files.ready_note) ImGui::TextColored(ImVec4(0.66f, 0.76f, 0.98f, 1), "%s", g_files.ready_note);
            } else {
                ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), "!  %s", busy ? "Setting up the game files..." : status.c_str());
                ImGui::SetCursorPosX(x0);
                ImGui::TextDisabled("%s", busy ? "Set up the game files to play." : status_hint(status));
            }
            if (g_onedrive > 0 && !g_settings.onedrive_kept) {
                float w = fminf(ds.x * 0.42f, 560 * s);
                ImGui::SetCursorPos(ImVec2(ds.x - w - 60 * s, 60 * s));
                int r = onedrive_notice(s, w, status.empty());
                if (r == 1) onedrive_keep();
                else if (r == 2) choose_data_folder();
            }
            if (const char *f = getenv("NXRT_LAUNCHER_FONT")) {  // (test) which typeface this window shows
                const char *b = strrchr(f, '/');
                ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetWindowPos().x + 20 * s, ImGui::GetWindowPos().y + 16 * s),
                                                    IM_COL32(255, 255, 255, 255), b ? b + 1 : f);
            }
            {  // version, and a newer release when the update check found one (lower right). The background is
               // always the title movie's still: bright clouds here, under a shadow banner from the right
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImFont *font = ImGui::GetFont();
                float fs = ImGui::GetFontSize(), right = ImGui::GetWindowPos().x + ds.x - 60 * s;
                auto text = [&](ImVec2 p, float size, ImU32 col, const char *t, bool bold) {
                    for (float d : {2.5f, 1.5f, 0.8f}) dl->AddText(font, size, ImVec2(p.x + d * s, p.y + d * s), IM_COL32(4, 8, 30, 150), t);
                    dl->AddText(font, size, p, col, t);
                    if (bold) dl->AddText(font, size, ImVec2(p.x + 0.7f * s, p.y), col, t);
                };
                std::string ver = tag_label(NXRT_VERSION, true);
                float vs = fs * 1.4f, y = ImGui::GetWindowPos().y + ds.y - 134 * s;
                ImVec2 vsz = font->CalcTextSizeA(vs, FLT_MAX, 0, ver.c_str());
                text(ImVec2(right - vsz.x, y), vs, IM_COL32(255, 255, 255, 255), ver.c_str(), false);
                pthread_mutex_lock(&g_update_lock);
                std::string upd = g_update_tag;
                pthread_mutex_unlock(&g_update_lock);
                if (!upd.empty()) {
                    const char *arrow = font->FindGlyphNoFallback(0x2192) ? " \xe2\x86\x92" : " \xe2\x96\xb8";
                    std::string pre = "Update available!  ", link = "DOWNLOAD " + tag_label(upd, false) + arrow;
                    float wp = font->CalcTextSizeA(fs, FLT_MAX, 0, pre.c_str()).x;
                    ImVec2 lsz = font->CalcTextSizeA(fs, FLT_MAX, 0, link.c_str());
                    ImVec2 lp(right - lsz.x, y + vsz.y + 2 * s);
                    text(ImVec2(lp.x - wp, lp.y), fs, IM_COL32(200, 214, 250, 255), pre.c_str(), false);
                    ImGui::SetCursorScreenPos(lp);
                    bool click = ImGui::InvisibleButton("update", lsz);
                    bool hover = ImGui::IsItemHovered();
                    // the menu's hover colours (yellow to orange), lighter while the link itself is hovered
                    ImU32 col = hover ? IM_COL32(255, 238, 150, 255) : IM_COL32(255, 214, 64, 255);
                    ImU32 col_b = hover ? IM_COL32(255, 196, 110, 255) : IM_COL32(255, 160, 50, 255);
                    text_fx(dl, font, fs, lp, col, col_b, link.c_str(), 1.5f * s);
                    text_fx(dl, font, fs, ImVec2(lp.x + 0.7f * s, lp.y), col, col_b, link.c_str(), 0);  // (faux bold)
                    if (hover) {
                        dl->AddLine(ImVec2(lp.x, lp.y + lsz.y), ImVec2(lp.x + lsz.x, lp.y + lsz.y), col, 1.5f * s);
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }
                    if (click && g_info.release_page) SDL_OpenURL((RELEASE_PAGE + upd).c_str());
                }
            }
            ImGui::PopFont();
            }
            ImGui::EndDisabled();
            data_folder_popup(s);  // (another data folder picked: move / start fresh, then restart)
            ImGui::End();
            ImGui::PopStyleColor();
            if (waiting) draw_busy_overlay(ds, "", false, true);
            osk_draw(g_font_menu, s * 0.7f);  // (a text field activated with the controller)
            typed_before = ui_text_busy();

            ImGui::Render();
            backend()->render(frame_);
            bool shot = shot_at && ++frame == shot_at;
            backend()->frame_end(frame_, shot ? getenv("NXRT_SHOT_PATH") ? getenv("NXRT_SHOT_PATH") : "build/launcher.png" : NULL);
            if (shot) exit(0);
        }
    }
    // play: remember the choice, hand the window to the game
    profile_choose(choice ? choices[choice] : "");
    settings_save();
    backend()->shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev_ctx);
    g_bg = 0;
}
