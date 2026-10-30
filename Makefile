# Devil Survivor 2 Record Breaker: PC port.
#
#   make setup         extract your dump in games/ into data/
#   make recomp        recompile data/code.bin into C (build/gen)
#   make linux         build build/linux/ds2rb (this machine's gcc)
#   make linux-compat  build build/linux-compat/ds2rb (Zig; any glibc 2.28+ Linux)
#   make windows       build build/windows/ds2rb.exe (MinGW-w64 cross compiler)
#   make windows-zig   build build/windows-zig/ds2rb.exe (Zig)
#   make run           run the Linux build
#   make package       player downloads in build/dist (launcher + tools, no game)
#   make clean         remove build/ (keeps data/)
#
# Everything made here comes from your own dump and stays on your machine:
# do not publish data/ or build/.

GAMES ?= games
DATA ?= data
BUILD ?= build
GEN := $(BUILD)/gen
HOSTCXX ?= c++
JOBS ?= $(shell nproc 2>/dev/null || echo 4)

SETUP_TOOL := $(BUILD)/tools/ds2rb_setup
# The game version in data/ (1.1 once the update is installed) picks the
# recompiler's function table and hints. It is read when recompiling, not
# when make starts: "make setup recomp" may install the update first.
VERSION_FILE := $(BUILD)/game-version
RECOMP_SRC := $(wildcard recomp/*.go recomp/arm/*.go) recomp/go.mod recomp/runtime/recomp.h \
	recomp/hints.txt recomp/functions.csv recomp/hints_v11.txt recomp/functions_v11.csv
TARGETS := linux linux-compat windows windows-zig

.PHONY: help setup recomp $(TARGETS) run clean distclean FORCE package package-windows package-linux
FORCE:

help:
	@sed -n '3,13p' Makefile | sed 's/^# \{0,1\}//'

# ---- game data ----

$(SETUP_TOOL): setup/setup_main.cpp setup/ctr.cpp setup/ctr.h setup/sha256.h
	@mkdir -p $(dir $@)
	$(HOSTCXX) -std=c++17 -O2 -o $@ setup/setup_main.cpp setup/ctr.cpp

setup: $(SETUP_TOOL)
	@mkdir -p $(GAMES)
	$(SETUP_TOOL) $(GAMES) $(DATA)

$(DATA)/code.bin:
	@echo "No $(DATA)/code.bin yet. Put a decrypted dump of your game in $(GAMES)/ and run 'make setup'."
	@exit 1

# ---- recompiling ----

# Rewritten only when the version in data/ differs from the last recompile,
# so installing (or removing) the update makes the next build recompile.
$(VERSION_FILE): FORCE
	@mkdir -p $(dir $@)
	@v=$$(cat $(DATA)/version.txt 2>/dev/null || echo 1.0); \
	if [ "$$(cat $@ 2>/dev/null)" != "$$v" ]; then echo "$$v" > $@; fi

$(GEN)/recomp.h: $(DATA)/code.bin $(VERSION_FILE) $(RECOMP_SRC)
	rm -rf $(GEN)
	@mkdir -p $(GEN)
	@v=$$(cat $(VERSION_FILE)); \
	if [ "$$v" = 1.1 ]; then funcs=functions_v11.csv; hints=hints_v11.txt; \
	else funcs=functions.csv; hints=hints.txt; fi; \
	echo "Recompiling game version $$v ($$funcs, $$hints)"; \
	cd recomp && go run . -code $(abspath $(DATA)/code.bin) -exheader $(abspath $(DATA)/exheader.bin) \
		-funcs $$funcs -hints $$hints -out $(abspath $(GEN))

recomp: $(GEN)/recomp.h

# ---- building ----

$(TARGETS): $(GEN)/recomp.h
	$(MAKE) -C runtime -j$(JOBS) GEN=$(abspath $(GEN)) BUILD=$(abspath $(BUILD)) TARGET=$@

run: linux
	$(BUILD)/linux/ds2rb --data $(DATA)

# ---- player downloads ----
# The launcher builds the game on the player's machine from their own dump.
package: package-windows package-linux
package-windows:
	packaging/package.sh windows
package-linux:
	packaging/package.sh linux

clean:
	rm -rf $(BUILD)

# Also forgets the extracted game data (make setup again to restore it).
distclean: clean
	rm -rf $(DATA)
