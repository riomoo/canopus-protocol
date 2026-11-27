// The cheat menu: an overlay on the top screen, opened with F1, that edits
// the loaded game (money, demons, skills, the party, the compendium).
#pragma once

#include "rt.h"

namespace rt {
namespace cheat {

// Keys the window passes on (besides printable ASCII, which filters lists).
enum Key : int {
    KEY_TOGGLE = 0x100,  // F1
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_ENTER, KEY_BACK, KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_ERASE,
};

// From the window thread: a key was pressed. Returns true if the menu took
// it (the game should not see it).
bool PushKey(int key);
// The menu is open: the game gets no input meanwhile.
bool Open();
// Once per frame, before it is shown: runs the menu and draws it over the
// top screen (400x240, 0xAARRGGBB).
void Frame(u32* top);

}  // namespace cheat
}  // namespace rt
