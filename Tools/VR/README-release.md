# urSovngarde

Co-op for Skyrim VR, a port of Skyrim Together Reborn.

The easiest way in is the urSovngarde launcher: it installs this, keeps it updated, starts the game and hosts or
joins with a code. To do it by hand, pick the file for your situation:

- **`README-mod-manager.md`**: you use Mod Organizer 2 or a Wabbajack list (FUS, and the like).
- **`README-vortex.md`**: you use Vortex.
- **`README-manual.md`**: no mod manager at all.
- **`README-host.md`**: you are the one running the server.

The folders: `urSovngarde mod` holds the game files (a mod for your mod manager), `urSovngarde` holds the program
that starts the game (`urSovngarde.exe`), and `Server` holds the server (`urSovngardeServer.exe`). Before the rename
of 9 October 2026 they were `Skyrim Together mod`, `Skyrim Together VR`, `SkyrimTogetherVR.exe` and
`SkyrimTogetherServer.exe`; `SkyrimTogether.esp` keeps its name, as saves depend on it.

Everyone needs Skyrim VR 1.4.15, SKSE VR and the VR Address Library for SKSEVR, and a build that matches the
others' (the server says when it does not). Problems: run `collect-logs.bat` in the `urSovngarde` folder and post
the zip it puts on your Desktop.

Skyrim Together Reborn is by Tilted Phoques and is GPLv3; this port inherits that licence. VR address
research from the TiltedEvolutionVR project. Source: https://github.com/Dosylia/SkyrimTogetherVR
Neither upstream project is responsible for this port.
