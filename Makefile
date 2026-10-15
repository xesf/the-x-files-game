# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf and contributors
# Build with: make            (game + tools for the host)
#             make DEBUG=1    (no optimization, sanitizers)

CC      ?= cc
BUILD   ?= build
CFLAGS  ?= -std=c99 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -Isrc -Isrc/core -Itools/xftool -Itests -D_FILE_OFFSET_BITS=64 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
ifdef DEBUG
CFLAGS  += -O0 -g -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
else
CFLAGS  += -O2 -g
endif

UNAME := $(shell uname -s)
# The platform backend: macos, linux or windows (cross-build with
# make PLATFORM=windows CC=x86_64-w64-mingw32-gcc).
ifeq ($(UNAME),Darwin)
PLATFORM ?= macos
else
PLATFORM ?= linux
endif

CORE_SRC := $(wildcard src/core/*.c) $(wildcard src/media/*.c) $(wildcard src/data/*.c) $(wildcard src/audio/*.c) $(wildcard src/gfx/*.c) $(wildcard src/vc/*.c)
CORE_OBJ := $(CORE_SRC:%.c=$(BUILD)/%.o)

EXE :=
ifeq ($(PLATFORM),macos)
PLAT_SRC := $(wildcard src/platform/macos/*.m)
PLAT_OBJ := $(PLAT_SRC:%.m=$(BUILD)/%.o)
PLAT_LIBS := -framework Cocoa -framework AudioToolbox -framework CoreAudio
endif
ifeq ($(PLATFORM),linux)
PLAT_SRC := $(wildcard src/platform/linux/*.c)
PLAT_OBJ := $(PLAT_SRC:%.c=$(BUILD)/%.o)
PLAT_LIBS := -lX11 -lasound -lpthread
endif
ifeq ($(PLATFORM),windows)
PLAT_SRC := $(wildcard src/platform/windows/*.c)
PLAT_OBJ := $(PLAT_SRC:%.c=$(BUILD)/%.o)
PLAT_LIBS := -lgdi32 -lwinmm -mwindows
EXE := .exe
endif

GAME_SRC := $(wildcard src/game/*.c) src/main.c
GAME_OBJ := $(GAME_SRC:%.c=$(BUILD)/%.o)

TOOL_SRC := $(wildcard tools/xftool/*.c)
TOOL_OBJ := $(TOOL_SRC:%.c=$(BUILD)/%.o)

all: $(BUILD)/xfiles$(EXE) $(if $(TOOL_SRC),$(BUILD)/xftool$(EXE))

$(BUILD)/xfiles$(EXE): $(GAME_OBJ) $(CORE_OBJ) $(PLAT_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(PLAT_LIBS) -lm

$(BUILD)/xftool$(EXE): $(TOOL_OBJ) $(CORE_OBJ) $(BUILD)/src/game/game.o $(BUILD)/src/game/script.o
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: %.m
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fobjc-arc -MMD -MP -c -o $@ $<

TEST_SRC := $(wildcard tests/*.c)
TEST_OBJ := $(TEST_SRC:%.c=$(BUILD)/%.o)

$(BUILD)/xftest$(EXE): $(TEST_OBJ) $(CORE_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

test: $(BUILD)/xftest$(EXE)
	$(BUILD)/xftest$(EXE)

# Run against a game install. Override on the command line, for example:
#   make run GAME_DIR=/path/to/game VIEW=1236 DISC=2
# VIEW=0 starts a new game from the title object. MEDIA_DIR is searched
# for clips missing from GAME_DIR: the DVD build streams its video from
# the disc, so the CD install provides the QuickTime clips.
GAME_DIR ?= ../Installations/x-files-en-dvd
MEDIA_DIR ?= ../Installations/x-files-en
VIEW     ?= 0
DISC     ?= 1
MOVIE    ?= ../Installations/x-files-en/xv/19650.xmv

run: $(BUILD)/xfiles
	$(BUILD)/xfiles --explore "$(GAME_DIR)" $(VIEW) $(DISC) --media "$(MEDIA_DIR)"

# Run the story scripts in tests/play against GAME_DIR, without a window.
playtest: $(BUILD)/xftool
	@for f in tests/play/*.txt; do $(BUILD)/xftool play "$(GAME_DIR)" $$f 2>/dev/null | tail -2 || exit 1; done

play: $(BUILD)/xfiles
	$(BUILD)/xfiles --play "$(MOVIE)" --data "$(GAME_DIR)"

# DOS and Windows 9x with Open Watcom (export WATCOM=<install>; the
# tools in $WATCOM/bino64 or binl64 on PATH). -j makes char signed, as
# with gcc and clang. The DOS backend builds unoptimised: at -otexan the
# Sound Blaster mixer, run from the interrupt, played silence (seen in
# the Ecstatica engine's identical code).
WATCOM  ?= $(HOME)/watcom
WCC     := wcc386
WCFLAGS := -6s -otexan -zq -za99 -j -i=src -i=src/core -i=tools/xftool -i=$(WATCOM)/h
WAT_SRC := $(CORE_SRC) $(GAME_SRC)
DOS_OBJ := $(WAT_SRC:%.c=$(BUILD)/dos/%.obj) $(BUILD)/dos/src/platform/dos/plat_dos.obj
W9X_OBJ := $(WAT_SRC:%.c=$(BUILD)/win9x/%.obj) $(BUILD)/win9x/src/platform/windows/plat_windows.obj

$(BUILD)/dos/src/platform/dos/plat_dos.obj: src/platform/dos/plat_dos.c
	@mkdir -p $(dir $@)
	$(WCC) -bt=dos $(subst -otexan,-od,$(WCFLAGS)) -fo=$@ $<

$(BUILD)/dos/%.obj: %.c
	@mkdir -p $(dir $@)
	$(WCC) -bt=dos $(WCFLAGS) -fo=$@ $<

$(BUILD)/win9x/%.obj: %.c
	@mkdir -p $(dir $@)
	$(WCC) -bt=nt $(WCFLAGS) -i=$(WATCOM)/h/nt -fo=$@ $<

$(BUILD)/dos/XFILES.EXE: $(DOS_OBJ)
	wlink system dos4g name $@ option quiet option stack=1048576 $(foreach o,$^,file $(o))

$(BUILD)/win9x/XFILES.EXE: $(W9X_OBJ)
	wlink system nt_win name $@ option quiet option stack=1048576 library winmm.lib $(foreach o,$^,file $(o))

DOS_TOOL_OBJ := $(TOOL_SRC:%.c=$(BUILD)/dos/%.obj) $(CORE_SRC:%.c=$(BUILD)/dos/%.obj) \
                $(BUILD)/dos/src/game/game.obj $(BUILD)/dos/src/game/script.obj

$(BUILD)/dos/XFTOOL.EXE: $(DOS_TOOL_OBJ)
	wlink system dos4g name $@ option quiet option stack=1048576 $(foreach o,$^,file $(o))

dos: $(BUILD)/dos/XFILES.EXE $(BUILD)/dos/XFTOOL.EXE
win9x: $(BUILD)/win9x/XFILES.EXE

# PlayStation Vita: needs VitaSDK ($VITASDK), for example inside the
# vitasdk/vitasdk Docker image:
#   docker run --rm -v "$PWD:/src" -w /src vitasdk/vitasdk make vita
vita:
	cmake -S platforms/vita -B $(BUILD)/vita -DCMAKE_BUILD_TYPE=Release -DGAME_VERSION=$(VERSION)
	cmake --build $(BUILD)/vita --parallel

# Release packaging (macOS): a universal app bundle and a zip with the
# tools and the licence. The game files are never included.
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
APP     := $(BUILD)/X-Files.app
DIST    := $(BUILD)/dist/xfiles-$(VERSION)-macos

universal:
	$(MAKE) BUILD=$(BUILD)/arm64 CC="$(CC) -arch arm64 -mmacosx-version-min=11.0" all
	$(MAKE) BUILD=$(BUILD)/x86_64 CC="$(CC) -arch x86_64 -mmacosx-version-min=11.0" all
	lipo -create -output $(BUILD)/xfiles-universal $(BUILD)/arm64/xfiles $(BUILD)/x86_64/xfiles
	lipo -create -output $(BUILD)/xftool-universal $(BUILD)/arm64/xftool $(BUILD)/x86_64/xftool

app: universal
	rm -rf "$(APP)"
	mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	cp $(BUILD)/xfiles-universal "$(APP)/Contents/MacOS/xfiles"
	sed 's/@VERSION@/$(VERSION)/g' src/platform/macos/Info.plist > "$(APP)/Contents/Info.plist"
	cp LICENSE "$(APP)/Contents/Resources/LICENSE"

dist: app
	rm -rf "$(DIST)" "$(DIST).zip"
	mkdir -p "$(DIST)"
	cp -R "$(APP)" "$(DIST)/"
	cp $(BUILD)/xftool-universal "$(DIST)/xftool"
	cp LICENSE README.md "$(DIST)/"
	cd "$(dir $(DIST))" && ditto -c -k --keepParent "$(notdir $(DIST))" "$(notdir $(DIST)).zip"

clean:
	rm -rf $(BUILD)

.PHONY: playtest all clean test run play universal app dist dos win9x vita
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
