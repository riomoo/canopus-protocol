# Canopus Protocol

A native PC (Windows and Linux) port of *Shin Megami Tensei: Devil Survivor 2
Record Breaker* (Nintendo 3DS), made by static recompilation. The game's ARM
code is translated to C on your machine and built together with a runtime
that does what the 3DS did: its kernel, system services, GPU (a software
PICA200 renderer), DSP audio, window and input.

**This repository contains no game data and no game code.** You need a dump
of your own copy of the game. Everything made from it (the extracted data
and the generated C code) stays on your machine; don't publish `data/` or
`build/`.

## What you need

- A **decrypted** dump of the game, as `.3ds`, `.cci`, `.cia` or `.cxi`.
  Make one from your own console with GodMode9. The supported version is
  the North American release; the setup step checks this. Updates and DLC
  aren't supported yet.
- [Go](https://go.dev/dl/) 1.21 or newer (runs the recompiler).
- `make` and a C/C++ compiler for the platform you're building for (below).
- About 3 GB of free disk space for the extracted game data.

## Where your dump goes

```
games/        <- put your .3ds / .cia here
data/         <- made by "make setup": code.bin, exheader.bin, romfs/, saves
build/gen/    <- made by "make recomp": the recompiled C code
build/<target>/ds2rb(.exe)  <- the game
```

Copy your dump into the `games/` folder. The file name doesn't matter.

## Building

All commands run from the top folder of this repository.

```sh
make setup      # extracts your dump from games/ into data/
make recomp     # recompiles data/code.bin into build/gen/ (a few seconds)
make linux      # or: make windows, make windows-zig, make linux-compat
```

`make setup` and `make recomp` are only needed once. Compiling the game
takes a few minutes the first time; later builds only recompile what
changed. Run `make` on its own to see every target.

### Linux

Install a compiler, make, Go, and SDL2 (used for the window and sound,
loaded when the game starts):

```sh
# Debian / Ubuntu
sudo apt install build-essential golang libsdl2-2.0-0
# Fedora
sudo dnf install gcc-c++ make golang SDL2
# Arch
sudo pacman -S base-devel go sdl2
```

Then:

```sh
make setup recomp linux
make run
```

`make linux` builds `build/linux/ds2rb` with your system's gcc.
`make linux-compat` builds `build/linux-compat/ds2rb` with Zig instead; that
binary runs on any Linux with glibc 2.28 or newer (2018 onwards), handy for
building once and playing on another machine. It needs Zig: either `zig` on
your PATH, or `pip install ziglang`.

### Windows

The Windows build is made with a cross compiler. Two ways:

**From WSL (Ubuntu) or any Linux, with MinGW-w64.** Recommended.

```sh
sudo apt install build-essential golang g++-mingw-w64-x86-64
make setup recomp windows
```

This builds `build/windows/ds2rb.exe`. The Makefile uses the *win32* thread
model compiler (`x86_64-w64-mingw32-g++-win32`). The posix one makes the
game hang, so don't swap it.

**With Zig (works from Windows itself, Linux or macOS).** Install Zig
([ziglang.org](https://ziglang.org/download/) or `pip install ziglang`), Go,
and `make` (for example from [MSYS2](https://www.msys2.org/):
`pacman -S make`). Then, in the MSYS2 shell:

```sh
make setup recomp windows-zig
```

This builds `build/windows-zig/ds2rb.exe`. Don't build the Windows version
with MSYS2's own gcc: it uses posix threads, which make the game hang.

## Playing

The game looks for its data in a `data` folder in the current directory,
or wherever `--data` points:

```sh
build/linux/ds2rb --data data          # Linux (or: make run)
build\windows\ds2rb.exe --data data    # Windows, from this folder
```

On Windows, `run.bat` in this folder starts whichever build you made.
`run.sh` does the same on Linux, you may need to run `chmod +x run.sh`. To play from somewhere else, copy the
`.exe` and pass the full path of `data` with `--data`.

### Controls

| 3DS | Keyboard |
| --- | --- |
| +Control Pad | Arrow keys |
| A / B | X / Z |
| X / Y | S / A |
| L / R | Q / W |
| START / SELECT | Enter / Backspace |
| Circle Pad | I J K L |
| Touch screen | Left mouse button on the bottom screen |

Saves are kept in `data/save/`. The window can be resized. The title bar
shows the frame rate and how busy the renderer is.

### If something goes wrong

The game writes `ds2rb.log` in the folder it runs from. It records sound
dropouts, stalls (`hitch` and `no new picture` lines), a frame-rate line
every 10 seconds and, on a crash, the registers and stack. Include it when
reporting a problem.

Options for testing: `-v` logs more, `--headless` runs without a window.
Environment variables used for debugging:

| Variable | Effect |
| --- | --- |
| `DS2RB_INPUT` | scripted input, e.g. `2000:START;2600:UP;2700-40000/90:A` |
| `DS2RB_DUMP=n` | write both screens to `frames/` every n frames |
| `DS2RB_WAV=file` | also write the sound to a WAV file |
| `DS2RB_SPEED=x` | run the game's clock x times faster |
| `DS2RB_TRACE=s` | log every system call after s seconds |
| `DS2RB_GPU_SYNC` | make the renderer synchronous |

## How it works

| Folder | What's in it |
| --- | --- |
| `setup/` | `ds2rb_setup`: reads decrypted 3DS containers (NCSD, CIA, NCCH), checks the game version and extracts `code.bin` and the RomFS. |
| `recomp/` | The static recompiler (Go). It decodes ARMv6K/Thumb/VFPv2, discovers functions (with `functions.csv`, a table of addresses, names and sizes from the reverse engineering, and `hints.txt`) and writes one C function per game function. `recomp/runtime/recomp.h` is the header the generated code is compiled with. |
| `runtime/` | The C++ runtime: a kernel with guest threads, synchronization and timers (`kernel.cpp`), IPC services such as fs, gsp, dsp, apt, hid, cfg and y2r (`svc_*.cpp`), the PICA200 GPU (`pica*.cpp`), DSP audio mixing, the window and input for Win32 and SDL2, and patches for the hand-written ARM the recompiler can't express (coroutines in `patches.cpp`). |

## Status

The game boots, plays the opening movie with sound, and gets through New
Game, the prologue and registration. Known gaps: no hardware-accelerated
renderer yet (it renders in software on all cores), no fragment lighting,
no DLC.
