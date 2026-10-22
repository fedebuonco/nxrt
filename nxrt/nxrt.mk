# nxrt build rules, included by a game's Makefile (games/<id>/Makefile) after it sets:
#   NXRT      path to this folder (e.g. ../../nxrt)
#   GAME_ID   short name: the game code library is $(B)/$(GAME_ID)game.<dylib|so|dll>
#   GAME_EXE  the executable's name
#   GAME_SRC  the game's own runtime sources (.c, .mm), with its `const NxGame nx_game` (runtime/nxgame.h)
#   GAME_INC  (optional) extra -I flags for them
#   GAME_TOOLS (optional, default tools) the game's tools folder: its patches.py goes into gen.py (--game)
#   GAME_MODULES (optional) its code modules as name:base ..., as nx_game.info.modules (default main + sdk)
# macOS: Homebrew clang/SDL3/FFmpeg, Metal renderer.
# Windows: MSYS2 UCRT64 shell, clang + Vulkan renderer.
# Linux: clang + Vulkan renderer, x86-64 or aarch64.
UNAME := $(shell uname -s)
comma := ,
ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
PLATFORM := windows
else ifeq ($(UNAME),Linux)
PLATFORM := linux
else
PLATFORM := macos
endif

GAME_TOOLS ?= tools
GAME_MODULES ?= main:0x7000000000 sdk:0x7004000000
NX_MODULES_X := $(foreach m,$(GAME_MODULES),X($(firstword $(subst :, ,$(m)))))
RT       := $(NXRT)/runtime

# Linux: ARCH (x86_64 / aarch64, default this machine's) picks the target, separately from PLATFORM. The generated code
# differs per arch (AArch64 hosts run the guest's FP/SIMD as inline asm, others use the portable C of simd.h), so a
# cross build has its own generated-<arch>/ and build/<arch>/; SYSROOT is the target's root filesystem (headers, libs).
GEN      ?= generated
ifeq ($(PLATFORM),linux)
HOSTARCH := $(patsubst arm64,aarch64,$(shell uname -m))
ARCH     ?= $(HOSTARCH)
ifneq ($(ARCH),$(HOSTARCH))
CROSS    := --target=$(ARCH)-linux-gnu $(if $(SYSROOT),--sysroot=$(SYSROOT))
GEN      := generated-$(ARCH)
B        ?= build/$(ARCH)
endif
GENSIMD  := --simd $(if $(filter aarch64,$(ARCH)),asm,c)
endif

# B: build dir. MACOS_MIN: oldest macOS to run on (default: this one). DEPS: prefix with SDL3 / FFmpeg instead of
# Homebrew's. CLANG: the compiler (macOS: Apple's /usr/bin/clang finds the SDK by itself; Homebrew's may need SDKROOT).
B         ?= build
MACOS_MIN ?=
DEPS      ?=
CLANG     ?= clang
MINFLAG  := $(if $(MACOS_MIN),-mmacosx-version-min=$(MACOS_MIN))
CC       := $(CLANG) $(MINFLAG) $(CROSS)
CXX      := $(CLANG)++ $(MINFLAG) $(CROSS)
GENFLAGS := -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -I$(RT)

# The game code (made by `make gen` from your own game files) is its own library next to the app (runtime/gamelib.h);
# the app itself holds none of it
GEN_SRC  := $(foreach m,$(GAME_MODULES),$(wildcard $(GEN)/$(firstword $(subst :, ,$(m)))/*.c))
GEN_OBJ  := $(patsubst %.c,$(B)/obj/%.o,$(patsubst $(GEN)/%,%,$(GEN_SRC))) $(B)/obj/gamelib_entry.o
include $(NXRT)/nx.mk  # the layers under the runtime (nx-platform ... nx-ui)

RT_SRC   := $(wildcard $(RT)/*.c) $(wildcard $(RT)/hle/*.c) $(wildcard $(RT)/services/*.c) \
            $(wildcard $(RT)/nvn/*.c) $(wildcard $(RT)/nvn/maxwell/*.c)
RT_HDRS  := $(wildcard $(RT)/*.h) $(wildcard $(RT)/nvn/*.h) $(wildcard $(RT)/nvn/maxwell/*.h) \
            $(wildcard $(RT)/nvn/vulkan/*.h) $(NX_HDRS)
UI_SRC   := $(wildcard $(RT)/*.mm)
UI_OBJ   := $(patsubst $(RT)/%.mm,$(B)/rt/%.o,$(UI_SRC))
GAME_C   := $(filter %.c,$(GAME_SRC))
GAME_MM  := $(filter %.mm,$(GAME_SRC))
GAME_OBJ := $(patsubst %.c,$(B)/game/%.o,$(GAME_C)) $(patsubst %.mm,$(B)/game/%.o,$(GAME_MM))
GAME_HDRS := $(wildcard $(dir $(GAME_SRC))*.h)

ifeq ($(PLATFORM),macos)
SDL      := $(or $(DEPS),/opt/homebrew/opt/sdl3)
FFMPEG   := $(or $(DEPS),/opt/homebrew/opt/ffmpeg)
EXE      := $(B)/$(GAME_EXE)
DEPINC   := -I$(SDL)/include -I$(FFMPEG)/include
LDFLAGS  := -L$(SDL)/lib -lSDL3 -Wl,-rpath,$(SDL)/lib -L$(FFMPEG)/lib -lavformat -lavcodec -lavutil -lswscale -Wl,-rpath,$(FFMPEG)/lib \
            -framework CoreFoundation -framework Foundation -framework Metal -framework MetalFX -framework QuartzCore -framework AppKit -framework ImageIO -framework CoreGraphics -framework UniformTypeIdentifiers
BACKEND_SRC := $(wildcard $(RT)/nvn/*.m)
BACKEND_OBJ := $(patsubst $(RT)/%.m,$(B)/rt/%.o,$(BACKEND_SRC))
UIFLAGS  := -fobjc-arc
GAMELIB  := $(B)/$(GAME_ID)game.dylib
GAMELIB_LDFLAGS := -dynamiclib -undefined dynamic_lookup  # (the runtime functions it calls: the app's own)
GAMELIB_SHARED := -shared -Wl,-undefined,dynamic_lookup  # (the same, for the start screen's build: zig cc)
else ifeq ($(PLATFORM),linux)
EXE      := $(B)/$(GAME_EXE)
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
ifeq ($(ARCH),x86_64)
GENARCH  ?= -march=x86-64-v3  # as on Windows: AVX2 + FMA for the lifted FP/SIMD code (checked at startup)
endif
GENFLAGS += $(GENARCH) -fPIC
OSFLAGS  := -Wno-format  # (%llx for uint64_t, which is unsigned long here: the same size)
SHADERC  ?= -lshaderc  # (Debian / Ubuntu's libshaderc.so; a shaderc built from source: -lshaderc_shared)
# -rdynamic: crash backtraces name host functions (runtime/platform/linux.c)
LDFLAGS  := -fuse-ld=lld -rdynamic $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan $(SHADERC) -lpthread -ldl -lm
BACKEND_SRC := $(wildcard $(RT)/nvn/vulkan/*.c) $(wildcard $(RT)/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst $(RT)/%.cpp,$(B)/rt/%.o,$(patsubst $(RT)/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
IMGUI_OBJ = $(NX_IMGUI_OBJ) $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++  # the .mm UI sources are plain C++ apart from __APPLE__ blocks
GAMELIB  := $(B)/$(GAME_ID)game.so
GAMELIB_LDFLAGS := -shared -fuse-ld=lld  # (the runtime functions it calls: the app's own, -rdynamic)
GAMELIB_SHARED := -shared
else
EXE      := $(B)/$(GAME_EXE).exe
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -D_CRT_RAND_S -DWIN32_LEAN_AND_MEAN -DNOMINMAX
GENARCH  ?= -march=x86-64-v3  # AVX2 + FMA for the lifted FP/SIMD code (simd.h); override for older CPUs
GENFLAGS += -D_FILE_OFFSET_BITS=64 $(GENARCH)
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan-1 -lwinmm -lole32 -lshell32 -lcomdlg32 -luuid -lshaderc_shared \
            $(if $(RELEASE),-Wl$(comma)--subsystem$(comma)windows)  # release: no console window (log -> <exe>.log)
BACKEND_SRC := $(wildcard $(RT)/nvn/vulkan/*.c) $(wildcard $(RT)/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst $(RT)/%.cpp,$(B)/rt/%.o,$(patsubst $(RT)/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
BACKEND_OBJ += $(B)/rt/platform/windows_rc.o  # the .exe's icon (Explorer, taskbar)
WINDRES  ?= windres
IMGUI_OBJ = $(NX_IMGUI_OBJ) $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++ $(OSFLAGS)  # the .mm UI sources are plain C++ apart from __APPLE__ blocks
GAMELIB  := $(B)/$(GAME_ID)game.dll
# the runtime functions it calls: exported by the .exe, linked through its import library
LDFLAGS  += -Wl,--export-all-symbols -Wl,--out-implib,$(B)/$(GAME_EXE).dll.a
GAMELIB_LDFLAGS := -shared -fuse-ld=lld $(B)/$(GAME_EXE).dll.a
GAMELIB_SHARED := -shared  # (+ the import library: runtime/gamebuild.c)
endif

# GAMELIB_ID: what the game code library is built from (the generator, the game's patches, the headers it is compiled
# against, the flags). A library with another id is rebuilt by the start screen, never loaded.
GAMELIB_SRCS := $(NXRT)/tools/gen.py $(NXRT)/tools/lifter.py $(NXRT)/tools/analyze.py $(NXRT)/tools/nso.py \
                $(NXRT)/tools/passes.py $(NXRT)/tools/simd_c.py $(NXRT)/tools/gamelib_entry.c $(RT)/hle/hle_symbols.txt \
                $(RT)/cpu.h $(RT)/simd.h $(RT)/simd_fp.h $(RT)/gamelib.h $(wildcard $(GAME_TOOLS)/patches.py) $(GAMELIB_EXTRA)
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS) $(GAME_MODULES)') | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)

RT_OBJ   := $(patsubst $(RT)/%.c,$(B)/rt/%.o,$(RT_SRC))
RTINC    := -I$(RT) -I$(RT)/nvn $(INC_UI)
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function $(RTINC) $(DEPINC) $(OSFLAGS)
CXXFLAGS := -O2 -g -std=c++17 -I$(IMGUI) -I$(IMGUI)/backends $(RTINC) $(DEPINC)
# the game's sources include the runtime's headers as their own folder's ("hle.h", "../settings.h" from hle/ or ui/)
GAMEINC  := -Iruntime $(RTINC) -I$(RT)/hle $(GAME_INC)

.DEFAULT_GOAL := all
all: $(EXE) $(if $(GEN_SRC),$(GAMELIB))

$(B)/obj/%.o: $(GEN)/%.c $(RT)/cpu.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@

$(B)/obj/gamelib_entry.o: $(NXRT)/tools/gamelib_entry.c $(GAMELIB_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)"' -DNX_MODULES='$(NX_MODULES_X)' -c $< -o $@
$(B)/rt/gamelib.o: $(GAMELIB_SRCS)
$(B)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'
# the start screen's build of it (runtime/gamebuild.c) uses the same flags
$(B)/rt/gamebuild.o: $(GAMELIB_SRCS) $(NXRT)/nxrt.mk
$(B)/rt/gamebuild.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"' -DGAMELIB_CFLAGS='"$(strip $(filter-out -I$(RT),$(GENFLAGS)))"' \
                                -DGAMELIB_SHARED='"$(GAMELIB_SHARED)"'

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
	$(CXX) $(CXXFLAGS) $(OSFLAGS) -c $< -o $@

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
# place gets running copies killed by macOS (code signature of the changed pages). The temp file keeps the real name
# (in a folder of its own): on Windows the import library records the name the game code library loads it by.
IMGUI_OBJ ?= $(NX_IMGUI_OBJ)
$(EXE): $(NX_OBJ) $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(GAME_OBJ) $(IMGUI_OBJ)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(dir $@)link/$(notdir $@) $@

# its id file (<library>.id) says what it was built from without loading it (runtime/gamelib.c)
$(GAMELIB): $(GEN_OBJ) | $(EXE)
	$(CC) $(GAMELIB_LDFLAGS) -o $@.tmp $^
	mv -f $@.tmp $@
	echo $(GAMELIB_ID) > $@.id

# Python: tools/venv in the game's folder (macOS, Linux: bin/python, Windows: Scripts/python.exe)
PY := $(firstword $(wildcard $(GAME_TOOLS)/venv/bin/python) $(wildcard $(GAME_TOOLS)/venv/Scripts/python.exe))

# The generator's Python environment, in the game's folder (tools/venv: not committed). `make venv` once per game;
# `make gen` and the app's own build need it. Windows: `py -3 -m venv` if `python3` isn't on PATH.
venv:
	@if [ ! -x "$(GAME_TOOLS)/venv/bin/python" ] && [ ! -x "$(GAME_TOOLS)/venv/Scripts/python.exe" ]; then \
	    echo "Creating $(GAME_TOOLS)/venv"; \
	    python3 -m venv "$(GAME_TOOLS)/venv"; \
	fi
	@"$(GAME_TOOLS)/venv/bin/python" -m pip install -q --upgrade capstone lz4 unicorn 2>/dev/null || \
	    "$(GAME_TOOLS)/venv/Scripts/python.exe" -m pip install -q --upgrade capstone lz4 unicorn
	@echo "$(GAME_TOOLS)/venv: capstone, lz4, unicorn ready"

# every module of the game (GAME_MODULES) from your own extracted/exefs
gen:
	$(foreach m,$(GAME_MODULES),$(PY) $(NXRT)/tools/gen.py $(firstword $(subst :, ,$(m))) extracted/exefs/$(firstword $(subst :, ,$(m))) \
	    $(lastword $(subst :, ,$(m))) $(GEN)/$(firstword $(subst :, ,$(m))) --game $(GAME_TOOLS) \
	    $(if $(filter sdk,$(firstword $(subst :, ,$(m)))),--hle $(RT)/hle/hle_symbols.txt) $(GENSIMD) &&) true

gen-objs: $(GEN_OBJ)

# Runtime + game objects only (no generated code, no link): quick compile check
runtime-objs: $(NX_OBJ) $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(GAME_OBJ)

$(B)/rt/platform/windows_rc.o: $(NXRT)/platform/windows.rc $(WINDOWS_ICON)
	@mkdir -p $(dir $@)
	$(WINDRES) -I $(dir $(WINDOWS_ICON)) -O coff $< -o $@

.PHONY: all gen venv gen-objs runtime-objs
