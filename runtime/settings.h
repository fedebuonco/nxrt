// The runtime's settings (display, sound, the system language): its section of the settings file (nx_settings.h),
// edited from the menu's Video / Audio / Controls pages. A game keeps its own keys in the same file (nx_game hooks settings_*).
#pragma once
#include <stdbool.h>

enum { ASPECT_UNLOCKED = 0, ASPECT_ORIGINAL = 1 };
enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12 };
// (11 was 67% of the display: the settings file "67%" now loads as RES_Q75)
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* Windows: AMD FSR 1 */, UPSCALE_NIS = 3 /* Vulkan: NVIDIA Image Scaling */ };
#ifdef __APPLE__
#define UPSCALE_SHARP UPSCALE_METALFX  // each OS's non-bilinear upscaler (the default)
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1 };
// System language served to the game (runtime/services/am.c GetDesiredLanguage, services/hid_set.c GetLanguageCode):
// the game picks its text language from it, as it would from the Switch's own system setting. LANG_AUTO follows the
// host locale ($LC_ALL / $LC_MESSAGES / $LANG); the rest are the codes of the languages the game has text for.
enum { LANG_AUTO = 0, N_LANGUAGES = 16 };
extern const char *const g_language_codes[N_LANGUAGES];  // what the game is told: "ja", "en-US", "es", ...
extern const char *const g_language_names[N_LANGUAGES];  // for the menu ("Auto", "Japanese", ...)

typedef struct {
    int aspect;          // ASPECT_*: unlocked = fill the window (wider/taller views), original = 16:9 with bars
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (MetalFX: macOS, FSR 1: others)
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    float volume;        // master volume, 1 = 100%
    bool reverb;         // room reverb effect
    bool movie_audio;
    int language;        // LANG_AUTO or an index into g_language_codes: the game's text language
    int frame_rate;       // 60 or 120 (runtime/frame_rate.h: 120 only where the game supports it)
    bool hd_textures;     // texture packs (runtime/nvn/custom_tex.c: mods/<pack>/textures), applied live on Vulkan
    bool reshade;         // Windows: load the ReShade layer that ships with the app on the next launch
    char game_dir[1024];  // the game folder (nx-platform rt_game_dir): "" = extracted/ in the data root
    bool game_in_place;   // it is the player's own extract (exefs/, romfs/ theirs): only what the app builds goes there
    bool aspect_asked;   // the start screen's one-time "authentic 16:9 or unlocked?" question was answered
} Settings;

extern Settings g_settings;

#include "nx_settings.h"  // settings_load / settings_save
// The code served to the game as its system language: settings_language, as the game wants it (nx_game hooks
// language_code: e.g. zh-Hans for zh-CN)
const char *settings_language_code(void);
void settings_game_started(void);       // (boot) the language the game starts with
bool settings_language_pending(void);   // the language setting changed since: the game must restart for it
void rt_settings_register(void);  // the runtime's and the game's sections (main, before anything loads settings)
// The language code to serve the game ("es", "en-US", ...): the setting, or the host locale for LANG_AUTO
const char *settings_language(void);
// That code's name for the menu ("Spanish")
const char *settings_language_name(void);
extern bool g_gpu_integrated;  // an integrated GPU (output scales differently at 1x)
