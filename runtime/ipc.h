// System services: the game talks to them through sessions (svcSendSyncRequest
// with the command in the thread's IPC buffer at TLS+0x80). Each service is a
// Service subclass that answers the commands the game uses.
#pragma once

#include "kernel.h"

#include <map>
#include <memory>
#include <string>

namespace rt {
namespace ipc {

// One request: cmd points at the 64-word IPC buffer in guest memory.
struct Request {
    u32* cmd;
    kernel::Thread* thread;
    std::unique_lock<std::mutex>* lock;
    u32 id() const { return cmd[0] >> 16; }
    u32 header() const { return cmd[0]; }
    // Writes the reply header (normal words include the result) and result.
    void Reply(u32 normal, u32 translate, u32 result = 0)
    {
        cmd[0] = id() << 16 | normal << 6 | translate;
        cmd[1] = result;
    }
};

// Lets go of the kernel lock for a slow host operation (disk), so the
// timing threads (sound frames, VBlank, timers) are not held up by it. The
// calling guest thread keeps the baton: no other guest thread runs meanwhile.
struct Unlocked {
    std::unique_lock<std::mutex>* l;
    explicit Unlocked(Request& r) : l(r.lock && r.lock->owns_lock() ? r.lock : nullptr)
    {
        if (l) l->unlock();
    }
    ~Unlocked()
    {
        if (l) l->lock();
    }
    Unlocked(const Unlocked&) = delete;
    Unlocked& operator=(const Unlocked&) = delete;
};

struct Service {
    explicit Service(std::string n) : name(std::move(n)) {}
    virtual ~Service() = default;
    std::string name;
    // Answers one request; the default logs it and replies success.
    virtual void Handle(Request& r);
    void Unhandled(Request& r);
};

struct Session : kernel::Object {
    explicit Session(std::shared_ptr<Service> s) : Object(kernel::Type::Session), service(std::move(s)) {}
    std::shared_ptr<Service> service;
};

void Register(const std::string& name, std::shared_ptr<Service> s);
u32 ConnectToPort(const std::string& port, u32* handle);
u32 SendSyncRequest(u32 handle, kernel::Thread* t, std::unique_lock<std::mutex>& lk);
// A new session to a registered service, as a handle (0 if unknown).
u32 OpenService(const std::string& name);

void RegisterAll();  // services.cpp

}  // namespace ipc
}  // namespace rt
