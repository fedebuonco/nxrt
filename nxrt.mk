# nxrt build rules, included by a game's Makefile (games/<id>/Makefile) after it sets:
#   NXRT      path to this folder (e.g. ../../nxrt)
#   GAME_ID   short name: the game code library is $(B)/$(GAME_ID)game.<dylib|so|dll>
#   GAME_EXE  the executable's name
#   GAME_SRC  the game's own runtime sources (.c, .mm), with its `const NxGame nx_game` (runtime/nxgame.h)
#   GAME_INC  (optional) extra -I flags for them
#   GAME_TOOLS (optional, default tools) the game's tools folder: its patches.py goes into gen.py (--game)
#   GAME_MODULES (optional) its code modules as name:base ..., as nx_game.info.modules (default main + sdk)
# macOS on Apple Silicon: Homebrew clang/SDL3/FFmpeg, the Metal renderer (NXRT_BACKEND, nx.mk).

GAME_TOOLS ?= tools
GAME_MODULES ?= main:0x7000000000 sdk:0x7004000000
NX_MODULES_X := $(foreach m,$(GAME_MODULES),X($(firstword $(subst :, ,$(m)))))
RT       := $(NXRT)/runtime

# the generated C (`make gen`)
GEN      ?= generated

# B: build dir. MACOS_MIN: oldest macOS to run on (default: this one). DEPS: prefix with SDL3 / FFmpeg instead of
# Homebrew's. CLANG: the compiler (macOS: Apple's /usr/bin/clang finds the SDK by itself; Homebrew's may need SDKROOT).
B         ?= build
MACOS_MIN ?=
DEPS      ?=
CLANG     ?= clang
MINFLAG  := $(if $(MACOS_MIN),-mmacosx-version-min=$(MACOS_MIN))
CC       := $(CLANG) $(MINFLAG)
CXX      := $(CLANG)++ $(MINFLAG)
GENFLAGS := -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -I$(RT)

# The game code (made by `make gen` from your own game files) is its own library next to the app (runtime/gamelib.h);
# the app itself holds none of it
GEN_SRC  := $(foreach m,$(GAME_MODULES),$(wildcard $(GEN)/$(firstword $(subst :, ,$(m)))/*.c))
GEN_OBJ  := $(patsubst %.c,$(B)/obj/%.o,$(patsubst $(GEN)/%,%,$(GEN_SRC))) $(B)/obj/gamelib_entry.o
include $(NXRT)/nx.mk  # the layers under the runtime (nx-platform ... nx-ui)

RT_SRC   := $(wildcard $(RT)/*.c) $(wildcard $(RT)/*.cpp) $(wildcard $(RT)/hle/*.c) $(wildcard $(RT)/services/*.c) \
            $(wildcard $(RT)/nvn/*.c) $(wildcard $(RT)/nvn/maxwell/*.c)
RT_HDRS  := $(wildcard $(RT)/*.h) $(wildcard $(RT)/nvn/*.h) $(wildcard $(RT)/nvn/maxwell/*.h) \
            $(NX_HDRS)
UI_SRC   := $(wildcard $(RT)/*.mm)
UI_OBJ   := $(patsubst $(RT)/%.mm,$(B)/rt/%.o,$(UI_SRC))
GAME_C   := $(filter %.c,$(GAME_SRC))
GAME_MM  := $(filter %.mm,$(GAME_SRC))
GAME_OBJ := $(patsubst %.c,$(B)/game/%.o,$(GAME_C)) $(patsubst %.mm,$(B)/game/%.o,$(GAME_MM))
GAME_HDRS := $(wildcard $(dir $(GAME_SRC))*.h)

SDL      := $(or $(DEPS),/opt/homebrew/opt/sdl3)
FFMPEG   := $(or $(DEPS),/opt/homebrew/opt/ffmpeg)
EXE      := $(B)/$(GAME_EXE)
DEPINC   := -I$(SDL)/include -I$(FFMPEG)/include
LDFLAGS  := -L$(SDL)/lib -lSDL3 -Wl,-rpath,$(SDL)/lib -L$(FFMPEG)/lib -lavformat -lavcodec -lavutil -lswscale -Wl,-rpath,$(FFMPEG)/lib \
            -framework CoreFoundation -framework Foundation -framework Metal -framework MetalFX -framework QuartzCore -framework AppKit -framework ImageIO -framework CoreGraphics -framework UniformTypeIdentifiers
# the Metal renderer (runtime/nvn/renderer.h is the seam)
BACKEND_SRC := $(wildcard $(RT)/nvn/*.m)
BACKEND_OBJ := $(patsubst $(RT)/%.m,$(B)/rt/%.o,$(BACKEND_SRC))
UIFLAGS  := -fobjc-arc
GAMELIB  := $(B)/$(GAME_ID)game.dylib
GAMELIB_LDFLAGS := -dynamiclib -undefined dynamic_lookup  # (the runtime functions it calls: the app's own)
GAMELIB_SHARED := -shared -Wl,-undefined,dynamic_lookup  # (the same, for the start screen's build: zig cc)

# GAMELIB_ID: what the game code library is built from (the generator, the game's patches, the headers it is compiled
# against, the flags). A library with another id is rebuilt by the start screen, never loaded.
GAMELIB_SRCS := $(NXRT)/tools/gen.py $(sort $(wildcard $(NXRT)/tools/recomp/*.py)) $(NXRT)/tools/lifter.py \
                $(NXRT)/tools/analyze.py $(NXRT)/tools/nso.py $(NXRT)/tools/gamelib_entry.c $(RT)/hle/hle_symbols.txt \
                $(RT)/cpu.h $(RT)/gamelib.h $(wildcard $(GAME_TOOLS)/patches.py) $(GAMELIB_EXTRA)
# (-I paths are where nxrt happens to live in this checkout, not what is built: the headers themselves are hashed above.
# Without this, the same content gives another id when nxrt moves, and every player rebuilds for nothing.)
GAMELIB_FLAGS := $(filter-out -I%,$(GENFLAGS))
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GAMELIB_FLAGS) $(GAME_MODULES)') | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)
# The library's own id adds the game's main build ID (what `make gen` wrote: $(GEN)/main/main_build_id.txt), so game
# code made from another game version is never loaded (runtime/gamelib.h gamelib_full_id).
GAMELIB_BUILD := $(shell cut -c1-16 $(GEN)/main/main_build_id.txt 2>/dev/null)
GAMELIB_FULL_ID := $(GAMELIB_ID)-$(or $(GAMELIB_BUILD),unknown)

RT_OBJ   := $(patsubst $(RT)/%.c,$(B)/rt/%.o,$(patsubst $(RT)/%.cpp,$(B)/rt/%.o,$(RT_SRC)))
RTINC    := -I$(RT) -I$(RT)/nvn $(INC_UI)
# Where a source checkout's app finds the recompiler and the game-code headers (runtime/gamebuild.c, NXRT_DEV): NXRT
# as this Makefile spelled it, so the same relative path works from the project root and from build/ (the exe's
# folder is searched too); an absolute NXRT is used as it is (platform/paths.c rt_resource), so such a build can
# rebuild its game code from wherever it is started. A packaged app finds what it ships (nxgen, zig) first.
NXRT_DEV := $(NXRT)/
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -DNXRT_DEV='"$(NXRT_DEV)"' $(RTINC) $(DEPINC)
CXXFLAGS := -O2 -g -std=c++17 -I$(IMGUI) -I$(IMGUI)/backends $(RTINC) $(DEPINC)
# the game's sources include the runtime's headers as their own folder's ("hle.h", "../settings.h" from hle/ or ui/)
GAMEINC  := -Iruntime $(RTINC) -I$(RT)/hle $(GAME_INC)

.DEFAULT_GOAL := all
all: $(EXE) $(if $(GEN_SRC),$(GAMELIB))

$(B)/obj/%.o: $(GEN)/%.c $(RT)/cpu.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@

# The id also depends on what isn't a file (the flags, the game's GAME_MODULES in its own Makefile): written to a stamp
# that changes only when the id does, so what embeds the id is rebuilt then (a port that set GAME_MODULES after its first
# build kept an app expecting the old id: "built for another version").
GAMELIB_STAMP := $(B)/gamelib.id
$(shell mkdir -p $(B) && [ "$$(cat $(GAMELIB_STAMP) 2>/dev/null)" = "$(GAMELIB_FULL_ID)" ] || echo '$(GAMELIB_FULL_ID)' > $(GAMELIB_STAMP))

$(B)/obj/gamelib_entry.o: $(NXRT)/tools/gamelib_entry.c $(GAMELIB_SRCS) $(NXRT)/nxrt.mk $(GAMELIB_STAMP) $(wildcard $(GEN)/main/main_build_id.txt)
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_FULL_ID)"' -DNX_MODULES='$(NX_MODULES_X)' -c $< -o $@
$(B)/rt/gamelib.o: $(GAMELIB_SRCS) $(NXRT)/nxrt.mk $(GAMELIB_STAMP)  # (the id comes from there: a rule change is a new id)
$(B)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'
# the start screen's build of it (runtime/gamebuild.c) uses the same flags
$(B)/rt/gamebuild.o: $(GAMELIB_SRCS) $(NXRT)/nxrt.mk $(GAMELIB_STAMP)
$(B)/rt/gamebuild.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"' -DGAMELIB_CFLAGS='"$(strip $(filter-out -I$(RT),$(GENFLAGS)))"' \
                                -DGAMELIB_SHARED='"$(GAMELIB_SHARED)"'

# The shader precompiler's list of variant shaders, as a C string (runtime/nvn/shader_prep.c). The list is the
# game's own (NX_SHADER_LIST, from the game's Makefile): the library ships none, and a game without one prepares only
# the shaders the player has already met (runtime/nvn/shader_seen.c).
NX_SHADER_LIST ?=
ifeq ($(NX_SHADER_LIST),)
$(B)/inc/shader_variants.inc: $(NXRT)/nxrt.mk
	@mkdir -p $(dir $@)
	@: > $@; echo '""' >> $@
else
$(B)/inc/shader_variants.inc: $(NX_SHADER_LIST)
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@; echo '""' >> $@
endif
$(B)/rt/nvn/shader_prep.o: $(B)/inc/shader_variants.inc
$(B)/rt/nvn/shader_prep.o: RTFLAGS += -I$(B)/inc

$(B)/rt/%.o: $(RT)/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

# SMAA (third_party/smaa, MIT): the reference shader as a C string for runtime/nvn/smaa_data.c (both backends build
# their passes from it); the non-ASCII bytes in its comments become '?', and its CRLF line ends plain LF (a CR left in
# the string literals ends them early on macOS clang)
$(B)/inc/smaa_hlsl.inc: $(NXRT)/third_party/smaa/SMAA.hlsl
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | LC_ALL=C tr '\200-\377' '?' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@
$(B)/rt/nvn/smaa_data.o: $(B)/inc/smaa_hlsl.inc
$(B)/rt/nvn/smaa_data.o: RTFLAGS += -I$(B)/inc -I$(NXRT)/third_party/smaa/Textures

$(B)/rt/%.o: $(RT)/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/rt/%.o: $(RT)/%.m $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -fobjc-arc -c $< -o $@

$(B)/rt/%.o: $(RT)/%.mm $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UIFLAGS) -c $< -o $@

# the game's own sources: as the runtime's, with its folder first on the include path
$(B)/game/%.o: %.c $(RT_HDRS) $(GAME_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) $(GAMEINC) -c $< -o $@

$(B)/game/%.o: %.mm $(RT_HDRS) $(GAME_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GAMEINC) $(UIFLAGS) -c $< -o $@

# linked to a temp file, then renamed over: a copy of the game still running keeps its (old) file - relinking in
# place gets running copies killed by macOS (code signature of the changed pages). The temp file keeps the real name,
# in a folder of its own.
IMGUI_OBJ ?= $(NX_IMGUI_OBJ)
$(EXE): $(NX_OBJ) $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(GAME_OBJ) $(IMGUI_OBJ)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(dir $@)link/$(notdir $@) $@

# its id file (<library>.id) says what it was built from without loading it (runtime/gamelib.c)
$(GAMELIB): $(GEN_OBJ) | $(EXE)
	$(CC) $(GAMELIB_LDFLAGS) -o $@.tmp $^
	mv -f $@.tmp $@
	echo $(GAMELIB_FULL_ID) > $@.id

# Python: tools/venv in the game's folder
PY := $(wildcard $(GAME_TOOLS)/venv/bin/python)

# The generator's Python environment, in the game's folder (tools/venv: not committed). `make venv` once per game;
# `make gen` and the app's own build need it.
venv:
	@if [ ! -x "$(GAME_TOOLS)/venv/bin/python" ]; then \
	    echo "Creating $(GAME_TOOLS)/venv"; \
	    python3 -m venv "$(GAME_TOOLS)/venv"; \
	fi
	@"$(GAME_TOOLS)/venv/bin/python" -m pip install -q --upgrade capstone lz4 unicorn
	@echo "$(GAME_TOOLS)/venv: capstone, lz4, unicorn ready"

# every module of the game (GAME_MODULES) from your own extracted/exefs
gen:
	$(foreach m,$(GAME_MODULES),$(PY) $(NXRT)/tools/gen.py $(firstword $(subst :, ,$(m))) extracted/exefs/$(firstword $(subst :, ,$(m))) \
	    $(lastword $(subst :, ,$(m))) $(GEN)/$(firstword $(subst :, ,$(m))) --game $(GAME_TOOLS) \
	    $(if $(filter sdk,$(firstword $(subst :, ,$(m)))),--hle $(RT)/hle/hle_symbols.txt) &&) true

gen-objs: $(GEN_OBJ)

# The lifter against this Mac's CPU (tools/test_lift.py), on every module of your own extracted/exefs
# (TEST_LIFT_COUNT distinct instructions each)
TEST_LIFT_COUNT ?= 5000
test-lift:
	$(foreach m,$(GAME_MODULES),$(PY) $(NXRT)/tools/test_lift.py extracted/exefs/$(firstword $(subst :, ,$(m))) \
	    $(lastword $(subst :, ,$(m))) --count $(TEST_LIFT_COUNT) &&) true

# Runtime + game objects only (no generated code, no link): quick compile check
runtime-objs: $(NX_OBJ) $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(GAME_OBJ)

# The library's host-side tests (tests/README.md): no game files, GPU or window needed.
test:
	@$(NXRT)/tests/run.sh

# Formatting and static analysis (tools/format.sh, tools/tidy.sh): Homebrew LLVM's clang-format/clang-tidy.
format:
	@$(NXRT)/tools/format.sh
format-fix:
	@$(NXRT)/tools/format.sh --fix
tidy:
	@$(NXRT)/tools/tidy.sh

.PHONY: all gen venv gen-objs test-lift runtime-objs test format format-fix tidy
