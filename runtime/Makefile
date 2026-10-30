# Builds the game from the recompiler's output (GEN) and this runtime.
# Normally run through the top-level Makefile; directly:
#   make GEN=<generated C folder> BUILD=<output folder> TARGET=<target>
#
# Targets:
#   linux         this machine's gcc/g++ (or CC/CXX)
#   linux-compat  Zig's clang, runs on any Linux with glibc 2.28+
#   windows       MinGW-w64 cross compiler, win32 thread model
#   windows-zig   Zig's clang for Windows (works on Linux, macOS and Windows)
#
# DEBUG=1 keeps debug information (much larger binaries).

GEN ?= ../build/gen
BUILD ?= ../build
TARGET ?= linux
DEBUG ?= 0

# Zig: a "zig" on the PATH, or the one from "pip install ziglang".
ZIG ?= $(shell command -v zig >/dev/null 2>&1 && echo zig || echo python3 -m ziglang)

ifeq ($(TARGET),windows)
# The win32 thread model matters: the posix one (winpthreads) loses wake-ups
# under this runtime's load and the game hangs.
CC := x86_64-w64-mingw32-gcc-win32
CXX := x86_64-w64-mingw32-g++-win32
EXE := ds2rb.exe
LDLIBS := -static -lm -lopengl32 -lgdi32 -luser32 -lwinmm -lsynchronization -Wl,--stack,67108864
else ifeq ($(TARGET),windows-zig)
CC := $(ZIG) cc -target x86_64-windows-gnu
CXX := $(ZIG) c++ -target x86_64-windows-gnu
EXE := ds2rb.exe
LDLIBS := -lopengl32 -lgdi32 -luser32 -lwinmm -Wl,--stack,67108864
else ifeq ($(TARGET),linux-compat)
CC := $(ZIG) cc -target x86_64-linux-gnu.2.28
CXX := $(ZIG) c++ -target x86_64-linux-gnu.2.28
EXE := ds2rb
LDLIBS := -lm -lpthread -ldl
else ifeq ($(TARGET),linux)
CC ?= gcc
CXX ?= g++
EXE := ds2rb
LDLIBS := -static-libstdc++ -static-libgcc -lm -lpthread -ldl
else
$(error unknown TARGET "$(TARGET)": use linux, linux-compat, windows or windows-zig)
endif

OUT := $(BUILD)/$(TARGET)
# Windows: the icon and version information (res/canopus.rc).
RES_OBJ := $(if $(filter windows,$(TARGET)),$(OUT)/obj/rt/canopus_res.o,$(if $(filter windows-zig,$(TARGET)),$(OUT)/obj/rt/canopus.res,))
G := $(if $(filter 1,$(DEBUG)),-g,)
STRIP := $(if $(filter 1,$(DEBUG)),,-s)
GEN_OPT ?= -O1
GEN_FLAGS := -std=c11 $(GEN_OPT) $(G) -ffp-contract=off -fno-strict-aliasing -w -I$(GEN) $(EXTRA)
RT_FLAGS := -std=c++17 -O2 $(G) -Wall -Wextra -Wno-unused-parameter -ffp-contract=off -I$(GEN) -I.

GEN_SRC := $(wildcard $(GEN)/*.c)
GEN_OBJ := $(patsubst $(GEN)/%.c,$(OUT)/obj/gen/%.o,$(GEN_SRC))
RT_SRC := $(wildcard *.cpp)
RT_OBJ := $(patsubst %.cpp,$(OUT)/obj/rt/%.o,$(RT_SRC))

$(OUT)/$(EXE): $(GEN_OBJ) $(RT_OBJ) $(RES_OBJ)
	@test -n "$(GEN_SRC)" || { echo "No generated code in $(GEN): run 'make recomp' first."; exit 1; }
	$(CXX) $(STRIP) -o $@ $^ $(LDLIBS)

$(OUT)/obj/rt/canopus_res.o: res/canopus.rc res/canopus.ico
	@mkdir -p $(dir $@)
	x86_64-w64-mingw32-windres -O coff -o $@ res/canopus.rc

$(OUT)/obj/rt/canopus.res: res/canopus.rc res/canopus.ico
	@mkdir -p $(dir $@)
	$(ZIG) rc /fo $@ res/canopus.rc

$(OUT)/obj/gen/%.o: $(GEN)/%.c $(GEN)/recomp.h
	@mkdir -p $(dir $@)
	$(CC) $(GEN_FLAGS) -c $< -o $@

$(OUT)/obj/rt/%.o: %.cpp *.h $(GEN)/recomp.h
	@mkdir -p $(dir $@)
	$(CXX) $(RT_FLAGS) -c $< -o $@

# The runtime alone, as a library (the player's machine links it with the
# game it recompiles): make lib TARGET=windows-zig|linux-compat GEN=<dir with recomp.h>
LIB := $(OUT)/libcanopus_rt.a
AR_CMD := $(if $(filter windows-zig linux-compat,$(TARGET)),$(ZIG) ar,ar)
lib: $(LIB) $(RES_OBJ)
$(LIB): $(RT_OBJ)
	rm -f $@
	$(AR_CMD) rcs $@ $^

.PHONY: clean lib
clean:
	rm -rf $(OUT)
