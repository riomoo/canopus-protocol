// Sound on Linux (and anything else that is not Windows): SDL2, loaded at
// run time so the binary does not need it to start.
#include "audio.h"

#ifndef _WIN32

#include "sdl.h"

namespace rt {
namespace audio {

bool OpenBackend()
{
    const sdl::Api* s = sdl::Get();
    if (!s) return false;
    if (s->InitSubSystem(sdl::INIT_AUDIO) != 0) {
        Log(LOG_WARN, "audio", "SDL audio: %s", s->GetError());
        return false;
    }
    sdl::AudioSpec want = {}, have = {};
    want.freq = kOutRate;
    want.format = sdl::AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 512;
    want.callback = [](void*, u8* stream, int len) { Pull(reinterpret_cast<s16*>(stream), len / 4); };
    const u32 dev = s->OpenAudioDevice(nullptr, 0, &want, &have, 0);  // no changes allowed: SDL converts
    if (!dev) {
        Log(LOG_WARN, "audio", "SDL audio device: %s", s->GetError());
        return false;
    }
    s->PauseAudioDevice(dev, 0);
    return true;
}

}  // namespace audio
}  // namespace rt

#endif
