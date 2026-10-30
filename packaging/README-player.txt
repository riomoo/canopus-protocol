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
     Linux:   run ./canopus-protocol (needs SDL2: for example
              "sudo apt install libsdl2-2.0-0")

   The first time, it reads your game files and builds the game from them.
   That takes a few minutes (it shows its progress). After that, the game
   starts straight away. If you add the update or DLC later, it rebuilds
   by itself the next time you start it.

   Linux: "./canopus-protocol --install-desktop" adds Canopus Protocol to
   your applications menu with its icon.

CONTROLS

  +Control Pad      Arrow keys
  A / B             X / Z
  X / Y             S / A
  L / R             Q / W
  START / SELECT    Enter / Backspace
  Circle Pad        I J K L
  Touch screen      Left mouse button on the bottom screen
  Cheat menu        F1

THE DLC

Each DLC battle arrives as a mail in the Download folder (X, Mail, then
Right twice) once the story opens that folder. Read a mail to the end and
answer Yes to fight it.

OPTIONS

Add these after the program's name (or to a shortcut):
  --no-vsync        don't wait for the monitor (may tear)
  --smooth          smooth scaling instead of sharp pixels
  --output gdi      Windows: draw with GDI instead of OpenGL
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
