CANOPUS PROTOCOL
Unofficial PC port of Shin Megami Tensei: Devil Survivor 2 Record Breaker

This download contains no game. It turns your own copy of the game into a
PC version, on your computer.

GETTING STARTED

1. Put a decrypted dump of your game (.3ds, .cci or .cia) in the "games"
   folder. Make one from your own 3DS with GodMode9. The supported version
   is the North American release.
   If you have the v1.1 update and the DLC, put their .cia files in
   "games" too.

2. Start Canopus Protocol:
     Windows: double-click "Canopus Protocol.exe"
     Linux:   run ./canopus-protocol

   The first time, it reads your game files and builds the game from them,
   in its window; then the game opens in the same window.
   That takes a few minutes (it shows its progress). After that, the game
   starts straight away. If you add the update or DLC later, it rebuilds
   by itself the next time you start it.

   Linux: "./canopus-protocol --install-desktop" adds Canopus Protocol to
   your applications menu with its icon.

CONTROLS

                    Keyboard            Controller
  +Control Pad      Arrow keys          D-pad
  A / B             X / Z               A / B
  X / Y             S / A               X / Y
  L / R             Q / W               LB / RB
  START / SELECT    Enter / Backspace   Start / Back
  Circle Pad        I J K L             Left stick
  Touch screen      Left mouse button   Right stick moves a pointer;
                    on the bottom       right trigger or R3 touches
                    screen
  Cheat menu        F1                  L3 + R3 (press both sticks in)
  Full screen       F11 or Alt+Enter

The controller buttons go by an Xbox pad's labels (bottom button = A).
With a Nintendo-style pad add --nintendo-buttons (see OPTIONS) to use its
own labels.

STEAM INPUT AND STEAM DECK

In Steam: Games > Add a Non-Steam Game to My Library, and pick
"Canopus Protocol.exe" (Windows) or canopus-protocol (Linux). Steam then
hands the game your controller (or the Deck's controls) as an Xbox pad,
and you can remap buttons in Steam's controller settings. Use the
"Gamepad" controller template.

THE DLC

Each DLC battle arrives as a mail in the Download folder (X, Mail, then
Right twice) once the story opens that folder. Read a mail to the end and
answer Yes to fight it.

OPTIONS

Add these after the program's name (or to a shortcut):
  --no-vsync        don't wait for the monitor (may tear)
  --smooth          smooth scaling instead of sharp pixels
  --nintendo-buttons  controller A/B/X/Y by a Nintendo pad's labels
  --no-backdrop     black around the game screens instead of the backdrop
  --rebuild         build the game again from your files

FILES IT MAKES

  data/                 your game data and saves (data/save)
  cache/                the game built from it (safe to delete: it is
                        rebuilt the next time)
  ds2rb.log             the game's log: include it when reporting a problem
  canopus-setup.log     what the first-time setup did

Everything made from your game stays on your computer: don't share data/
or cache/.

Canopus Protocol is a fan project, not affiliated with Atlus or Sega.
tools/zig is the Zig compiler (ziglang.org), with its licence in licenses/.
