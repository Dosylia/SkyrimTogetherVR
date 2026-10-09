# urSovngarde without a mod manager

Vortex users: see `README-vortex.md` instead. The urSovngarde launcher does all of this for you. By hand:

1. Open the folder `urSovngarde mod` and copy everything in it into `Skyrim VR\Data`, next to
   `Skyrim.esm`. Enable `SkyrimTogether.esp` in your plugin list (the Mods screen in the game).
2. Put the folder `urSovngarde` anywhere, for example inside `Skyrim VR`.
3. Start the game with `urSovngarde.exe` from that folder, not with SKSE. The first start asks
   where Skyrim VR is installed.
4. Run `setup-connect.bat` once in the `urSovngarde` folder and type the host's address.
   If you host yourself, type `127.0.0.1:10578`.
5. Start the game, load a save. It connects by itself a few seconds later.

You still need SKSE VR and the VR Address Library for SKSEVR installed in `Data`, like any SKSE mod.
`uGridsToLoad` must be 5 in `SkyrimPrefs.ini`, which is the default.

Updating: close the game and drag the update zip onto `update.bat` in the mod's folder.
(By hand: replace `urSovngarde.exe` and `urSovngarde.pdb` there.)

Coming from a build before the rename (the program was `SkyrimTogetherVR.exe`): start `urSovngarde.exe` from now
on. `update.bat` keeps the old name working with the new build in the meantime.
