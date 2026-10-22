# Kickoff prompt: bring a recomp onto nxrt

Paste the prompt below into a coding assistant (e.g. Claude Code) opened in this repository. First fill in the three
lines at the top. It works for both kinds of project:

- **Path A:** a fork of LAS Recomp (it has `runtime/`, `tools/gen.py`, a `larecomp.ini`-style settings file).
- **Path B:** a recomp with its own runtime.

For a game with **no recomp yet**, follow [NEW_GAME.md](NEW_GAME.md) instead. You can hand that guide to an assistant
too, along with your game's name, version and the path of your game files.

The assistant works out which path applies.

---

```text
I work on a Nintendo Switch static recompilation and want it to run on nxrt, the shared runtime in this repository.

- My project: <git URL or local path>   (commit: <hash or "HEAD">)
- The game: <full name>, version <x.y.z>, title id <0x0100...>
- Where my game files are (for testing; never copy or commit them): <path, or "none">

Read these first: README.md, docs/ARCHITECTURE.md, docs/PORTING.md, nxrt/runtime/nxgame.h (the game interface), games/template, and one
full example: games/sky (a multi-module game with hooks and a menu page) or games/eow (the minimal real game).

Then decide which path applies and tell me before changing anything.

PATH A - my project is a fork of LAS Recomp (same runtime/ layout, tools/gen.py, HLE_FUNC, NVN renderer):
1. Find the LAS commit it forked from (shared history, or the closest LASRecomp commit by file content). Show me how
   you found it.
2. List every file my fork changed against that base and sort each change into one of four groups:
   - nxrt: a fix or feature that helps every game (kernel, IPC, services, recompiler, shader translator, platform,
     player features).
   - games/<id>: what is my game's own (identity, module layout, patches, input logic, menu pages, texture work).
   - a new hook: shared code my fork changed with an "if this game" check. The shared code calls a new hook in
     nxgame.h, and my game provides it.
   - left out: a platform nxrt doesn't target yet, or debug code. Say why.
   Show me this table before merging.
3. Merge 3-way per file (git merge-file: base = the LAS fork commit, theirs = my fork, ours = nxrt). Renames
   (LAS -> my game's name) are not conflicts: nxrt takes names from nx_game.info. Before overwriting any nxrt file,
   check it hasn't changed since you read it.
4. Create games/<id>: Makefile (GAME_ID, GAME_EXE, GAME_SRC, GAME_MODULES), runtime/<id>_game.c with
   `const NxGame nx_game`, tools/patches.py, and the project's LICENSE. All game options go in the game's menu tab
   (menu_pages); system tabs stay system-only.

PATH B - my project has its own runtime:
1. Keep my runtime. Adopt the five layers below nx-runtime (nx-platform, nx-settings, nx-input, nx-save, nx-ui) the
   way examples/ui-host does.
2. Map what my project already has (menu, settings file, input, saves) onto those layers. Show me what each one
   replaces, and what keeps working the same for my players (settings keys, save locations).
3. Write an NxUiBackend for my renderer if it isn't Metal, wire events and pad reading through nx-input, and register my
   game's options as pages of the game's tab.
4. If parts of my runtime are generic fixes nxrt lacks, list them as proposals for nxrt. Don't merge them silently.

Either path:
- Never change behaviour for the games already on nxrt (las, sky, eow) without telling me. A shared change that
  affects them gets a hook or a default that keeps their behaviour.
- Respect licenses: nxrt is MIT. If my project is GPL, my changes don't go into nxrt itself; say so up front.
- Done means:
  1. My game and every existing game (las, sky, eow, template, examples/ui-host) build.
  2. My game's start screen renders (use a scratch NXRT_DATA_ROOT and NXRT_LAUNCHER_SHOT).
  3. If I gave a game-files path: setup builds the game code there and the game boots to its title screen. Use a
     scratch NXRT_SAVE_DIR so my real saves are untouched, and capture a frame with NXRT_WINDOW_SHOT /
     NXRT_SHOT_PATH.
  4. A new "How <game> came in" section in docs/PORTING.md and a row in the README's game table.
- Report what you verified and what you didn't, plainly. Ask me before anything outward-facing (pushing, opening PRs,
  contacting upstream).
```

---

**Tips**

- Give it the game files path if you can. Without one, the most it can check is that things build and the start screen
  renders.
- For a fork that's far from LAS, ask for the step 2 table first and review it. That table is where most of the
  judgment goes.
- It may need `NXRT_CC=/usr/bin/clang` to build the game code from a terminal outside a source checkout (see
  docs/PORTING.md, "Known gotcha").
