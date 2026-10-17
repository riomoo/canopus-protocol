// Sound on Windows: waveOut, which every Windows has. A thread keeps six
// 15 ms buffers queued, refilling each as the device finishes it.
#include "audio.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include <thread>
#include <vector>

namespace rt {
namespace audio {

bool OpenBackend()
{
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = kOutRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = kOutRate * 4;
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT dev = nullptr;
    if (waveOutOpen(&dev, WAVE_MAPPER, &fmt, (DWORD_PTR)done, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        CloseHandle(done);
        return false;
    }
    std::thread([dev, done] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        constexpr int kBuffers = 6, kFrames = kOutRate / 66;  // 15 ms each: waveOut needs some slack
        std::vector<s16> data(kBuffers * kFrames * 2, 0);
        WAVEHDR hdr[kBuffers] = {};
        for (int i = 0; i < kBuffers; ++i) {
            hdr[i].lpData = (LPSTR)&data[i * kFrames * 2];
            hdr[i].dwBufferLength = kFrames * 4;
            waveOutPrepareHeader(dev, &hdr[i], sizeof hdr[i]);
            Pull((s16*)hdr[i].lpData, kFrames);
            waveOutWrite(dev, &hdr[i], sizeof hdr[i]);
        }
        for (;;) {
            WaitForSingleObject(done, 50);
            for (int i = 0; i < kBuffers; ++i) {
                if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
                Pull((s16*)hdr[i].lpData, kFrames);
                waveOutWrite(dev, &hdr[i], sizeof hdr[i]);
            }
        }
    }).detach();
    return true;
}

}  // namespace audio
}  // namespace rt

#endif
