## urSovngarde

Co-op for Skyrim VR: a port of Skyrim Together Reborn (Tilted Online) to Skyrim VR 1.4.15. Playable, and played
by the two of us; still early, and only ever tested with two players at once.

## What works

- Playing together in Skyrim VR through a dedicated server, the world's NPCs, fights, deaths and items shared.
- The other player's body follows their real head and hands (VRIK), with their weapons held where they hold them.
- Sword against sword: both players feel it in the controller, hear it and see sparks, and a hit is dropped when the
  defender's blade met the attacker's just before it landed. Only a weapon or a shield parries.
- Dead bodies lie in the same place in both worlds and can be dragged, seen by both.
- A player's follower stays with that player's game; a dragon flying in one game is not taken over by the other
  until it lands.
- Builds that speak the same network messages connect to each other whatever their version.
- The urSovngarde launcher: finds Skyrim VR and the mod manager (Mod Organizer 2, Vortex or none), installs and
  updates the mod, checks the setup, plays, hosts with a six-letter invite code and joins with one, through our relay
  so that no port has to be opened. Crash reports are sent only with the player's consent.

## Known issues

- Blades do not physically stop each other yet: an equipped sword passes through the other one (only feedback and
  the parry rule exist).
- Some NPC and quest situations still differ between the two games (an NPC invisible for one player, quest
  dialogue heard twice). `VR_TODO.md` and `KNOWN_ISSUES.md` hold the full list.

Needed: Skyrim VR 1.4.15, SKSE VR, the VR Address Library for SKSEVR, and `uGridsToLoad = 5` (the default). The
release's `README.md` explains the install with the launcher, Mod Organizer 2, Vortex or by hand.

## Building

On Windows 10 or 11 (x64), the same steps the Windows build on GitHub Actions takes (`.github/workflows/windows.yml`).

Needed: Visual Studio 2022 with "Desktop development with C++", [xmake](https://xmake.io) 3.1.0 or newer, Git, and
Node.js 20 with pnpm for the in-game menu.

```
git clone --recursive https://github.com/Dosylia/SkyrimTogetherVR.git
cd SkyrimTogetherVR
xmake config --plat=windows --arch=x64 --mode=release --yes
xmake -y
```

The programs land in `build\windows\x64\release`: `urSovngarde.exe` (the client, which starts the game),
`TPProcess.exe` (the in-game menu's browser), `urSovngardeServer.exe` and `STServer.dll` (the server).
`xmake install -o distrib` gathers them with the files they need at run time (the Chromium Embedded Framework).

The in-game menu (`Code/skyrim_ui`), into `Code/skyrim_ui/dist/UI`:

```
pnpm --prefix Code/skyrim_ui install
pnpm --prefix Code/skyrim_ui deploy:production
```

The game files (`SkyrimTogether.esp` and the rest) are in `GameFiles/Skyrim` and install as a mod. A release as
published is packaged by `Tools/VR/make-release.ps1`.

## Original project
All core multiplayer logic is from Skyrim Together Reborn by the TiltedPhoques team.

## License

[![GNU GPLv3 Image](https://www.gnu.org/graphics/gplv3-127x51.png)](http://www.gnu.org/licenses/gpl-3.0.en.html)

Tilted Online is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
