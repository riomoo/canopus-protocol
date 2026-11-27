// Small services: cfg:u (system settings), hid:USER (buttons, touch,
// sensors), ndm:u, ptm:u, ac:u and friends.
#include "input.h"
#include "ipc.h"

#include <algorithm>
#include <cstring>
#include <set>

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
        if (name == "ac:u" && Ac(r)) return;
        // Network services the game may try (DLC shop, SpotPass): log each
        // command once so what the game expects of them shows up.
        if (name == "nim:aoc" || name == "ac:u" || name == "boss:U") {
            const u32 key = r.header();
            if (seen.insert(key).second)
                Log(LOG_INFO, "ipc", "%s: command %04X (header %08X): success", name.c_str(), r.id(), key);
        } else {
            Log(LOG_DEBUG, "ipc", "%s: command %04X: success", name.c_str(), r.id());
        }
        r.Reply(1, 0);
    }

    std::set<u32> seen;

    // The events among a request's translated parameters.
    static std::vector<std::shared_ptr<kernel::Event>> Events(Request& r)
    {
        std::vector<std::shared_ptr<kernel::Event>> out;
        const u32 normal = (r.cmd[0] >> 6) & 0x3F, translate = r.cmd[0] & 0x3F;
        u32 i = 1 + normal;
        const u32 end = i + translate;
        while (i < end) {
            const u32 d = r.cmd[i++];
            if ((d & 0xF) == 0 && (d & 0x20)) {  // calling process id
                ++i;
            } else if ((d & 0xF) == 0) {  // handles
                const u32 n = (d >> 26) + 1;
                for (u32 k = 0; k < n && i < end; ++k, ++i)
                    if (auto e = kernel::GetAs<kernel::Event>(r.cmd[i], kernel::Type::Event)) out.push_back(e);
            } else {  // a buffer: one address word
                ++i;
            }
        }
        return out;
    }

    // ac:u, as on a console with Wi-Fi switched off: connecting finishes at
    // once (its event fires) and fails, so the game reports it can't connect
    // instead of waiting for ever.
    bool Ac(Request& r)
    {
        static constexpr u32 kNotConnected = 0xE0A09D2E;
        u32* c = r.cmd;
        switch (r.id()) {
        case 0x0004:  // ConnectAsync(pid, event, config)
        case 0x0008:  // CloseAsync(pid, event)
            Log(LOG_INFO, "ac", "%s: no network", r.id() == 4 ? "ConnectAsync" : "CloseAsync");
            for (auto& e : Events(r)) {
                e->signaled = true;
                kernel::Signal(e.get(), *r.lock);
            }
            r.Reply(1, 0);
            return true;
        case 0x0005:  // GetConnectResult
            r.Reply(1, 0, kNotConnected);
            return true;
        case 0x0009:  // GetCloseResult
            r.Reply(1, 0);
            return true;
        case 0x000A:  // GetLastErrorCode
            r.Reply(2, 0);
            c[2] = 0;
            return true;
        case 0x000C:  // GetStatus: not connected
        case 0x000D:  // GetWifiStatus: not connected
            r.Reply(2, 0);
            c[2] = 0;
            return true;
        case 0x003E:  // IsConnected: no
            r.Reply(2, 0);
            c[2] = 0;
            return true;
        }
        return false;
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
