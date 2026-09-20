# Skyrim Together VR with Vortex

Vortex works. It writes mods straight into the game's `Data` folder (hardlinks by default), so the
launcher sees them without going through Vortex at all. Mod Organizer 2 is only what the developers
tested on.

1. Install the usual SKSE mods through Vortex like any other mod: SKSE VR, VR Address Library for
   SKSEVR, Engine Fixes VR, and VRIK (recommended, it is the body your friend sees). Both players
   should run the same list; different setups are the number one cause of "he sees a bear, I see a wolf".
2. Zip the folder `Skyrim Together mod` (right click, Send to, Compressed folder), then in Vortex go to
   Mods, Install From File, pick that zip, and enable the mod. In the Plugins tab enable
   `SkyrimTogether.esp`. Deploy if Vortex asks.
3. Put the folder `Skyrim Together VR` anywhere, for example next to `SkyrimVR.exe`. Not inside `Data`.
4. Run `setup-connect.bat` in that folder once and type the host's address. If you host yourself, type
   `127.0.0.1:10578`.
5. Start the game with `SkyrimTogetherVR.exe` from that folder: double-click it, or add it as a tool on
   the Vortex dashboard (Add Tool, point it at the exe) so you can launch from Vortex. Never start the
   SKSE loader directly: the launcher starts the game itself and loads SKSE for you. The first start asks
   where Skyrim VR is installed.
6. Load a save. It connects by itself a few seconds later.

`uGridsToLoad` must be 5 in `SkyrimPrefs.ini`, which is the default. The server refuses other values.

Two Vortex things to know:

- Purge in Vortex removes every deployed mod from `Data`, the co-op mod included. Deploy again before playing.
- Vortex needs the game and its staging folder on the same drive for hardlinks. That is a Vortex rule,
  not ours; if Vortex complains about it, fix that first.

Updating: close the game and drag the update zip onto `update.bat` in the `Skyrim Together VR` folder.
(By hand: replace `SkyrimTogetherVR.exe` and `SkyrimTogetherVR.pdb` there.)
