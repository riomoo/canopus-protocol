#include "input.h"

#include <cstring>
#include <mutex>

namespace rt {
namespace input {

namespace {
std::shared_ptr<kernel::SharedMemory> s_mem;
std::shared_ptr<kernel::Event> s_events[5];
u32 s_prevButtons = 0;
u32 s_index = 0, s_touchIndex = 0;
u64 s_tick = 0;

void Init()
{
    if (s_mem) return;
    s_mem = std::make_shared<kernel::SharedMemory>();
    s_mem->size = 0x2B0;
    s_mem->name = "HID shared memory";
    static const char* names[5] = {"HID PAD0", "HID PAD1", "HID accelerometer", "HID gyroscope", "HID debug pad"};
    for (int i = 0; i < 5; ++i) {
        s_events[i] = std::make_shared<kernel::Event>();
        s_events[i]->name = names[i];
        s_events[i]->reset = kernel::ONESHOT;
    }
}
}  // namespace

std::shared_ptr<kernel::SharedMemory> SharedMemory()
{
    Init();
    return s_mem;
}

std::shared_ptr<kernel::Event> Event(int i)
{
    Init();
    return s_events[i];
}

void Update(const State& s)
{
    Init();
    u8* m = s_mem->Ptr();
    if (!m) return;  // not mapped yet
    auto w32 = [&](u32 o, u32 v) { std::memcpy(m + o, &v, 4); };
    auto w64 = [&](u32 o, u64 v) { std::memcpy(m + o, &v, 8); };
    auto w16 = [&](u32 o, u16 v) { std::memcpy(m + o, &v, 2); };
    s_tick += 268111856 / 60;
    // Pad: ring of 8 entries at 0x28 (current, pressed, released, circle pad).
    s_index = (s_index + 1) % 8;
    w64(0x08, s_tick - 268111856 / 60);
    w64(0x00, s_tick);
    w32(0x10, s_index);
    const u32 e = 0x28 + s_index * 0x10;
    w32(e + 0, s.buttons);
    w32(e + 4, s.buttons & ~s_prevButtons);
    w32(e + 8, ~s.buttons & s_prevButtons);
    w16(e + 12, (u16)s.circleX);
    w16(e + 14, (u16)s.circleY);
    w32(0x20, s.buttons);
    s_prevButtons = s.buttons;
    // Touch screen: ring of 8 at 0xA8 + 0x20.
    s_touchIndex = (s_touchIndex + 1) % 8;
    w64(0xA8 + 0x08, s_tick - 268111856 / 60);
    w64(0xA8 + 0x00, s_tick);
    w32(0xA8 + 0x10, s_touchIndex);
    const u32 t = 0xA8 + 0x20 + s_touchIndex * 8;
    w16(t + 0, s.touch ? s.touchX : 0);
    w16(t + 2, s.touch ? s.touchY : 0);
    w32(t + 4, s.touch ? 1 : 0);
    kernel::SignalEvent(s_events[0]);
}

}  // namespace input
}  // namespace rt
