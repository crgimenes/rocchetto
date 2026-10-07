# rocchetto: a POSIX shell whose utilities are Filo programs, and the
# screens and apps those programs can be. Hosts: posix, wasm (the browser),
# esp32 (the Cardputer); a host in another repository links libroc.a.
#
# This Makefile builds rocchetto on its own, and is also included by a
# build that adds a layer over it (a board: its screens, its commands, its
# own C): such a build sets ROC to where this repository is and the hooks
# below, then includes this file. Paths of this repository are $(ROC)/...;
# what a build makes goes to build/ where make runs.
ROC ?= .
CC ?= cc
CLANG ?= clang
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy
# lld for the wasm link, when the clang above has none of its own on PATH
LLD ?=

WARN = -Wall -Wextra -Werror -Wshadow -Wconversion -Wdouble-promotion -Wundef
# Filo, the terminal a Filo program runs on (filo-term), Core War, and the
# editor are their own repositories, beside this one.
FILO ?= $(ROC)/../clang_filo
FILOSRC = $(FILO)/filo.c $(FILO)/filo_math.c $(FILO)/filo_strings.c $(FILO)/filo_nolibc.c
FILO_TERM ?= $(ROC)/../filo-term
TERM_SRC = $(FILO_TERM)/src
CW ?= $(ROC)/../corewar
CW_SRC = $(CW)/src
EDT ?= $(ROC)/../edt
# Asyncify (binaryen) lets a script wait for the site's files in the
# browser: the core's stack unwinds while the page fetches.
WASM_OPT ?= wasm-opt
# the layer's own headers (-I...), when a build adds one
EXTRA_INC ?=
# glibc and musl hide POSIX (cfmakeraw, sigaction, tm_gmtoff) under a strict
# -std; the BSDs and macOS ignore the macro.
INC = -D_DEFAULT_SOURCE -I$(ROC)/src -I$(TERM_SRC) -I$(CW_SRC) -I$(FILO) -Ibuild $(EXTRA_INC)
CFLAGS = -std=c23 -O2 $(WARN) $(APPFLAGS) $(INC)

# The version is whatever git says, written to a header so that a new tag
# actually rebuilds: make compares files, not command lines. Outside a
# checkout it degrades to "unknown" instead of failing the build.
ROC_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)
# What a build contains. Each switch drives both the compile flag the code
# reads and the files the link sees: turning one off with -D alone would
# still link the app in, so they live here together.
APPS_SCREENS ?= 1
APPS_EDIT ?= 1
APPS_COREWAR ?= 1
APPS_TOOLS ?= 1
APPFLAGS = -DROC_APP_SCREENS=$(APPS_SCREENS) -DROC_APP_EDIT=$(APPS_EDIT) -DROC_APP_COREWAR=$(APPS_COREWAR) \
	-DROC_APP_TOOLS=$(APPS_TOOLS) -DFBC_HOST_NUMBERS
APP_SCREENS = $(ROC)/src/screen.c $(TERM_SRC)/paint.c $(TERM_SRC)/field.c
APP_EDIT = $(ROC)/src/edit.c $(TERM_SRC)/tbx.c
APP_COREWAR = $(ROC)/src/corewar.c $(CW_SRC)/mars.c $(CW_SRC)/arena.c
# filo's tools, clang_filo's listing and decompiler: their numbers are the
# runtime's (FBC_HOST_NUMBERS), since the shell has no C library
APP_TOOLS = $(addprefix $(ROC)/src/,filotools.c debug.c diff.c) $(FILO)/fbc_dump.c $(FILO)/fbc_decompile.c
# the formatter, which the tools and the editor (tb-format) both take
APP_FMT = $(FILO)/filo_fmt.c
APPSRC = $(if $(filter 1,$(APPS_SCREENS)),$(APP_SCREENS)) \
	$(if $(filter 1,$(APPS_EDIT)),$(APP_EDIT)) \
	$(if $(filter 1,$(APPS_COREWAR)),$(APP_COREWAR)) \
	$(if $(filter 1,$(APPS_TOOLS)),$(APP_TOOLS)) \
	$(if $(filter 1,$(APPS_EDIT) $(APPS_TOOLS)),$(APP_FMT))

# The host's own builtins past rocchetto's (filo_extend) and the layer over
# the shell (roc_layer_spec, src/roc.h): none here; a layer names its own
# file, and in LAYER_SRC the rest of its sources. A layer is screens: the
# builds without them (small, esp32) take none.
EXTEND_SRC ?= $(ROC)/host/extend_none.c
LAYER_SRC ?=
BASE = $(EXTEND_SRC) $(addprefix $(ROC)/src/,roc.c cmds.c script.c script_data.c fsmeta.c fileh.c sh.c tree.c vfs.c ufs.c home.c ed.c printf.c regex.c) \
	$(addprefix $(TERM_SRC)/,keyin.c canvas.c md.c hl.c pager.c term.c utf8.c tbuf.c)
CORE = $(BASE) $(APPSRC) $(LAYER_SRC)
HDRS = build/version.h $(FILO)/filo.h $(wildcard $(ROC)/src/*.h) $(CW_SRC)/mars.h $(CW_SRC)/arena.h \
	$(FILO)/fbc_dump.h $(FILO)/fbc_decompile.h $(FILO)/filo_fmt.h $(wildcard $(TERM_SRC)/*.h) \
	$(EXTRA_HDRS)

# The tree goes into the binary with C23's #embed where the compiler has it
# (clang 19, GCC 15), and as plain byte arrays where it does not.
HASH := \#
EMBED ?= $(shell printf '$(HASH)embed "%s"\n' '$(abspath $(ROC)/Makefile)' | \
	$(CC) -std=c23 -E -x c - >/dev/null 2>&1 || echo --bytes)

# The tree /lib/roc is: this repository's lib/ (the help, the Filo API) and
# whatever roots a layer adds (its screens). The commands of /bin, as
# source: compiled into bin/NAME and carried that way, never as the .filo.
TREE_ROOTS ?= $(ROC)/lib
COMMAND_ROOTS ?= $(ROC)/commands
SCREENS = $(shell find $(TREE_ROOTS) -type f 2>/dev/null | sort)
COMMANDS = $(shell find $(COMMAND_ROOTS) -type f 2>/dev/null | sort)
GEN = build/screens_data.c
GEN_SRC = build/screens_src.c
UNITS = build/units/.built

# The tree the shell carries is the screens plus the unit each one compiles
# to (docs/bytecode.md in the Filo repository). The units are compiled by
# the shell itself, linked against the sources alone and with every app in,
# so any build can carry them: one without an app refuses that app's screens
# at load, as it would refuse their source.
$(GEN_SRC): $(SCREENS) $(COMMANDS) $(ROC)/tools/embed.sh
	@mkdir -p build
	sh $(ROC)/tools/embed.sh $(EMBED) $(TREE_ROOTS) $(COMMAND_ROOTS) > $@

# The units are compiled with every app this build can have, so any build
# can carry them: one without an app refuses that app's screens at load.
ALLAPPS = -DROC_APP_SCREENS=1 -DROC_APP_EDIT=1 -DROC_APP_COREWAR=1 -DROC_APP_TOOLS=1 -DFBC_HOST_NUMBERS
ALLSRC = $(BASE) $(APP_SCREENS) $(APP_EDIT) $(APP_COREWAR) $(LAYER_SRC) $(APP_TOOLS) $(APP_FMT) $(FILOSRC)
build/mkunits: $(ALLSRC) $(GEN_SRC) $(HDRS) $(ROC)/tools/mkunits.c
	$(CC) -std=c23 -O2 $(WARN) $(ALLAPPS) $(INC) -o $@ $(ALLSRC) $(GEN_SRC) $(ROC)/tools/mkunits.c

$(UNITS): build/mkunits
	@rm -rf build/units && mkdir -p build/units
	./build/mkunits --strip-screens build/units
	@touch $@

# The apps carried from their own repositories, one bundle each, in /bin
# under the bundle's name (edt.fbb is /bin/edt): the same files the desktop
# programs embed, byte for byte. A layer adds its own (APP_FBBS +=).
APP_FBBS ?= $(EDT)/edt.fbb
# Core War's classics in /lib/roc/warriors, its manual where the pick and
# the editor open it, and the pick in /lib/roc/apps, which rocchetto opens
# by name and the shell does not run: `corewar` stays the command.
CW_TREE = $(addprefix build/apps/warriors/,$(notdir $(wildcard $(CW)/warriors/*.red))) \
	build/apps/corewar/redcode.md build/apps/apps/corewar
APPS_TREE = $(addprefix build/apps/bin/,$(basename $(notdir $(APP_FBBS)))) \
	$(if $(filter 1,$(APPS_COREWAR)),$(CW_TREE)) $(EXTRA_TREE)

build/apps/warriors/%.red: $(CW)/warriors/%.red
	@mkdir -p $(dir $@)
	cp $< $@

$(CW)/corewar.fbb: FORCE
	$(MAKE) -C $(CW) corewar.fbb

build/apps/apps/corewar: $(CW)/corewar.fbb
	@mkdir -p $(dir $@)
	cmp -s $< $@ || cp $< $@

build/apps/corewar/redcode.md: $(CW)/redcode.md
	@mkdir -p $(dir $@)
	cp $< $@

# Each repository's own build says when its program changed; asking it
# every time is cheap and keeps the copy honest.
$(APP_FBBS): FORCE
	$(MAKE) -C $(dir $@) $(notdir $@)

define app_copy
build/apps/bin/$(basename $(notdir $(1))): $(1)
	@mkdir -p build/apps/bin
	cmp -s $$< $$@ || cp $$< $$@
endef
$(foreach f,$(APP_FBBS),$(eval $(call app_copy,$(f))))

# /bin holds the programs, not the sources they were compiled from.
$(GEN): $(SCREENS) $(ROC)/tools/embed.sh $(UNITS) $(APPS_TREE)
	@mkdir -p build build/apps
	sh $(ROC)/tools/embed.sh $(EMBED) $(TREE_ROOTS) build/units build/apps > $@

# The Cardputer carries its own screens, drawn for its 20x8 ASCII panel:
# lib/bin (the help), its commands compiled from commands/, the rest from
# cardputer/. Its units come from the same mkunits, linked against this
# tree instead.
CARDPUTER = $(shell find $(ROC)/cardputer -type f 2>/dev/null | sort)
CP_TREE = build/cardputer-tree/.built
CP_SRC = build/cardputer_src.c
CP_UNITS = build/cardputer-units/.built

$(CP_TREE): $(SCREENS) $(CARDPUTER)
	@rm -rf build/cardputer-tree && mkdir -p build/cardputer-tree
	@cp -R $(ROC)/lib/bin build/cardputer-tree/bin
	@cp -R $(ROC)/cardputer/. build/cardputer-tree/
	@touch $@

$(CP_SRC): $(CP_TREE) $(COMMANDS) $(ROC)/tools/embed.sh
	sh $(ROC)/tools/embed.sh $(EMBED) build/cardputer-tree $(ROC)/commands > $@

build/mkunits-cardputer: $(ALLSRC) $(CP_SRC) $(HDRS) $(ROC)/tools/mkunits.c
	$(CC) -std=c23 -O2 $(WARN) $(ALLAPPS) $(INC) -o $@ $(ALLSRC) $(CP_SRC) $(ROC)/tools/mkunits.c

$(CP_UNITS): build/mkunits-cardputer
	@rm -rf build/cardputer-units && mkdir -p build/cardputer-units
	./build/mkunits-cardputer --strip build/cardputer-units
	@touch $@

all: rocchetto

rocchetto: $(CORE) $(FILOSRC) $(GEN) $(HDRS) $(ROC)/host/posix/main.c
	$(CC) $(CFLAGS) -o $@ $(CORE) $(FILOSRC) $(GEN) $(ROC)/host/posix/main.c

# What release.sh publishes (VERSION is its tag, which the binary reports):
# rocchetto for macOS and for Linux.
DIST_DIR ?= dist
dist: ROC_VERSION := $(or $(VERSION),$(ROC_VERSION))
dist: $(CORE) $(FILOSRC) $(GEN) $(HDRS) $(ROC)/host/posix/main.c
	sh $(FILO_TERM)/tools/dist.sh $(DIST_DIR) rocchetto $(CFLAGS) \
		$(CORE) $(FILOSRC) $(GEN) $(ROC)/host/posix/main.c

# The binary as shipped, on a terminal: it starts, prompts, and exits.
smoke: rocchetto
	sh $(FILO_TERM)/tools/smoke.sh ./rocchetto 'exit\r'

.PHONY: all test footprint small fmt fmt-check tidy check fuzz clean all qa smoke dist wasm wasm-test compose lib FORCE

# The composing tool: the same runtime, screens read from a directory and
# redrawn as they are saved.
compose: $(CORE) $(FILOSRC) $(GEN) $(HDRS) $(ROC)/host/posix/compose.c
	$(CC) $(CFLAGS) -o $@ $(CORE) $(FILOSRC) $(GEN) $(ROC)/host/posix/compose.c

# libroc.a: the objects of the rocchetto binary without a host, for hosts that
# live in other repositories (fosforo). libroc.cflags carries what such a
# host must compile with: the ROC_APP_* flags shape struct roc, so a host
# built with others would read the wrong layout.
LIB_SRC = $(CORE) $(FILOSRC) $(GEN)
lib: build/libroc.a build/libroc.cflags

build/libroc.a: $(LIB_SRC) $(HDRS) build/libroc.cflags
	@rm -rf build/libobj && mkdir -p build/libobj
	@for f in $(LIB_SRC); do \
		$(CC) $(CFLAGS) -c $$f -o build/libobj/$$(basename $$f .c).o || exit 1; \
	done
	rm -f $@
	ar rcs $@ build/libobj/*.o

# The sources of libroc.a, for a host that compiles them itself for other
# platforms (fosforo's iOS slices); make lib first, for the generated ones.
lib-sources:
	@echo $(abspath $(LIB_SRC))

# rewritten only when the flags change, so a build with other APPS_* redoes
# the objects and an unchanged one does not
build/libroc.cflags: FORCE
	@mkdir -p build
	@printf '%s\n' '-std=c23 $(APPFLAGS) -I$(abspath $(ROC)/src) -I$(abspath build) -I$(abspath $(TERM_SRC)) -I$(abspath $(CW_SRC)) -I$(abspath $(FILO))' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

build/version.h: FORCE
	@mkdir -p build
	@printf '#define ROC_VERSION "%s"\n' '$(ROC_VERSION)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

FORCE:

# -Oz: every visitor downloads it, and speed is not what it lacks — a key
# costs well under a millisecond either way. Measured 2026-09-25: 382 KB to
# 284 KB (147 to 121 KB gzip), Filo runs 0-28% slower, the MARS the same;
# wasm-opt on top saves under 1 KB more, not worth a tool.
wasm: $(CORE) $(FILOSRC) $(GEN) $(HDRS) $(ROC)/host/wasm/wasm.c $(ROC)/host/wasm/libc.c $(ROC)/host/wasm/include/string.h
	@mkdir -p build
	PATH=$(LLD):$$PATH $(CLANG) --target=wasm32 -std=c23 -Oz \
		-ffreestanding -nostdlib -isystem $(ROC)/host/wasm/include $(INC) \
		$(WARN) $(APPFLAGS) \
		-Wl,--no-entry -Wl,-z,stack-size=131072 \
		-o build/roc.wasm $(CORE) $(FILOSRC) $(GEN) $(ROC)/host/wasm/wasm.c $(ROC)/host/wasm/libc.c
	$(WASM_OPT) --asyncify --pass-arg=asyncify-imports@env.host_site_wait,env.host_site_read,env.host_yield -Oz \
		build/roc.wasm -o build/roc.wasm
	@ls -la build/roc.wasm

# The wasm the site serves, run under Node with the page's imports faked
# (test/wasm/harness.mjs): the C tests do not show the freestanding build
# links, nor that the glue's contract holds.
WASM_TESTS = $(ROC)/test/wasm/test_wasm.mjs $(ROC)/test/wasm/test_store.mjs
wasm-test: wasm $(ROC)/test/wasm/harness.mjs $(WASM_TESTS) $(ROC)/web/store.mjs $(ROC)/test/fixtures/utilities.txt $(ROC)/test/fixtures/oracle.mjs
	for t in $(WASM_TESTS); do ROC_WASM=$(abspath build/roc.wasm) node $$t || exit 1; done

# A run is far more expensive now that a boot loads and runs a screen, so
# the bound is time, not iterations: the gate has to stay predictable.
FUZZ_SECONDS ?= 60

# The tests run where the repository is: they read its fixtures and the
# sources of its commands. A layer runs its own (TEST_SRC), which includes
# this file's.
TEST_SRC ?= $(ROC)/test/test_roc.c
test: $(CORE) $(FILOSRC) $(GEN) $(HDRS) $(TEST_SRC) $(CP_UNITS)
	@mkdir -p build
	$(CC) -std=c23 -O1 -g $(INC) -I$(ROC)/test -fsanitize=address,undefined -fno-sanitize-recover=all \
		$(WARN) $(APPFLAGS) -DROC_ROOT='"$(abspath $(ROC))"' \
		-o build/test_roc $(CORE) $(FILOSRC) $(GEN) $(TEST_SRC)
	./build/test_roc

FMT_SRC ?= $(wildcard $(addprefix $(ROC)/,src/*.c src/*.h host/*.c host/posix/*.c test/*.c fuzz/*.c tools/*.c))
fmt:
	$(CLANG_FORMAT) -i $(FMT_SRC)

fmt-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FMT_SRC)

# Padding is off: the shell's state is one big struct of arrays and grids,
# where a few bytes of alignment slack are noise against megabytes.
# simplify-boolean-expr is off for the same reason else-after-return is: the
# house style is guard clauses, and collapsing them into one negated return
# reads worse. It only started firing under C23, where true/false are the
# language's own and the check finally sees them.
# insecureAPI wants C11 Annex K (memcpy_s), which no libc here has; its strcpy
# check fires only where libc's strcpy is not fortified (glibc, not macOS),
# and every strcpy here copies a length checked just before or between
# buffers of one size. not-null-terminated-result reads a memcpy of a part as
# a string: the terminator here comes with the next copy.
tidy:
	$(CLANG_TIDY) --quiet --warnings-as-errors='*' \
		--checks='bugprone-*,cert-*,clang-analyzer-*,readability-*,-readability-identifier-length,-readability-function-cognitive-complexity,-readability-magic-numbers,-cert-err33-c,-readability-else-after-return,-readability-simplify-boolean-expr,-bugprone-easily-swappable-parameters,-clang-analyzer-optin.performance.Padding,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-clang-analyzer-security.insecureAPI.strcpy,-bugprone-not-null-terminated-result' \
		$(CORE) $(ROC)/host/posix/main.c $(ROC)/host/posix/compose.c $(ROC)/tools/mkunits.c -- -std=c23 $(INC) $(APPFLAGS)

check:
	cppcheck --enable=warning,style,performance,portability --inline-suppr \
		--suppress=missingIncludeSystem --error-exitcode=1 $(INC) $(APPFLAGS) $(CORE) $(ROC)/host/posix/main.c $(ROC)/host/posix/compose.c $(ROC)/tools/mkunits.c

# Needs a clang with libFuzzer (CLANG=/path/bin/clang when the system's has none).
fuzz: $(CORE) $(FILOSRC) $(GEN) $(HDRS) $(ROC)/fuzz/fuzz_input.c $(ROC)/fuzz/fuzz_home.c
	@mkdir -p build
	$(CLANG) -std=c23 -g -O1 $(INC) $(APPFLAGS) \
		-fsanitize=fuzzer,address,undefined -o build/fuzz_input $(CORE) $(FILOSRC) $(GEN) $(ROC)/fuzz/fuzz_input.c
	./build/fuzz_input -max_total_time=$(FUZZ_SECONDS) -timeout=10 -max_len=512
	$(CLANG) -std=c23 -g -O1 $(INC) -fsanitize=fuzzer,address,undefined \
		-o build/fuzz_home $(addprefix $(ROC)/src/,home.c ufs.c vfs.c) $(ROC)/fuzz/fuzz_home.c
	./build/fuzz_home -max_total_time=$(FUZZ_SECONDS) -timeout=10 -max_len=4096

qa: rocchetto compose small fmt-check test smoke tidy check wasm-test

clean:
	rm -rf rocchetto compose build dist

# What a build costs in RAM, by part. The same program under two
# configurations, which is the point of roc_config.h.
footprint: $(CORE) $(FILOSRC) $(GEN) $(HDRS) $(ROC)/tools/footprint.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o build/footprint $(CORE) $(FILOSRC) $(GEN) $(ROC)/tools/footprint.c
	@./build/footprint
	@echo "\nwhat the linker reserves for the whole binary:"
	@size rocchetto 2>/dev/null || true

# The small configuration: every limit that decides footprint pushed down
# to what a microcontroller holds. The gate builds it so that a fixed array
# added next month cannot quietly break the small build. The user's files
# stay tiny rather than absent — a device with a card mounts it instead.
SMALL = -DFT_CFG_COLS_MAX=100 -DFT_CFG_ROWS_MAX=40 \
	-DFT_CFG_OUT_CAP='(16*1024)' \
	-DROC_CFG_UFS_DATA_CAP='(32*1024)' -DROC_CFG_UFS_FILE_MAX='(16*1024)' \
	-DROC_CFG_UFS_FILES_MAX=16 \
	-DROC_CFG_VFS_ARENA_CAP='(64*1024)' -DROC_CFG_VFS_NODES_MAX=512 \
	-DFT_CFG_PAGER_TEXT_CAP='(64*1024)' -DFT_CFG_PAGER_LINES_MAX=2048 \
	-DFT_CFG_PAGER_ROWS_MAX=4096 \
	-DROC_CFG_SC_MEM_PERSISTENT='(32*1024)' -DROC_CFG_SC_MEM_RUN='(96*1024)' \
	-DROC_CFG_SC_SRC_MAX='(16*1024)' \
	-DROC_CFG_SCR_MEM_PERSISTENT='(96*1024)' -DROC_CFG_SCR_MEM_RUN='(64*1024)' \
	-DROC_CFG_CAP_MAX='(8*1024)' \
	-DFT_CFG_TB_CAP='(16*1024)' -DFT_CFG_TB_LINES_MAX=1024 \
	-DROC_CFG_HOME_BLOB_CAP='(8*1024)' -DROC_CFG_HIST_MAX=8 -DFILO_SYMBOLS_MAX=128 \
	-DROC_CFG_SC_REGEX=2

small: APPS_SCREENS = 0
small: EXTEND_SRC = $(ROC)/host/extend_none.c
small: LAYER_SRC =
small: APPS_EDIT = 0
small: APPS_COREWAR = 0
small: APPS_TOOLS = 0
small: $(CORE) $(FILOSRC) $(GEN) $(HDRS) $(ROC)/tools/footprint.c $(ROC)/host/posix/main.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SMALL) -o build/roc-small $(CORE) $(FILOSRC) $(GEN) $(ROC)/host/posix/main.c
	$(CC) $(CFLAGS) $(SMALL) -o build/footprint-small $(CORE) $(FILOSRC) $(GEN) $(ROC)/tools/footprint.c
	@./build/footprint-small
	@echo "\nwhat the linker reserves for the whole binary:"
	@size build/roc-small 2>/dev/null || true

# The runtime's own gate — built and tested without the shell — is
# filo-term's make qa.

# ---- the ESP32-S3 firmware -------------------------------------------------
#
# Arduino compiles what lives under the sketch folder, and the core lives
# elsewhere and is not moving for a build system — so each source gets a
# one-line stub that includes it. A stub per file keeps every translation
# unit separate, which a single unity file would not, and the list comes
# from CORE so there is nowhere for it to drift out of step.
#
# The tree is generated with --bytes: C23 #embed is the fast path here and
# the firmware toolchain (GCC 14.2) does not have it yet. The stubs are
# generated, so host/esp32/src is not in the repository.
#
# ctags is neutralised: Arduino ships an x86-only build of it and runs it
# only to invent prototypes for the sketch. Everything here is declared
# before it is used, so there is nothing for it to invent.

# The Cardputer's flash is 8 MB, and the embedded tree outgrew the 1.3 MB app
# of the default 4 MB layout: this one is the same shape, OTA kept, with 3 MB.
ESP32_FQBN ?= m5stack:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB
# the shell's serial port, which only the machine it is plugged into knows:
# make esp32-flash ESP32_PORT=/dev/cu.usbmodemXXXX
ESP32_PORT ?=
ESP32_DIR = $(ROC)/host/esp32
# only sizes here: what the build contains comes from APPS_*, through
# APPFLAGS, and saying it twice is how the two drifted apart once already
# The screen arenas and the symbol table are measured, not guessed, with the
# Cardputer's own screens at 20x8 on a 64-bit host (more than here, where
# values are smaller): 3.7 KB persistent at the peak, and the run arena
# works down to 1 KB. The widest screen of the shell names 74 globals.
# The scripts' arenas are sized for compiling on the shell, which /bin does
# not (its programs are units): a source takes about 11 times its size in
# each arena on 32 bits, so 12 KB compiles tree.filo (1 KB, 10.9 KB
# persistent and 10.4 KB run), the largest command, and a user's script as
# large; the REPL's lines are far smaller, and a source past 4 KB would not
# compile in them anyway.
# Everything else is cut to what fits the 232 KB of DRAM the sketch has: a
# pipe or a > holds 2 KB, the pager and ed 4 KB, a typed command 128 bytes
# (six rows of this screen). The clipboard is the full-screen editor's, which
# is not in this build.
ESP32_CFG = -DFT_CFG_COLS_MAX=20 -DFT_CFG_ROWS_MAX=8 -DFT_CFG_OUT_CAP=4096 \
	-DROC_CFG_UFS_DATA_CAP=8192 -DROC_CFG_UFS_FILE_MAX=4096 -DROC_CFG_UFS_FILES_MAX=6 \
	-DROC_CFG_VFS_ARENA_CAP=12288 -DROC_CFG_VFS_NODES_MAX=96 \
	-DFT_CFG_PAGER_TEXT_CAP=4096 -DFT_CFG_PAGER_LINES_MAX=128 \
	-DFT_CFG_PAGER_ROWS_MAX=256 -DFT_CFG_MD_LINE=1024 \
	-DROC_CFG_SC_MEM_PERSISTENT=12288 -DROC_CFG_SC_MEM_RUN=12288 \
	-DROC_CFG_SC_SRC_MAX=4096 -DROC_CFG_CAP_MAX=2048 \
	-DFT_CFG_TB_CAP=4096 -DFT_CFG_TB_LINES_MAX=256 -DFT_CFG_TB_CLIP=64 \
	-DROC_CFG_HOME_BLOB_CAP=4096 -DROC_CFG_HIST_MAX=8 \
	-DROC_CFG_SCR_MEM_PERSISTENT=6144 -DROC_CFG_SCR_MEM_RUN=6144 -DFILO_SYMBOLS_MAX=128 \
	-DROC_CFG_SC_REGEX=1 -DROC_CFG_SC_FILES=2 -DROC_CFG_SC_FILE_BUF=512 -DROC_CFG_SC_REPL=1024 \
	-DROC_CFG_SH_LINE=128 -DROC_CFG_SH_SCRIPT=1024 -DROC_CFG_SH_WORDS=32 -DROC_CFG_SUB_DEPTH=1 \
	-DROC_CFG_SH_TEXT=2048 -DROC_CFG_FUNC_TEXT=2048 -DROC_CFG_FUNCS=8 -DROC_CFG_LOCALS=8 \
	-DROC_CFG_ERRCAP=512 -DROC_CFG_RE_INST=128 -DROC_CFG_RE_CLASSES=8 -DROC_CFG_RE_STACK=512 \
	-DROC_CFG_SH_NEST=8 -DROC_CFG_SPOOL_PIECE=256

.PHONY: esp32-src esp32 esp32-flash

esp32-src: APPS_SCREENS = 0
esp32-src: EXTEND_SRC = $(ROC)/host/extend_none.c
esp32-src: LAYER_SRC =
esp32-src: APPS_EDIT = 0
esp32-src: APPS_COREWAR = 0
esp32-src: APPS_TOOLS = 0
esp32-src:
	@rm -rf $(ESP32_DIR)/src
	@mkdir -p $(ESP32_DIR)/src
	@for f in $(CORE) $(FILOSRC); do \
		b=$$(basename $$f); \
		printf '/* generated by "make esp32-src": the unit itself is %s.\n   Angle brackets on purpose — a quoted include would find this stub. */\n#include <%s>\n' \
			"$$f" "$$b" > $(ESP32_DIR)/src/$$b; \
	done
	@$(MAKE) --no-print-directory $(CP_UNITS)
	@sh $(ROC)/tools/embed.sh --bytes build/cardputer-tree build/cardputer-units > $(ESP32_DIR)/src/screens_data.c
	@printf 'esp32: %s translation units\n' "$$(ls $(ESP32_DIR)/src | wc -l | tr -d ' ')"

esp32: APPS_SCREENS = 0
esp32: EXTEND_SRC = $(ROC)/host/extend_none.c
esp32: LAYER_SRC =
esp32: APPS_EDIT = 0
esp32: APPS_COREWAR = 0
esp32: APPS_TOOLS = 0
esp32: esp32-src
	arduino-cli compile -b $(ESP32_FQBN) $(ESP32_DIR) \
		--build-property "tools.ctags.cmd.path=/usr/bin/true" \
		--build-property "compiler.c.extra_flags=-std=c23 -I$(abspath $(ROC)/src) -I$(abspath $(TERM_SRC)) -I$(CURDIR)/build -I$(abspath $(FILO)) $(APPFLAGS) $(ESP32_CFG)" \
		--build-property "compiler.cpp.extra_flags=-I$(abspath $(ROC)/src) -I$(abspath $(TERM_SRC)) -I$(CURDIR)/build -I$(abspath $(FILO)) $(APPFLAGS) $(ESP32_CFG)"

esp32-flash: esp32
	@test -n "$(ESP32_PORT)" || { echo "esp32-flash: name the port, ESP32_PORT=/dev/..."; exit 2; }
	arduino-cli upload -b $(ESP32_FQBN) -p $(ESP32_PORT) $(ESP32_DIR)
