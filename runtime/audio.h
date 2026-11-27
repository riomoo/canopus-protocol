// Sound out of the host: the DSP pushes mixed stereo frames at the 3DS rate
// (32728 Hz); a platform backend (audio_win32.cpp, audio_sdl.cpp) pulls them
// at its own rate through a small ring buffer with a resampler.
#pragma once

#include "rt.h"

namespace rt {
namespace audio {

constexpr int kDspRate = 32728;
constexpr int kOutRate = 48000;

// Starts the platform's sound output; false (and silence) if there is none.
bool Open();
// Queues interleaved stereo samples at kDspRate.
void Push(const s16* stereo, int frames);
// For backends: fills `frames` interleaved stereo samples at kOutRate
// (silence when nothing is queued).
void Pull(s16* stereo, int frames);

// Platform backends (one is compiled in; false if unavailable).
bool OpenBackend();

}  // namespace audio
}  // namespace rt
