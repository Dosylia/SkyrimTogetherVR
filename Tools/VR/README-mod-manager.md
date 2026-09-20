# Skyrim Together VR with Mod Organizer 2 or a Wabbajack list

1. Install the folder `Skyrim Together mod` as a mod in MO2 (drag it onto the mod list, or zip it and
   use "Install a new mod"). Tick it. Tick `SkyrimTogether.esp` in the plugin list on the right.
2. Copy the folder `Skyrim Together VR` into your list's `tools\` folder (or anywhere you like).
3. In MO2, add an executable: title `Skyrim Together VR`, binary `SkyrimTogetherVR.exe` from that folder.
   Start the game with it from now on, not with SKSE. Your mods load exactly as before.
4. Run `setup-connect.bat` once in the `Skyrim Together VR` folder and type the host's address.
   If you host yourself, type `127.0.0.1:10578`.
5. Start the game through MO2, load a save. It connects by itself a few seconds later.

`uGridsToLoad` must be 5, which is the default of every list. The server refuses other values.

Updating: close the game and drag the update zip onto `update.bat` in the `Skyrim Together VR` folder. It swaps the
files without closing MO2. (By hand: replace `SkyrimTogetherVR.exe` and `SkyrimTogetherVR.pdb` there.)
