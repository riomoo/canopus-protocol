# Devil Survivor 2 Record Breaker: PC port.
#
#   make setup         extract your dump in games/ into data/
#   make recomp        recompile data/code.bin into C (build/gen)
#   make linux         build build/linux/ds2rb (this machine's gcc)
#   make linux-compat  build build/linux-compat/ds2rb (Zig; any glibc 2.28+ Linux)
#   make windows       build build/windows/ds2rb.exe (MinGW-w64 cross compiler)
#   make windows-zig   build build/windows-zig/ds2rb.exe (Zig)
#   make run           run the Linux build
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
RECOMP_SRC := $(wildcard recomp/*.go recomp/arm/*.go) recomp/go.mod recomp/hints.txt recomp/functions.csv recomp/runtime/recomp.h
TARGETS := linux linux-compat windows windows-zig

.PHONY: help setup recomp $(TARGETS) run clean distclean

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

$(GEN)/recomp.h: $(DATA)/code.bin $(RECOMP_SRC)
	rm -rf $(GEN)
	@mkdir -p $(GEN)
	cd recomp && go run . -code $(abspath $(DATA)/code.bin) -exheader $(abspath $(DATA)/exheader.bin) \
		-funcs functions.csv -hints hints.txt -out $(abspath $(GEN))

recomp: $(GEN)/recomp.h

# ---- building ----

$(TARGETS): $(GEN)/recomp.h
	$(MAKE) -C runtime -j$(JOBS) GEN=$(abspath $(GEN)) BUILD=$(abspath $(BUILD)) TARGET=$@

run: linux
	$(BUILD)/linux/ds2rb --data $(DATA)

clean:
	rm -rf $(BUILD)

# Also forgets the extracted game data (make setup again to restore it).
distclean: clean
	rm -rf $(DATA)
