// Small services: cfg:u (system settings), hid:USER (buttons, touch,
// sensors), ndm:u, ptm:u, ac:u and friends.
#include "input.h"
#include "ipc.h"

#include <algorithm>
#include <cstring>

namespace rt {
namespace ipc {

namespace {

// cfg:u: system settings the game asks for, as on a US console set to English.
struct Cfg : Service {
    Cfg() : Service("cfg:u") {}
    void Handle(Request& r) override
    {
        auto& c = r.cmd;
        switch (r.id()) {
        case 0x0001: {  // GetConfigInfoBlk2(size, block id) -> data at the buffer
            const u32 size = c[1], id = c[2], buf = c[4];
            std::memset(mem::Ptr(buf), 0, size);
            switch (id) {
            case 0x000A0002:  // language: English
                mem::W8(buf, 1);
                break;
            case 0x000B0000:  // country: United States (49), US region
                if (size >= 4) { mem::W8(buf + 2, 1); mem::W8(buf + 3, 49); }
                break;
            case 0x000A0000: {  // user name
                const char* n = "Player";
                for (u32 i = 0; n[i] && 2 * i + 1 < size; ++i) mem::W16(buf + 2 * i, (u16)n[i]);
                break;
            }
            case 0x00070001:  // sound output: stereo
                mem::W8(buf, 1);
                break;
            case 0x000A0001:  // birthday
                if (size >= 2) { mem::W8(buf, 1); mem::W8(buf + 1, 1); }
                break;
            case 0x00050005: {  // stereo display calibration: retail defaults
                const float v[8] = {62.0f, 289.0f, 76.80000305f, 46.08000183f, 10.0f, 5.0f, 55.58000183f, 21.56999969f};
                std::memcpy(mem::Ptr(buf), v, std::min<u32>(size, sizeof v));
                break;
            }
            default:
                Log(LOG_DEBUG, "cfg", "block %08X (size %X): zeros", id, size);
                break;
            }
            r.Reply(1, 2);
            c[2] = size << 4 | 0xC;
            c[3] = buf;
            return;
        }
        case 0x0002:  // GetRegion: USA
            r.Reply(2, 0);
            c[2] = 1;
            return;
        case 0x0003:  // GenHashConsoleUnique
            r.Reply(3, 0);
            c[2] = 0x12345678;
            c[3] = 0x9ABCDEF0;
            return;
        case 0x0004:  // GetRegionCanadaUSA
        case 0x0005:  // GetSystemModel: old 3DS
            r.Reply(2, 0);
            c[2] = 0;
            return;
        case 0x0006:  // GetModelNintendo2DS: not a 2DS
            r.Reply(2, 0);
            c[2] = 1;
            return;
        }
        Unhandled(r);
    }
};

// hid:USER: the pad, touch screen and sensors through shared memory.
struct Hid : Service {
    Hid() : Service("hid:USER") {}
    void Handle(Request& r) override
    {
        auto& c = r.cmd;
        switch (r.id()) {
        case 0x000A:  // GetIPCHandles -> shared memory, PAD0, PAD1, accel, gyro, debug pad events
            r.Reply(1, 7);
            c[2] = 0x14000000;
            c[3] = kernel::NewHandle(input::SharedMemory());
            for (int i = 0; i < 5; ++i) c[4 + i] = kernel::NewHandle(input::Event(i));
            return;
        case 0x0011:  // EnableAccelerometer
        case 0x0012:  // DisableAccelerometer
        case 0x0013:  // EnableGyroscopeLow
        case 0x0014:  // DisableGyroscopeLow
            r.Reply(1, 0);
            return;
        case 0x0015:  // GetGyroscopeLowRawToDpsCoefficient
            r.Reply(2, 0);
            c[2] = 0x41700000;  // 15.0f
            return;
        case 0x0016:  // GetGyroscopeLowCalibrateParam
            r.Reply(6, 0);
            std::memset(&c[2], 0, 20);
            return;
        case 0x0017:  // GetSoundVolume
            r.Reply(2, 0);
            c[2] = 63;
            return;
        }
        Unhandled(r);
    }
};

// Services the game only initialises or asks trivial things of.
struct Simple : Service {
    explicit Simple(const char* n) : Service(n) {}
    void Handle(Request& r) override
    {
        if (name == "ptm:u") {
            switch (r.id()) {
            case 0x0005:  // GetAdapterState: plugged in
            case 0x0007:  // GetBatteryLevel: full
                r.Reply(2, 0);
                r.cmd[2] = r.id() == 7 ? 5 : 1;
                return;
            case 0x0008:  // GetBatteryChargeState
            case 0x0009:  // GetPedometerState
                r.Reply(2, 0);
                r.cmd[2] = 0;
                return;
            }
        }
        if (name == "cecd:u" && (r.id() == 0x000F || r.id() == 0x0010)) {
            // GetCecInfoEventHandle / GetChangeStateEventHandle: an event that
            // never fires (no StreetPass).
            auto e = std::make_shared<kernel::Event>();
            e->name = "cecd event (never signalled)";
            r.Reply(1, 2);
            r.cmd[2] = 0;
            r.cmd[3] = kernel::NewHandle(e);
            return;
        }
        if (name == "cecd:u" && r.id() == 0x000E) {  // GetCecStateAbbreviated: idle
            r.Reply(2, 0);
            r.cmd[2] = 1;
            return;
        }
        if (name == "ac:u" && r.id() == 0x000D) {  // GetWifiStatus: not connected
            r.Reply(2, 0);
            r.cmd[2] = 0;
            return;
        }
        Log(LOG_DEBUG, "ipc", "%s: command %04X: success", name.c_str(), r.id());
        r.Reply(1, 0);
    }
};

}  // namespace

void RegisterMisc()
{
    Register("cfg:u", std::make_shared<Cfg>());
    Register("hid:USER", std::make_shared<Hid>());
    for (const char* n : {"ndm:u", "ptm:u", "ac:u", "boss:U", "frd:u", "news:u", "cecd:u", "ir:USER", "mic:u", "nim:aoc"})
        Register(n, std::make_shared<Simple>(n));
}

}  // namespace ipc
}  // namespace rt
