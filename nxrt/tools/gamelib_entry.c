// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the function
// tables of each module, found by name when the app loads the library. Everything else in it stays hidden.
// NX_MODULES: the game's modules as X(name) X(name)... (-D or a wrapper's #define: runtime/gamebuild.c, nxrt.mk).
#include "gamelib.h"

#ifndef NX_MODULES
#define NX_MODULES X(main) X(sdk)
#endif

#define X(m) extern const uint64_t m##_fn_addrs[], m##_fn_count; extern const GuestFn m##_fn_ptrs[];
NX_MODULES
#undef X

GAMELIB_EXPORT const GameLibTables *nxgame_tables(void) {
#define X(m) {#m, m##_fn_addrs, m##_fn_ptrs, &m##_fn_count},
    static const GameLibModule mods[] = {NX_MODULES};
#undef X
    static const GameLibTables t = {GAMELIB_ABI, GAMELIB_ID, sizeof mods / sizeof *mods, mods};
    return &t;
}
