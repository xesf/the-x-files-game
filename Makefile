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

CORE_SRC := $(wildcard src/core/*.c) $(wildcard src/media/*.c) $(wildcard src/data/*.c) $(wildcard src/audio/*.c) $(wildcard src/gfx/*.c) $(wildcard src/vc/*.c)
CORE_OBJ := $(CORE_SRC:%.c=$(BUILD)/%.o)

ifeq ($(UNAME),Darwin)
PLAT_SRC := $(wildcard src/platform/macos/*.m)
PLAT_OBJ := $(PLAT_SRC:%.m=$(BUILD)/%.o)
PLAT_LIBS := -framework Cocoa -framework AudioToolbox -framework CoreAudio
endif

GAME_SRC := $(wildcard src/game/*.c) src/main.c
GAME_OBJ := $(GAME_SRC:%.c=$(BUILD)/%.o)

TOOL_SRC := $(wildcard tools/xftool/*.c)
TOOL_OBJ := $(TOOL_SRC:%.c=$(BUILD)/%.o)

all: $(BUILD)/xfiles $(if $(TOOL_SRC),$(BUILD)/xftool)

$(BUILD)/xfiles: $(GAME_OBJ) $(CORE_OBJ) $(PLAT_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(PLAT_LIBS) -lm

$(BUILD)/xftool: $(TOOL_OBJ) $(CORE_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: %.m
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fobjc-arc -MMD -MP -c -o $@ $<

TEST_SRC := $(wildcard tests/*.c)
TEST_OBJ := $(TEST_SRC:%.c=$(BUILD)/%.o)

$(BUILD)/xftest: $(TEST_OBJ) $(CORE_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

test: $(BUILD)/xftest
	$(BUILD)/xftest

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

play: $(BUILD)/xfiles
	$(BUILD)/xfiles --play "$(MOVIE)" --data "$(GAME_DIR)"

clean:
	rm -rf $(BUILD)

.PHONY: all clean test run play
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
