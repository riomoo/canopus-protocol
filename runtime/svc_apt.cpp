// APT:U, the applet manager. The game is the only application; the HOME
// menu never takes over. At start-up it is woken with the usual Wakeup
// parameter; nothing else is ever sent to it.
#include "ipc.h"

#include <cstring>
#include <deque>

namespace rt {
namespace ipc {

namespace {

constexpr u32 APPID_HOMEMENU = 0x101;
constexpr u32 APPID_APPLICATION = 0x300;
constexpr u32 RESULT_NO_DATA = 0xC880CFFA;

struct Parameter {
    u32 sender, command;
    std::vector<u8> data;
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
        r.cmd[6] = 0;     // no handle
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
            params.push_back({APPID_HOMEMENU, 1 /* Wakeup */, {}});
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
        case 0x0009:  // IsRegistered(appid)
            r.Reply(2, 0);
            r.cmd[2] = (r.cmd[1] == APPID_HOMEMENU || r.cmd[1] == APPID_APPLICATION) ? 1 : 0;
            return;
        case 0x000B:  // InquireNotification -> none
            r.Reply(2, 0);
            r.cmd[2] = 0;
            return;
        case 0x000C:  // SendParameter: nobody listens
            r.Reply(1, 0);
            return;
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
