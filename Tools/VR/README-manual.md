# Skyrim Together VR without a mod manager, or with Vortex

1. Open the folder `Skyrim Together mod` and copy everything in it into `Skyrim VR\Data`, next to
   `Skyrim.esm`. Vortex users: zip that folder and install it as a mod instead. Enable
   `SkyrimTogether.esp` in your plugin list (the Mods screen in the game, or Vortex's plugins tab).
2. Put the folder `Skyrim Together VR` anywhere, for example inside `Skyrim VR`.
3. Start the game with `SkyrimTogetherVR.exe` from that folder, not with SKSE. The first start asks
   where Skyrim VR is installed.
4. Run `setup-connect.bat` once in the `Skyrim Together VR` folder and type the host's address.
   If you host yourself, type `127.0.0.1:10578`.
5. Start the game, load a save. It connects by itself a few seconds later.

You still need SKSE VR and the VR Address Library for SKSEVR installed in `Data`, like any SKSE mod.
`uGridsToLoad` must be 5 in `SkyrimPrefs.ini`, which is the default.

Updating: close the game and drag the update zip onto `update.bat` in the `Skyrim Together VR` folder.
(By hand: replace `SkyrimTogetherVR.exe` and `SkyrimTogetherVR.pdb` there.)
