// A new game on nxrt: copy this folder to games/<id>/, fill in who the game is, and add hooks (runtime/nxgame.h) as
// the port needs them. With no hooks the game runs as a plain recompilation: no overlay, no extra menu pages.
#include "nxgame.h"

const NxGame nx_game = {
    .info = {
        .id = "template",                    // -> templategame.dylib / .so / .dll
        .exe = "templaterecomp",
        .title = "Template Recomp",
        .game_name = "Some Switch Game",
        .game_version = "1.0.0",
        .title_id = 0x0100000000000000ull,   // the game's program id
        .build_id = {0},                     // the first 8 bytes of exefs/main's build id (offset 0x40)
        .bundle_id = "org.example.templaterecomp",
        .data_dir = "templaterecomp",
        .ini = "template.ini",
        .version = "dev",
        .headline = "Template Recompiled",
        .romfs_probe = "",                   // a RomFS path every complete copy has ("" = the RomFS folder itself)
    },
};
