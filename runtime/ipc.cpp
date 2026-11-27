// Sessions, the service directory (srv:) and request dispatch.
#include "ipc.h"

#include <cstring>

namespace rt {
namespace ipc {

namespace {

std::map<std::string, std::shared_ptr<Service>>& Registry()
{
    static std::map<std::string, std::shared_ptr<Service>> r;
    return r;
}

// srv: hands out sessions to the other services.
struct Srv : Service {
    Srv() : Service("srv:") {}
    std::shared_ptr<kernel::Semaphore> notification = std::make_shared<kernel::Semaphore>();
    void Handle(Request& r) override
    {
        switch (r.id()) {
        case 0x0001:  // RegisterClient
            r.Reply(1, 0);
            return;
        case 0x0002:  // EnableNotification -> semaphore handle
            notification->max = 64;
            r.Reply(1, 2);
            r.cmd[2] = 0x20;  // copy handle
            r.cmd[3] = kernel::NewHandle(notification);
            return;
        case 0x0005: {  // GetServiceHandle(name[8], length, flags)
            char name[9] = {};
            std::memcpy(name, &r.cmd[1], 8);
            const std::string n(name, std::min<u32>(r.cmd[3], 8));
            const u32 h = OpenService(n);
            if (!h) {
                Log(LOG_WARN, "srv", "GetServiceHandle %s: no such service", n.c_str());
                r.Reply(1, 0, 0xD8E06406);
                return;
            }
            Log(LOG_INFO, "srv", "GetServiceHandle %s -> %X", n.c_str(), h);
            r.Reply(1, 2);
            r.cmd[2] = 0x10;  // move handle
            r.cmd[3] = h;
            return;
        }
        case 0x0009:  // Subscribe
        case 0x000A:  // Unsubscribe
            r.Reply(1, 0);
            return;
        case 0x000B:  // ReceiveNotification
            r.Reply(2, 0);
            r.cmd[2] = 0;
            return;
        }
        Unhandled(r);
    }
};

}  // namespace

void Service::Handle(Request& r) { Unhandled(r); }

void Service::Unhandled(Request& r)
{
    Log(LOG_WARN, "ipc", "%s: command %04X (header %08X) not handled; replying success", name.c_str(), r.id(), r.header());
    r.Reply(1, 0);
}

void Register(const std::string& name, std::shared_ptr<Service> s) { Registry()[name] = std::move(s); }

u32 OpenService(const std::string& name)
{
    auto it = Registry().find(name);
    if (it == Registry().end()) {
        // Unknown services get a stub that logs what is asked of it.
        Register(name, std::make_shared<Service>(name));
        it = Registry().find(name);
        Log(LOG_WARN, "ipc", "service %s is a stub", name.c_str());
    }
    return kernel::NewHandle(std::make_shared<Session>(it->second));
}

u32 ConnectToPort(const std::string& port, u32* handle)
{
    if (port == "srv:") {
        static auto srv = std::make_shared<Srv>();
        *handle = kernel::NewHandle(std::make_shared<Session>(srv));
        return kernel::RESULT_OK;
    }
    Log(LOG_WARN, "ipc", "ConnectToPort %s: no such port", port.c_str());
    return kernel::RESULT_NOT_FOUND;
}

u32 SendSyncRequest(u32 handle, kernel::Thread* t, std::unique_lock<std::mutex>& lk)
{
    auto s = kernel::GetAs<Session>(handle, kernel::Type::Session);
    if (!s) {
        Log(LOG_WARN, "ipc", "SendSyncRequest on %08X: not a session", handle);
        return kernel::RESULT_INVALID_HANDLE;
    }
    Request r{reinterpret_cast<u32*>(mem::Ptr(t->tls + 0x80)), t, &lk};
    Log(LOG_TRACE, "ipc", "%s %08X", s->service->name.c_str(), r.header());
    s->service->Handle(r);
    return kernel::RESULT_OK;
}

}  // namespace ipc
}  // namespace rt
