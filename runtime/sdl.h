// SDL2, loaded at run time (dlopen) so the game starts without it: the few
// functions and structures the Linux window and sound use, declared here
// with SDL2's stable ABI.
#pragma once

#ifndef _WIN32

#include "rt.h"

namespace rt {
namespace sdl {

constexpr u32 INIT_AUDIO = 0x10, INIT_VIDEO = 0x20;
constexpr int WINDOWPOS_UNDEFINED = 0x1FFF0000;
constexpr u32 WINDOW_RESIZABLE = 0x20;
constexpr u32 RENDERER_ACCELERATED = 2, RENDERER_PRESENTVSYNC = 4;
constexpr u32 PIXELFORMAT_ARGB8888 = 0x16362004;
constexpr int TEXTUREACCESS_STREAMING = 1;
constexpr u16 AUDIO_S16LSB = 0x8010;

enum EventType : u32 {
    QUIT = 0x100, WINDOWEVENT = 0x200, KEYDOWN = 0x300, KEYUP = 0x301, TEXTINPUT = 0x303,
    MOUSEMOTION = 0x400, MOUSEBUTTONDOWN = 0x401, MOUSEBUTTONUP = 0x402,
};

struct Keysym {
    int32_t scancode;
    int32_t sym;
    u16 mod;
    u32 unused;
};
struct KeyboardEvent {
    u32 type, timestamp, windowID;
    u8 state, repeat, padding2, padding3;
    Keysym keysym;
};
struct MouseMotionEvent {
    u32 type, timestamp, windowID, which, state;
    int32_t x, y, xrel, yrel;
};
struct MouseButtonEvent {
    u32 type, timestamp, windowID, which;
    u8 button, state, clicks, padding1;
    int32_t x, y;
};
struct TextInputEvent {
    u32 type, timestamp, windowID;
    char text[32];
};
union Event {
    u32 type;
    KeyboardEvent key;
    TextInputEvent text;
    MouseMotionEvent motion;
    MouseButtonEvent button;
    u8 padding[56];
};
struct Rect {
    int x, y, w, h;
};
struct AudioSpec {
    int freq;
    u16 format;
    u8 channels, silence;
    u16 samples, padding;
    u32 size;
    void (*callback)(void* userdata, u8* stream, int len);
    void* userdata;
};

struct Api {
    int (*Init)(u32);
    int (*InitSubSystem)(u32);
    const char* (*GetError)();
    void* (*CreateWindow)(const char*, int, int, int, int, u32);
    void (*SetWindowTitle)(void*, const char*);
    void* (*CreateRenderer)(void*, int, u32);
    int (*RenderSetLogicalSize)(void*, int, int);
    void* (*CreateTexture)(void*, u32, int, int, int);
    int (*UpdateTexture)(void*, const Rect*, const void*, int);
    int (*RenderClear)(void*);
    int (*RenderCopy)(void*, void*, const Rect*, const Rect*);
    void (*RenderPresent)(void*);
    int (*SetRenderDrawColor)(void*, u8, u8, u8, u8);
    int (*WaitEventTimeout)(Event*, int);
    int (*PollEvent)(Event*);
    void (*RenderWindowToLogical)(void*, int, int, float*, float*);
    u32 (*OpenAudioDevice)(const char*, int, const AudioSpec*, AudioSpec*, int);
    void (*PauseAudioDevice)(u32, int);
};

// The API, or null if SDL2 is not installed.
const Api* Get();

}  // namespace sdl
}  // namespace rt

#endif
