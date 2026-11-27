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
LDLIBS := -static -lm -lopengl32 -lgdi32 -luser32 -lshell32 -lwinmm -lsynchronization -Wl,--stack,67108864
else ifeq ($(TARGET),windows-zig)
CC := $(ZIG) cc -target x86_64-windows-gnu
CXX := $(ZIG) c++ -target x86_64-windows-gnu
EXE := ds2rb.exe
LDLIBS := -lopengl32 -lgdi32 -luser32 -lshell32 -lwinmm -Wl,--stack,67108864
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
RT_FLAGS := -std=c++17 -O2 $(G) -Wall -Wextra -Wno-unused-parameter -ffp-contract=off -I$(GEN) -I. -Ithird_party/raylib/src

# raylib (third_party/raylib, zlib licence): the window, input, controllers
# and sound. OpenGL 2.1, GLFW; on Linux X11 only (XWayland covers Wayland),
# with X11, OpenGL and the sound libraries loaded when the game starts, so
# only the X11 headers are needed to build: X11_DIR (default /usr/include/X11).
RL := third_party/raylib/src
RL_SRC := rcore rglfw rshapes rtextures raudio
RL_OBJ := $(patsubst %,$(OUT)/obj/raylib/%.o,$(RL_SRC))
RL_FORMATS := PNG BMP TGA JPG GIF QOI PEP PSD DDS HDR PIC PNM KTX ASTC PKM PVR WAV OGG MP3 QOA FLAC XM MOD
RL_FLAGS := -std=gnu99 -O2 $(G) -w -fno-strict-aliasing -DPLATFORM_DESKTOP_GLFW -DGRAPHICS_API_OPENGL_21 \
	-DSUPPORT_SCREEN_CAPTURE=0 -DSUPPORT_AUTOMATION_EVENTS=0 -DSUPPORT_MODULE_RMODELS=0 -DSUPPORT_MODULE_RTEXT=0 \
	-DSUPPORT_CAMERA_SYSTEM=0 -DSUPPORT_GESTURES_SYSTEM=0 -DSUPPORT_MOUSE_GESTURES=0 -DSUPPORT_COMPRESSION_API=0 \
	-DSUPPORT_CLIPBOARD_IMAGE=0 $(patsubst %,-DSUPPORT_FILEFORMAT_%=0,$(RL_FORMATS)) -I$(RL) -I$(RL)/external/glfw/include
X11_DIR ?= /usr/include/X11
ifneq ($(filter linux linux-compat,$(TARGET)),)
RL_FLAGS += -D_GLFW_X11 -isystem $(OUT)/x11inc
RL_DEPS := $(OUT)/x11inc/X11/Xlib.h
endif

GEN_SRC := $(wildcard $(GEN)/*.c)
GEN_OBJ := $(patsubst $(GEN)/%.c,$(OUT)/obj/gen/%.o,$(GEN_SRC))
RT_SRC := $(wildcard *.cpp)
RT_OBJ := $(patsubst %.cpp,$(OUT)/obj/rt/%.o,$(RT_SRC))

$(OUT)/$(EXE): $(GEN_OBJ) $(RT_OBJ) $(RL_OBJ) $(RES_OBJ)
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

$(OUT)/obj/raylib/%.o: $(RL)/%.c $(RL_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(RL_FLAGS) -c $< -o $@

# Only the X11 headers, not the rest of /usr/include (Zig brings its own libc).
$(OUT)/x11inc/X11/Xlib.h:
	@test -f $(X11_DIR)/Xlib.h || { echo "X11 headers not found in $(X11_DIR): install them (Debian/Ubuntu: libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev; Fedora: libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel; Arch: libx11 libxrandr libxinerama libxcursor libxi) or set X11_DIR"; exit 1; }
	@mkdir -p $(OUT)/x11inc
	ln -sfn $(abspath $(X11_DIR)) $(OUT)/x11inc/X11

# The runtime alone, as a library (the player's machine links it with the
# game it recompiles): make lib TARGET=windows-zig|linux-compat GEN=<dir with recomp.h>
LIB := $(OUT)/libcanopus_rt.a
AR_CMD := $(if $(filter windows-zig linux-compat,$(TARGET)),$(ZIG) ar,ar)
lib: $(LIB) $(RES_OBJ)
$(LIB): $(RT_OBJ) $(RL_OBJ)
	rm -f $@
	$(AR_CMD) rcs $@ $^

.PHONY: clean lib
clean:
	rm -rf $(OUT)
