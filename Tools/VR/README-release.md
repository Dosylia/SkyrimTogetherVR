# Skyrim Together VR

An experimental port of Skyrim Together Reborn to Skyrim VR. Co-op for two players.

**Read the Discord install channels before starting.** This file is a reminder of what's in the
folder, not a full guide.

---

## What's in here

| Folder | Who needs it | What to do with it |
| --- | --- | --- |
| `Skyrim Together VR` | Everyone | The client. Put it in your modlist's `tools\` folder and add `SkyrimTogetherVR.exe` to Mod Organizer 2 as an executable. |
| `Skyrim Together mod` | Everyone | Install as a mod in MO2 and tick it, **and** tick `SkyrimTogether.esp` in the plugin list. |
| `Server` | The host only | Run `host-server.bat` before playing. Leave the console open. |

## Requirements

- Skyrim VR **1.4.15** (the only Steam version), launched through **Mod Organizer 2**.
- **SKSE VR**.
- **VR Address Library for SKSEVR** — required. The client reads its
  `Data/SKSE/Plugins/version-1-4-15-0.csv` at startup and does nothing without it.
- **Engine Fixes VR**, and **Skyrim VR ESL Support** if your list is near 255 plugins.
- `uGridsToLoad=5` in your INI. The server refuses any other value.
- **Both players on the same build**, and as close to the same modlist as you can manage.

## Playing

1. **Host:** run `Server\host-server.bat`. It prints the port (UDP 10578). Either forward that port
   in your router or put both players on the same virtual LAN (Radmin VPN, ZeroTier, Tailscale).
2. **Everyone:** run `setup-connect.bat` in the client folder and enter the address. The host uses
   `127.0.0.1:10578`; everyone else uses the host's address.
3. **Everyone:** pick **SkyrimTogetherVR** in the MO2 dropdown, run it, load a save. It connects on
   its own about five seconds later and puts you in the same party.
4. **F6** disconnects or reconnects.

## When something goes wrong

Run **`collect-logs.bat`** in the client folder. It puts one zip on your Desktop with the client log
and any crash log and dump from the last day. Post it on the Discord with what you were doing.

## Updating

Close the game, replace `SkyrimTogetherVR.exe` and `SkyrimTogetherVR.pdb` with the new ones, done.
Both players, every time — mismatched builds are refused at connect.

## Licence

Skyrim Together Reborn is by Tilted Phoques and is GPLv3; this port inherits that licence. VR address
research from the TiltedEvolutionVR project. If you want the source for the build you have, ask on
the Discord and you'll get it. Neither upstream project is responsible for this port.
