# raylib 6.2 (vendored)

The window, keyboard, mouse, controllers and sound of the runtime
(`window_raylib.cpp`, `audio_raylib.cpp`). Only the files the runtime
compiles are here, unchanged, from https://github.com/raysan5/raylib at tag
6.2: `src/` with the GLFW backend (`rglfw.c`, `external/glfw`), miniaudio
and glad. Licences: `LICENSE` (raylib, zlib/libpng) and `LICENSE-glfw.md`
(GLFW, zlib/libpng); miniaudio is public domain / MIT-0.

Build flags are in `runtime/Makefile` (`RL_FLAGS`): OpenGL 2.1, no model,
text or file-format modules, no screenshot key, X11 on Linux.
