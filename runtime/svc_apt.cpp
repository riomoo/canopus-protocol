// APT:U, the applet manager. The game is the only application; the HOME
// menu never takes over. At start-up it is woken with the usual Wakeup
// parameter; nothing else is ever sent to it.
#include "ipc.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>

namespace rt {
namespace ipc {

namespace {

constexpr u32 APPID_HOMEMENU = 0x101;
constexpr u32 APPID_APPLICATION = 0x300;
constexpr u32 RESULT_NO_DATA = 0xC880CFFA;

struct Parameter {
    u32 sender, command;
    std::vector<u8> data;
    kernel::ObjectPtr object;  // a handle that goes with it, if any
};

struct Apt : Service {
    Apt() : Service("APT:U")
    {
        lock->name = "APT lock";
        notify->name = "APT notification event";
        notify->reset = kernel::ONESHOT;
        resume->name = "APT parameter event";
        resume->reset = kernel::ONESHOT;
    }
    std::shared_ptr<kernel::Mutex> lock = std::make_shared<kernel::Mutex>();
    std::shared_ptr<kernel::Event> notify = std::make_shared<kernel::Event>();
    std::shared_ptr<kernel::Event> resume = std::make_shared<kernel::Event>();
    std::deque<Parameter> params;
    u32 applet = 0;  // the library applet asked for, if any

    void Signal(const std::shared_ptr<kernel::Event>& e, Request& r)
    {
        e->signaled = true;
        kernel::Signal(e.get(), *r.lock);
    }

    // ReceiveParameter / GlanceParameter: the parameter to the static buffer
    // the caller gave (TLS+0x180: descriptor, address).
    void SendParameterTo(Request& r, bool take)
    {
        if (params.empty()) {
            r.Reply(1, 0, RESULT_NO_DATA);
            return;
        }
        const struct Parameter& p = params.front();
        const u32 max = r.cmd[2];
        const u32 n = std::min<u32>(max, (u32)p.data.size());
        const u32 buf = mem::R32(r.thread->tls + 0x184);
        if (n && buf) std::memcpy(mem::Ptr(buf), p.data.data(), n);
        r.Reply(4, 4);
        r.cmd[2] = p.sender;
        r.cmd[3] = p.command;
        r.cmd[4] = n;
        r.cmd[5] = 0x10;  // move handle
        r.cmd[6] = p.object ? kernel::NewHandle(p.object) : 0;
        r.cmd[7] = n << 14 | 2;
        r.cmd[8] = buf;
        if (take) params.pop_front();
    }

    void Handle(Request& r) override
    {
        switch (r.id()) {
        case 0x0001:  // GetLockHandle -> attributes, state, lock
            r.Reply(3, 2);
            r.cmd[2] = 0;
            r.cmd[3] = 0;
            r.cmd[4] = 0x04000000;
            r.cmd[5] = kernel::NewHandle(lock);
            return;
        case 0x0002:  // Initialize(appid, attributes) -> notification and resume events
            r.Reply(1, 3);
            r.cmd[2] = 0x04000000;
            r.cmd[3] = kernel::NewHandle(notify);
            r.cmd[4] = kernel::NewHandle(resume);
            return;
        case 0x0003:  // Enable: the HOME menu wakes the application up (a parameter)
            params.push_back({APPID_HOMEMENU, 1 /* Wakeup */, {}, nullptr});
            Signal(resume, r);
            r.Reply(1, 0);
            return;
        case 0x0004:  // Finalize
            r.Reply(1, 0);
            return;
        case 0x0005:  // GetAppletManInfo(pos) -> pos, requested, home menu, active
            r.Reply(5, 0);
            r.cmd[3] = 0;
            r.cmd[4] = APPID_HOMEMENU;
            r.cmd[5] = APPID_APPLICATION;
            r.cmd[2] = r.cmd[2] & 0xFF;
            return;
        case 0x0009: {  // IsRegistered(appid)
            const u32 id = r.cmd[1];  // read before the reply overwrites it
            r.Reply(2, 0);
            r.cmd[2] = (id == APPID_HOMEMENU || id == APPID_APPLICATION || (applet && id == applet)) ? 1 : 0;
            return;
        }
        case 0x0006: {  // GetAppletInfo(appid) -> title, media type, registered, loaded, attributes
            const u32 id = r.cmd[1];
            const bool known = id == APPID_HOMEMENU || id == APPID_APPLICATION || (applet && id == applet);
            r.Reply(7, 0);
            r.cmd[2] = r.cmd[3] = 0;
            r.cmd[4] = 0;
            r.cmd[5] = known ? 1 : 0;
            r.cmd[6] = known ? 1 : 0;
            r.cmd[7] = 0;
            return;
        }
        // Library applets (the software keyboard, the error display, the
        // eShop): there are none here, so one "loads" at once and, when
        // started, closes straight away, handing back what it was given.
        case 0x0016:  // PreloadLibraryApplet(appid)
        case 0x0018:  // PrepareToStartLibraryApplet(appid)
            applet = r.cmd[1];
            Log(LOG_INFO, "apt", "%s library applet %03X (not available: it will close at once)",
                r.id() == 0x16 ? "preload" : "prepare", applet);
            r.Reply(1, 0);
            return;
        case 0x0017:  // FinishPreloadingLibraryApplet(appid)
        case 0x0010:  // SendCaptureBufferInfo
            r.Reply(1, 0);
            return;
        case 0x0011:  // ReceiveCaptureBufferInfo(size) -> size, buffer
            r.Reply(2, 2);
            r.cmd[2] = 0;
            r.cmd[3] = 2;  // empty static buffer
            r.cmd[4] = mem::R32(r.thread->tls + 0x184);
            return;
        case 0x001E: {  // StartLibraryApplet(appid, size, handle, parameter)
            const u32 id = r.cmd[1], size = r.cmd[2];
            const u32 buf = r.cmd[6];
            Parameter p{id, 10 /* WakeupByExit */, {}, nullptr};
            if (size && buf) {
                const u8* src = (const u8*)mem::Ptr(buf);
                p.data.assign(src, src + size);
            }
            Log(LOG_INFO, "apt", "start library applet %03X (%u bytes): closed at once", id, size);
            if (id == 0x406 && p.data.size() >= 12) {
                // The error display: what the game wanted to tell the player.
                const u8* d = p.data.data();
                std::string text;
                for (size_t i = 12; i + 1 < p.data.size() && text.size() < 300; i += 2) {
                    const u16 ch = d[i] | d[i + 1] << 8;
                    if (!ch) break;
                    text += ch < 0x80 ? (char)ch : '?';
                }
                u32 type, code;
                std::memcpy(&type, d, 4);
                std::memcpy(&code, d + 4, 4);
                Log(LOG_INFO, "apt", "error display: type %u, code %08X (error %03u-%04u) \"%s\"", type, code,
                    code / 10000, code % 10000, text.c_str());
            }
            params.push_back(std::move(p));
            applet = 0;
            Signal(resume, r);
            r.Reply(1, 0);
            return;
        }
        case 0x000B:  // InquireNotification -> none
            r.Reply(2, 0);
            r.cmd[2] = 0;
            return;
        case 0x000C: {  // SendParameter(source, destination, command, size, handle, buffer)
            const u32 dst = r.cmd[2], command = r.cmd[3], size = r.cmd[4], buf = r.cmd[8];
            if (applet && dst == applet && command == 2 /* Request */) {
                // The library applet answers the start-up request with the
                // shared memory for the screen capture it would draw over.
                u32 capture = size >= 4 && buf ? mem::R32(buf) : 0;
                auto shm = std::make_shared<kernel::SharedMemory>();
                shm->size = std::max<u32>(0x1000, (capture + 0xFFF) & ~0xFFFu);
                shm->name = "library applet capture memory";
                params.push_back({applet, 3 /* Response */, {}, shm});
                Signal(resume, r);
            }
            r.Reply(1, 0);
            return;
        }
        case 0x000D:  // ReceiveParameter
            SendParameterTo(r, true);
            return;
        case 0x000E:  // GlanceParameter
            SendParameterTo(r, false);
            return;
        case 0x000F:  // CancelParameter
            r.Reply(2, 0);
            r.cmd[2] = 0;
            return;
        case 0x003E:  // ReplySleepQuery
        case 0x0043:  // NotifyToWait
        case 0x004F:  // SetApplicationCpuTimeLimit
        case 0x0055:  // SetScreenCapPostPermission
            r.Reply(1, 0);
            return;
        case 0x004B:  // AppletUtility -> utility result
            r.Reply(2, 0);
            r.cmd[2] = 0;
            return;
        case 0x0050:  // GetApplicationCpuTimeLimit
            r.Reply(2, 0);
            r.cmd[2] = 30;
            return;
        case 0x0056:  // GetScreenCapPostPermission
            r.Reply(2, 0);
            r.cmd[2] = 0;
            return;
        }
        Unhandled(r);
    }
};

}  // namespace

void RegisterApt() { Register("APT:U", std::make_shared<Apt>()); }

}  // namespace ipc
}  // namespace rt
