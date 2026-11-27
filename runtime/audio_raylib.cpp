// Sound out: raylib's audio device (miniaudio: WASAPI / DirectSound /
// waveOut on Windows, PulseAudio / ALSA / PipeWire on Linux, loaded at run
// time). Its mixing thread pulls frames from the ring buffer in audio.cpp.
#include "audio.h"

#include "raylib.h"

namespace rt {

void InstallRaylibLog();  // window_raylib.cpp

namespace audio {

bool OpenBackend()
{
    InstallRaylibLog();
    InitAudioDevice();
    if (!IsAudioDeviceReady()) return false;
    SetAudioStreamBufferSizeDefault(1024);
    static AudioStream stream = LoadAudioStream(kOutRate, 16, 2);
    if (!IsAudioStreamValid(stream)) return false;
    SetAudioStreamCallback(stream, [](void* data, unsigned int frames) { Pull(static_cast<s16*>(data), (int)frames); });
    PlayAudioStream(stream);
    return true;
}

}  // namespace audio
}  // namespace rt
