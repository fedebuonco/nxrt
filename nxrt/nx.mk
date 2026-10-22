# The nx layers, for any runtime: include this from a Makefile, link $(NX_OBJ) $(NX_IMGUI_OBJ) with $(NX_LDFLAGS).
# (nxrt.mk, the nx-runtime build, includes it too.) Variables a host may set first: NXRT (this folder), B (build dir),
# CLANG, DEPS (a prefix with SDL3 instead of Homebrew's).
#
#   nx-platform  platform/   OS layer, data root and paths, app identity, crash handling hooks
#   nx-settings  settings/   the ini file: sections registered by each part
#   nx-input     input/      SDL3 keyboard / controllers, bindings, the Switch pad state (NxPad)
#   nx-save      save/       save folders per boot profile, backups, read / write listeners
#   nx-ui        ui/         menu, start screen, on-screen keyboard (Dear ImGui), drawing backends (ui/backends)
#
# Each layer compiles against its own headers and the layers below it only, so an upward dependency doesn't compile.
NXRT ?= $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
ifeq ($(.DEFAULT_GOAL),)
.DEFAULT_GOAL := all  # (not this file's first rule: the includer's `all`)
endif

ifndef PLATFORM
ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(shell uname -s) $(OS)),)
PLATFORM := windows
else ifeq ($(shell uname -s),Linux)
PLATFORM := linux
else
PLATFORM := macos
endif
endif
B     ?= build
CLANG ?= clang
ifeq ($(origin CC),default)
CC := $(CLANG)
endif
ifeq ($(origin CXX),default)
CXX := $(CLANG)++
endif
IMGUI ?= $(NXRT)/third_party/imgui

ifeq ($(PLATFORM),macos)
SDL ?= $(or $(DEPS),/opt/homebrew/opt/sdl3)
NX_DEPINC  ?= -I$(SDL)/include
NX_UIFLAGS ?= -fobjc-arc
NX_LDFLAGS ?= -L$(SDL)/lib -lSDL3 -Wl,-rpath,$(SDL)/lib -framework Foundation -framework Metal -framework QuartzCore \
              -framework AppKit -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers
else
NX_DEPINC  ?= $(if $(DEPS),-I$(DEPS)/include)
NX_UIFLAGS ?= -x c++ $(OSFLAGS)
NX_LDFLAGS ?= $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lpthread
endif

NX_PLATFORM_SRC := $(NXRT)/platform/app.c $(NXRT)/platform/paths.c $(NXRT)/platform/$(PLATFORM).c
NX_SETTINGS_SRC := $(NXRT)/settings/settings.c
NX_INPUT_SRC    := $(NXRT)/input/input.c $(NXRT)/input/bindings.c
NX_SAVE_SRC     := $(NXRT)/save/save.c
NX_UI_SRC       := $(wildcard $(NXRT)/ui/*.mm) $(if $(filter macos,$(PLATFORM)),$(NXRT)/ui/backends/metal.mm)
INC_PLATFORM := -I$(NXRT)/platform
INC_SETTINGS := $(INC_PLATFORM) -I$(NXRT)/settings
INC_INPUT    := $(INC_SETTINGS) -I$(NXRT)/input
INC_SAVE     := $(INC_SETTINGS) -I$(NXRT)/save
INC_UI       := $(INC_INPUT) -I$(NXRT)/save -I$(NXRT)/ui -I$(NXRT)/ui/backends
NX_HDRS      := $(wildcard $(NXRT)/platform/*.h $(NXRT)/settings/*.h $(NXRT)/input/*.h $(NXRT)/save/*.h $(NXRT)/ui/*.h \
                           $(NXRT)/ui/backends/*.h)
NX_OBJ   := $(patsubst $(NXRT)/%.c,$(B)/nx/%.o,$(NX_PLATFORM_SRC) $(NX_SETTINGS_SRC) $(NX_INPUT_SRC) $(NX_SAVE_SRC)) \
            $(patsubst $(NXRT)/%.mm,$(B)/nx/%.o,$(NX_UI_SRC))
NX_IMGUI_OBJ := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
                $(B)/imgui/imgui_impl_sdl3.o $(if $(filter macos,$(PLATFORM)),$(B)/imgui/imgui_impl_metal.o)

NXFLAGS = -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function $(NX_DEPINC) $(OSFLAGS)
NXCXXFLAGS = -O2 -g -std=c++17 -I$(IMGUI) -I$(IMGUI)/backends $(NX_DEPINC)

$(B)/nx/platform/%.o: $(NXRT)/platform/%.c $(NX_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(NXFLAGS) $(INC_PLATFORM) -c $< -o $@
$(B)/nx/settings/%.o: $(NXRT)/settings/%.c $(NX_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(NXFLAGS) $(INC_SETTINGS) -c $< -o $@
$(B)/nx/input/%.o: $(NXRT)/input/%.c $(NX_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(NXFLAGS) $(INC_INPUT) -c $< -o $@
$(B)/nx/save/%.o: $(NXRT)/save/%.c $(NX_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(NXFLAGS) $(INC_SAVE) -c $< -o $@
$(B)/nx/ui/%.o: $(NXRT)/ui/%.mm $(NX_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(NXCXXFLAGS) $(INC_UI) $(NX_UIFLAGS) -c $< -o $@

$(B)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(NXCXXFLAGS) -c $< -o $@
$(B)/imgui/imgui_impl_%.o: $(IMGUI)/backends/imgui_impl_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(NXCXXFLAGS) -c $< -o $@
$(B)/imgui/imgui_impl_metal.o: $(IMGUI)/backends/imgui_impl_metal.mm
	@mkdir -p $(dir $@)
	$(CXX) $(NXCXXFLAGS) -fobjc-arc -c $< -o $@

nx-objs: $(NX_OBJ) $(NX_IMGUI_OBJ)
.PHONY: nx-objs
