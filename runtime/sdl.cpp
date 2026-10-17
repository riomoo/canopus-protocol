// Loads SDL2 at run time.
#include "sdl.h"

#ifndef _WIN32

#include <dlfcn.h>

#include <mutex>

namespace rt {
namespace sdl {

const Api* Get()
{
    static Api api;
    static bool ok = false;
    static std::once_flag once;
    std::call_once(once, [] {
        void* lib = nullptr;
        for (const char* name : {"libSDL2-2.0.so.0", "libSDL2.so", "libSDL2-2.0.so"})
            if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
        if (!lib) {
            Log(LOG_WARN, "sdl", "SDL2 is not installed (libSDL2-2.0.so.0): no window or sound; install it with your package manager");
            return;
        }
        bool all = true;
        auto get = [&](auto& fn, const char* name, bool required = true) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            if (!fn && required) {
                Log(LOG_WARN, "sdl", "SDL2 lacks %s", name);
                all = false;
            }
        };
        get(api.Init, "SDL_Init");
        get(api.InitSubSystem, "SDL_InitSubSystem");
        get(api.GetError, "SDL_GetError");
        get(api.CreateWindow, "SDL_CreateWindow");
        get(api.SetWindowTitle, "SDL_SetWindowTitle");
        get(api.CreateRenderer, "SDL_CreateRenderer");
        get(api.RenderSetLogicalSize, "SDL_RenderSetLogicalSize");
        get(api.CreateTexture, "SDL_CreateTexture");
        get(api.UpdateTexture, "SDL_UpdateTexture");
        get(api.RenderClear, "SDL_RenderClear");
        get(api.RenderCopy, "SDL_RenderCopy");
        get(api.RenderPresent, "SDL_RenderPresent");
        get(api.SetRenderDrawColor, "SDL_SetRenderDrawColor");
        get(api.WaitEventTimeout, "SDL_WaitEventTimeout");
        get(api.PollEvent, "SDL_PollEvent");
        get(api.RenderWindowToLogical, "SDL_RenderWindowToLogical", false);
        get(api.OpenAudioDevice, "SDL_OpenAudioDevice");
        get(api.PauseAudioDevice, "SDL_PauseAudioDevice");
        ok = all;
    });
    return ok ? &api : nullptr;
}

}  // namespace sdl
}  // namespace rt

#endif
