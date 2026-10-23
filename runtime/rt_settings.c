#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <ctype.h>
#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>

#include "hle.h"
#include "mods.h"
#include "nx_settings.h"
#include "nxgame.h"

const char *const g_enemy_colour_modes[N_ENEMY_COLOUR_MODES] = {"Off", "Random per seed", "Random every load"};
static const char *const k_enemy_colour_keys[N_ENEMY_COLOUR_MODES] = {"off", "seed", "load"};  // (larecomp.ini)
const char *const g_screen_filters[N_SCREEN_FILTERS] = {"Off", "Game Boy", "Game Boy Color", "Grayscale"};
static const char *const k_screen_filter_keys[N_SCREEN_FILTERS] = {"off", "gameboy", "gameboy_color", "grayscale"};  // (else: off)

Settings g_settings = {
    .aspect = ASPECT_UNLOCKED,
    .volume = 1.0f,
    .reverb = true,
    .movie_audio = true,
    .upscaler = UPSCALE_SHARP,  // (MetalFX on macOS, AMD FSR 1 on Windows)
    .frame_rate = 60,
    .hd_textures = true,
    .reshade = true,
};

// The game's own text languages: its RomFS carries these per region (regionUS/USen|USes|USfr|USpt,
// regionEU/EUen|EUde|EUes|EUfr|EUit|EUnl|EUru, regionJP/JPja, regionKR/KRko, regionTW/TWzh, regionCN/CNzh), and the
// game picks one from the system language it reads back (runtime/services/am.c), as on the console. Codes as the
// Switch reports them (nn::settings::LanguageCode).
const char *const g_language_codes[N_LANGUAGES] = {"auto", "ja", "en-US", "en-GB", "fr", "fr-CA", "de", "it", "es",
                                                   "es-419", "nl", "ru", "ko", "zh-CN", "zh-TW", "pt-BR"};
const char *const g_language_names[N_LANGUAGES] = {"Auto", "Japanese", "English (US)", "English (UK)", "French",
                                                   "French (Canada)", "German", "Italian", "Spanish",
                                                   "Spanish (Latin America)", "Dutch", "Russian", "Korean",
                                                   "Chinese (Simplified)", "Chinese (Traditional)",
                                                   "Portuguese (Brazil)"};

// A locale ("es_ES.UTF-8", "zh-Hant-TW", "pt-BR") as one of the game's language codes, NULL = none of them
static const char *language_for_locale(const char *locale) {
    if (!locale || !*locale) return NULL;
    char language[8];
    size_t n = strcspn(locale, "_- .@");
    if (!n || n >= sizeof language) return NULL;
    for (size_t i = 0; i < n; i++) language[i] = (char)tolower((unsigned char)locale[i]);
    language[n] = 0;
    if (!strcmp(language, "zh")) {
        bool traditional = false;
        const char *p = locale + n;
        while (*p == '_' || *p == '-') {
            p++;
            size_t length = strcspn(p, "_- .@");
            char part[8] = {0};
            if (length < sizeof part) {
                for (size_t i = 0; i < length; i++) part[i] = (char)tolower((unsigned char)p[i]);
                if (!strcmp(part, "hans")) return "zh-CN";
                if (!strcmp(part, "hant")) return "zh-TW";
                if (!strcmp(part, "tw") || !strcmp(part, "hk") || !strcmp(part, "mo")) traditional = true;
            }
            p += length;
        }
        return traditional ? "zh-TW" : "zh-CN";
    }
    static const struct { const char *loc, *code; } map[] = {{"ja", "ja"}, {"ko", "ko"}, {"ru", "ru"}, {"nl", "nl"},
        {"de", "de"}, {"it", "it"}, {"fr", "fr"}, {"es", "es"}, {"en", "en-US"}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(language, map[i].loc)) return map[i].code;
    return NULL;
}

// LANG_AUTO: the host's own language - Linux: $LC_ALL / $LC_MESSAGES / $LANG; else (and when those are unset) the
// system's preferred languages (SDL) - en-US when it is none of the game's.
static const char *host_language(void) {
#ifdef __linux__
    const char *e = getenv("LC_ALL");
    if (!e || !*e) e = getenv("LC_MESSAGES");
    if (!e || !*e) e = getenv("LANG");
    if (e && *e && strcmp(e, "C") && strcmp(e, "POSIX")) {
        const char *code = language_for_locale(e);
        return code ? code : "en-US";
    }
#endif
    int count = 0;
    SDL_Locale **locales = SDL_GetPreferredLocales(&count);
    const char *code = NULL;
    for (int i = 0; locales && i < count && !code; i++) {
        char locale[128];
        snprintf(locale, sizeof locale, "%s%s%s", locales[i]->language ? locales[i]->language : "",
                 locales[i]->country ? "-" : "", locales[i]->country ? locales[i]->country : "");
        code = language_for_locale(locale);
    }
    SDL_free(locales);
    return code ? code : "en-US";
}

const char *settings_language(void) {
    int i = g_settings.language;
    return i > LANG_AUTO && i < N_LANGUAGES ? g_language_codes[i] : host_language();
}

const char *settings_language_code(void) {
    const char *code = settings_language();
    const char *game = NX_HOOK(language_code) ? NX_HOOK(language_code)(code) : NULL;
    return game ? game : code;
}

// the language the game started with: changing the setting needs a restart of the game to apply
static char g_game_language[16];
RT_ON_RESET(language_reset) { g_game_language[0] = 0; }
void settings_game_started(void) {
    snprintf(g_game_language, sizeof g_game_language, "%s", settings_language_code());
    LOG("[language] game starting in %s\n", g_game_language);
}
bool settings_language_pending(void) { return g_game_language[0] && strcmp(g_game_language, settings_language_code()); }

const char *settings_language_name(void) {
    const char *c = settings_language();
    for (int i = 0; i < N_LANGUAGES; i++)
        if (!strcmp(c, g_language_codes[i])) return g_language_names[i];
    return "English";
}

// ---- the runtime's section of the settings file (nx_settings.h): display, sound, the system language
static bool g_aspect_key;
static bool rt_key(const char *key, const char *val) {
    if (!strcmp(key, "aspect")) g_settings.aspect = !strcmp(val, "original") ? ASPECT_ORIGINAL : ASPECT_UNLOCKED;
    else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
    else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
    else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
    else if (!strcmp(key, "resolution")) {
        int n = atoi(val);
        g_settings.res_mode = !strcmp(val, "dynamic") ? RES_DYNAMIC : (!strcmp(val, "75%") || !strcmp(val, "67%")) ? RES_Q75 :
                              !strcmp(val, "50%") ? RES_Q50 : !strcmp(val, "25%") ? RES_Q25 :
                              (n >= 1 && n <= 4) ? n : n > 4 ? 4 : RES_MATCH;  // (5x-8x were removed)
    }
    else if (!strcmp(key, "upscaler"))  // (metalfx / fsr1: this OS's sharp one)
        g_settings.upscaler = !strcmp(val, "bilinear") ? UPSCALE_BILINEAR : !strcmp(val, "nis") ? UPSCALE_NIS : UPSCALE_SHARP;
    else if (!strcmp(key, "frame_rate")) g_settings.frame_rate = atoi(val) == 120 ? 120 : 60;
    else if (!strcmp(key, "hd_textures")) g_settings.hd_textures = atoi(val) != 0;
    else if (!strcmp(key, "reshade")) g_settings.reshade = atoi(val) != 0;
    else if (!strcmp(key, "disabled_mods") || !strcmp(key, "mod_order")) mods_settings_load(key, val);
    else if (!strcmp(key, "antialias")) g_settings.antialias = !strcmp(val, "off") ? AA_OFF : AA_SMAA;  // ("fxaa": SMAA now)
    else if (!strcmp(key, "language")) {  // ("auto" or one of the game's codes; anything else: auto)
        g_settings.language = LANG_AUTO;
        const char *code = language_for_locale(val);
        for (int i = 1; i < N_LANGUAGES; i++)
            if (!strcmp(val, g_language_codes[i]) || (code && !strcmp(code, g_language_codes[i]))) g_settings.language = i;
    }
    else if (!strcmp(key, "game_dir")) snprintf(g_settings.game_dir, sizeof g_settings.game_dir, "%s", val);
    else if (!strcmp(key, "game_in_place")) g_settings.game_in_place = atoi(val) != 0;
    else if (!strcmp(key, "aspect_asked")) g_settings.aspect_asked = atoi(val) != 0, g_aspect_key = true;
    else return false;
    return true;
}

static void rt_loaded(bool file_found) {
    if (file_found && !g_aspect_key) g_settings.aspect_asked = true;  // (a settings file from before the question)
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
}

static void rt_write(FILE *f) {
    fprintf(f, "aspect = %s\n", g_settings.aspect == ASPECT_ORIGINAL ? "original" : "unlocked");
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "upscaler = %s\n", g_settings.upscaler == UPSCALE_METALFX ? "metalfx" : g_settings.upscaler == UPSCALE_FSR1 ? "fsr1" :
                                   g_settings.upscaler == UPSCALE_NIS ? "nis" : "bilinear");
    fprintf(f, "frame_rate = %d\nhd_textures = %d\nreshade = %d\n", g_settings.frame_rate, g_settings.hd_textures, g_settings.reshade);
    mods_settings_save(f);
    fprintf(f, "antialias = %s\n", g_settings.antialias == AA_SMAA ? "smaa" : "off");
    if (g_settings.res_mode == RES_DYNAMIC) fprintf(f, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) fprintf(f, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) fprintf(f, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) fprintf(f, "resolution = 50%%\n");
    else if (g_settings.res_mode >= 1 && g_settings.res_mode <= 8) fprintf(f, "resolution = %dx\n", g_settings.res_mode);
    else fprintf(f, "resolution = match\n");
    fprintf(f, "language = %s\n", g_language_codes[g_settings.language > LANG_AUTO && g_settings.language < N_LANGUAGES
                                                     ? g_settings.language : LANG_AUTO]);
    if (g_settings.game_dir[0]) fprintf(f, "game_dir = %s\ngame_in_place = %d\n", g_settings.game_dir, g_settings.game_in_place);
    fprintf(f, "aspect_asked = %d\n", g_settings.aspect_asked);
}

// the game's own keys (nx_game hooks settings_*)
static void game_defaults(void) { if (NX_HOOK(settings_defaults)) NX_HOOK(settings_defaults)(); }
static bool game_key(const char *key, const char *val) { return NX_HOOK(settings_key) && NX_HOOK(settings_key)(key, val); }
static void game_write(FILE *f) { if (NX_HOOK(settings_write)) NX_HOOK(settings_write)(f); }

void rt_settings_register(void) {
    static const NxSettingsSection rt = {"runtime", NULL, rt_key, rt_loaded, rt_write};
    static const NxSettingsSection game = {"game", game_defaults, game_key, NULL, game_write};
    nx_settings_register(&rt);
    nx_settings_register(&game);
}
