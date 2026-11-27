// The launcher's window (raylib): the Canopus backdrop with the progress of
// the first-time build, at the size and place the game's window then takes
// over, so the one becomes the other.
#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ui {

struct Progress {
    std::mutex lock;
    std::string stage;               // what is happening ("Compiling the game")
    std::string detail;              // its latest detail ("23 of 48 parts done")
    float fraction = -1;             // 0..1, or < 0 when unknown
    std::vector<std::string> lines;  // everything said, for when it fails
    enum State { Working, Ready, Failed } state = Working;
    bool needGames = false;          // the games folder is empty: offer to open it
};

// Opens the window (hidden until it has its picture). False: no display.
bool Open(const char* title);

// Runs the window until the game has taken over (true) or the player
// closed it (false). Once the build is Ready, start(x, y, w, h) starts the
// game where this window is; handedOver() then says when the game's window
// is up.
bool Run(Progress& p, const std::function<bool(int, int, int, int)>& start, const std::function<bool()>& handedOver,
         const std::function<void()>& openGamesFolder);

}  // namespace ui
