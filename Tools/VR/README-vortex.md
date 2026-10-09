# urSovngarde with Vortex

The urSovngarde launcher does this for you, and with Vortex 1.14 or newer it installs the mod as a Vortex mod.
By hand:

Vortex works. It writes mods straight into the game's `Data` folder (hardlinks by default), so the
game sees them without going through Vortex at all. Mod Organizer 2 is only what the developers
tested on.

1. Install the usual SKSE mods through Vortex like any other mod: SKSE VR and VR Address Library for
   SKSEVR (required), and VRIK (recommended, it is the body your friend sees). Engine Fixes VR is
   optional: urSovngarde does not need it (its program raises the open-file limit itself), but its
   general crash fixes help a big modlist. Engine Fixes 7.x needs its "Part 2" preloader in the SkyrimVR
   folder; urSovngarde switches off two of its settings that black out the view under it (it writes them
   into `EngineFixesCustom.toml`). If the game misbehaves with it anyway, leave it off. Both
   players should run the same list; different setups are the number one cause of "he sees a bear, I see
   a wolf".
2. Zip the folder `urSovngarde mod` (right click, Send to, Compressed folder), then in Vortex go to
   Mods, Install From File, pick that zip, and enable the mod. In the Plugins tab enable
   `SkyrimTogether.esp`. Deploy if Vortex asks.
3. Put the folder `urSovngarde` anywhere, for example next to `SkyrimVR.exe`. Not inside `Data`.
4. Run `setup-connect.bat` in that folder once and type the host's address. If you host yourself, type
   `127.0.0.1:10578`.
5. Start the game with `urSovngarde.exe` from that folder: double-click it, or add it as a tool on
   the Vortex dashboard (Add Tool, point it at the exe) so you can launch from Vortex. Never start the
   SKSE loader directly: urSovngarde starts the game itself and loads SKSE for you. The first start asks
   where Skyrim VR is installed.
6. Load a save. It connects by itself a few seconds later.

`uGridsToLoad` must be 5 in `SkyrimPrefs.ini`, which is the default. The server refuses other values.

Two Vortex things to know:

- Purge in Vortex removes every deployed mod from `Data`, the co-op mod included. Deploy again before playing.
- Vortex needs the game and its staging folder on the same drive for hardlinks. That is a Vortex rule,
  not ours; if Vortex complains about it, fix that first.

Updating: close the game and drag the update zip onto `update.bat` in the mod's folder.
(By hand: replace `urSovngarde.exe` and `urSovngarde.pdb` there.)

Coming from a build before the rename (the program was `SkyrimTogetherVR.exe`): point your Vortex tool at
`urSovngarde.exe`. `update.bat` keeps the old name working with the new build in the meantime.
