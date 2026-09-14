# Skyrim Together VR: playing guide

How to host and join a Skyrim Together VR session with the FUS modlist. The **host** runs the
server and plays; the **friend** only plays.

Paths below are the host's PC. On the friend's PC, only the MO2 and Skyrim VR folders differ.

---

## 1. The pieces

| What | Who needs it | Path (host PC) |
|---|---|---|
| Server | Host only | `C:\dev\TiltedEvolution\build\windows\x64\release\SkyrimTogetherServer.exe` |
| Server settings | Host only | `C:\dev\TiltedEvolution\build\windows\x64\release\config\STServer.ini` |
| Server log | Host only | `C:\dev\TiltedEvolution\build\windows\x64\release\logs\STServerOut.log` |
| Game client folder | Everyone | `E:\FUS\tools\Skyrim Together VR\` |
| Game client | Everyone | `E:\FUS\tools\Skyrim Together VR\SkyrimTogetherVR.exe` |
| Client log | Everyone | `E:\FUS\tools\Skyrim Together VR\logs\tp_client.log` |
| Connect file | Everyone | `%LOCALAPPDATA%\SkyrimTogetherVR\connect.txt` |
| Crash dumps | Everyone | `E:\FUS\overwrite\Root\crash_UTC_*.dmp` or `<SkyrimVR game folder>\crash_UTC_*.dmp` |
| Crash Logger logs | Everyone | `C:\Users\<name>\Documents\My Games\Skyrim VR\SKSE\crash-*.log` |

`%LOCALAPPDATA%` is `C:\Users\<name>\AppData\Local`. You can paste `%LOCALAPPDATA%\SkyrimTogetherVR`
straight into the Explorer address bar.

---

## 2. Host: start the server

1. Double-click `C:\dev\TiltedEvolution\build\windows\x64\release\SkyrimTogetherServer.exe`.
2. A console window opens with `Server ... started on port 10578`. **Leave it open** for the whole
   session; closing it ends the server.
3. Run **only one** server. Windows 11 may open it as a tab in an existing Terminal window. If you
   see two server tabs, close one.
4. When the server has been updated (a new `STServer.dll` in that folder), close it and start it
   again.

When someone joins, the console (and `STServerOut.log`) prints
`New player '<name>' ... connected`. That is the quickest way to confirm a connection reached the
server.

### Server settings (`config\STServer.ini`)

Edit these only while the server is closed.

| Setting | Current | Meaning |
|---|---|---|
| `uPort` | `10578` | UDP port players connect to |
| `sPassword` | *(empty)* | Set one to keep strangers out. Players then put it on line 2 of `connect.txt` |
| `bAutoPartyJoin` | `true` | Players join a party automatically (needed for weather and quest sharing) |
| `bEnablePvp` | `false` | Players can't damage each other |
| `bEnableDeathSystem` | `true` | Death respawns you at a temple instead of loading a save |
| `bAllowMO2` / `bAllowSKSE` | `true` | Must stay on for this modlist |
| `bEnableModCheck` | `false` | Keep off; the modlists don't need to match byte for byte |

---

## 3. Host: network (one-time setup)

Already done on the host's PC; repeat these steps if the router or PC changes.

1. **Router (Orange Livebox)**, `http://192.168.1.1`:
   - **Network → DHCP → Static leases:** `DESKTOP-5JBCN0L` → `192.168.1.10`.
   - **Network → NAT/PAT:** internal port `10578`, external port `10578`, protocol **UDP**, device
     `DESKTOP-5JBCN0L`, external IP **Toutes** (all). Make sure the device is picked from the
     dropdown and isn't typed by hand.
   - **Firewall level:** `Moyen` is fine.
2. **Windows Firewall:** run once in **Terminal (Admin)**:
   ```powershell
   New-NetFirewallRule -DisplayName "Skyrim Together Server (UDP 10578)" -Direction Inbound -Protocol UDP -LocalPort 10578 -Action Allow -Profile Any
   ```
3. **Public IP:** open https://api.ipify.org. At the time of writing it is `86.248.47.218`. Orange can
   change it after a Livebox restart; if the friend suddenly can't connect, check it again.

---

## 4. Everyone: set up `connect.txt`

Create the folder `%LOCALAPPDATA%\SkyrimTogetherVR\` if it doesn't exist, then a plain text file
`connect.txt` inside it:

| Who | Line 1 | Line 2 |
|---|---|---|
| Host (server on the same PC) | `127.0.0.1:10578` | server password, if one is set |
| Friend | `86.248.47.218:10578` (host's public IP) | server password, if one is set |

Just the address, nothing else: no `http://` and no quotes.

---

## 5. Friend: install the client (one-time)

1. Copy the host's whole `E:\FUS\tools\Skyrim Together VR\` folder into the friend's own FUS
   `tools\` folder.
2. In the friend's **MO2**: **Executables** (gear icon next to *Run*) → **+** → *Add from file* →
   select `SkyrimTogetherVR.exe` in that folder → **Apply**.
3. Create `connect.txt` (section 4).
4. Keep the modlist as close to the host's as possible (same FUS version and the same optional
   mods).

---

## 6. Play a session

1. **Host:** start the server (section 2).
2. **Everyone:** in MO2, pick **SkyrimTogetherVR** in the executable dropdown and click **Run**.
   Launching can take a while with this modlist.
3. Load your save and wait until you're fully in the world.
4. Connect, either way:
   - **In the headset:** press the controller's **system button** to open the SteamVR dashboard, and pick
     the **Skyrim Together** tab at the bottom. Use the laser pointer for the menu; text fields open the
     SteamVR keyboard.
   - **On the keyboard:** press **F6**. It uses `connect.txt`.

   A notification shows `Skyrim Together: connected to server`.
5. Disconnect from the same menu, or with **F6**.

---

## 7. Updating to a new build

When a new client build is ready:

1. **Close the game first.**
2. Replace `SkyrimTogetherVR.exe` (and `SkyrimTogetherVR.pdb`) in the `Skyrim Together VR` tools
   folder. When the build notes say the menu changed, also replace the `UI` folder and `TPProcess.exe`.
3. **Everyone must run the same build.** The friend replaces their exe too. Compare the file's
   *Date modified*.
4. If the build notes say the server changed, restart the server as well.

---

## 8. Troubleshooting

### "Connecting" then immediately disconnected

In `tp_client.log`, find the last line `Disconnected from server N`:

| N | Meaning | Fix |
|---|---|---|
| `0` | Timeout: nothing answered | Server not running, wrong public IP, router rule or firewall (section 3). Check whether the server console printed anything |
| `3` | Address couldn't be read | Typo or extra characters in `connect.txt` |

### The game crashes

Send these to the developer (zip the `.dmp`; it shrinks from about 100 MB to about 13 MB):

1. `tp_client.log` from the tools folder's `logs\`.
2. The newest `crash_UTC_*.dmp` (MO2 `overwrite\Root\` or the Skyrim VR game folder). The name uses
   UTC time: a crash at 17:52 in France is `..._15-52-..`.
3. The matching Crash Logger file `crash-*.log` from `Documents\My Games\Skyrim VR\SKSE\`.

### Stutter

The client logs `Perf spike: frame X ms, mod update Y ms, slowest mod section Z` in `tp_client.log`.
Send the log after a stuttery session:

- A small *mod update* means the hitch comes from the game or another mod.
- A large one names the part of Skyrim Together to optimise.

### Known behaviour (not bugs to report)

- **Shared followers:** if both saves have the same follower (e.g. Lydia), the two games fight over
  them. Dismiss the follower in one save.
- **Horses:** two players can end up on one horse, and only one can steer.
- **Party dialogue:** the other player sees NPC dialogue text when you talk.
- **Working NPCs:** NPCs at work spots (woodcutting, forges) aren't fully synced.
- **Weather:** the party leader's weather is pushed to members when it changes.
