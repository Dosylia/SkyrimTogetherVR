# Skyrim Together VR: TODO

Plan for getting from "co-op works" to "smooth, and easy for other people to set up".
Items are ordered by priority inside each section. Details on existing bugs are in
`KNOWN_ISSUES.md`, and missing VR addresses are in `VR_POINTERS_TODO.md`.

Status tags: **[untested]** built but never checked in game · **[measure]** needs numbers
before any change · **[big]** several sessions of work.

---

## 0. First thing next session: check what is already built

Three changes were deployed and never played. They need a new server as well as the new client
on both PCs.

- [ ] **[untested] Weapon, spell and torch sync (snapshot).** Switch weapons, spells and torches in
  both hands. The other player should see each change within about 1-2 s.
  Logs: `Equipment snapshot sent` on the sender, `Equipment sync:` on the receiver.
- [ ] **[untested] Dragon and NPC churn.** A dragon or mammoth near the edge of the loaded area
  should keep its health and die for both players.
  Log: `Actor removed, form id` should no longer repeat every few seconds for the same NPC.
- [ ] **[untested] Kill and respawn.**
  - Kill an enemy that the other player "owns". Log: `Death sync:`.
  - Die and respawn. There should be no black screen. Log: `PlayerService:`.

---

## 1. Lag and stutter (top priority)

What we know:
- The mod's own per-frame update (World::Update) measures about 0.1 ms, which is negligible.
- Frames run at 22-28 ms, with spikes up to 40 ms, and the other PC runs at about 28 ms all the
  time.
- Nobody has measured the cost of the hooks that run *inside* the engine (animation update, equip,
  inventory), of logging, or of spawning remote actors. That's where the remaining suspects are.

### 1.1 Measure first [measure]
- [ ] Extend the perf logger in `World.cpp` to write a summary every 30 s:
  - average, 95th and 99th percentile frame time;
  - number of frames over 50 ms;
  - the slowest sections.
- [ ] Add PerfScopes around the engine-side hooks: the VRBodySync animation hook,
  `BehaviorVar::Patch`, the equip hooks, `SetActorInventory` and `CreateCharacterForEntity`.
- [ ] Benchmark 60 s at the same save and spot, standing still and then walking, in four setups:
  1. FUS without Skyrim Together;
  2. with Skyrim Together, not connected;
  3. connected alone to a local server;
  4. co-op.

  Keep the four summaries in this file, so every later change is compared against real numbers.
- [ ] Don't attach the debugger watcher during normal sessions. It stops the game on every thread
  start and exit (DynDOLOD alone runs 600+ threads).

### 1.2 Likely wins (do after 1.1 confirms them)
- [ ] **Logging is synchronous, and writes to a console window too.**
  - The last session logged about 26k lines, including bursts of hundreds of lines at once
    (behavior variable dumps, `Spawn Actor`, `Setting inventory`). Writing to a Windows console is
    slow and blocks the game thread.
  - Fix: switch to spdlog's async logger, stop writing to the console (or warnings only), and move
    spammy info lines to debug.
- [ ] **VRBodySync searches for bones every frame.**
  - `FindBones` walks the whole skeleton, including armour nodes, on every animation update of every
    remote player.
  - Fix: cache the 12 bone pointers per actor and only search again when the 3D root changes.
- [ ] **Spawn bursts on cell change.** Entering an area creates dozens of actors in one frame, each
  with a full inventory rebuild (remove everything, then add and equip item by item).
  - Fix: limit spawns and inventory applies to a few per frame, and queue the rest.
- [ ] **Remote movement is shown 300 ms late** (`CharacterService::RunRemoteUpdates`).
  - Fix: base the delay on measured ping and jitter, clamped to 100-200 ms. Keep the 300 ms delay
    for NPCs if they get jittery.
- [ ] `RunNakedNPCBugChecks` looks at every actor's worn items once a second. Measure it, then limit
  it to a few actors per tick.
- [ ] The new equipment snapshot reads the whole player inventory once a second, and FUS
  inventories are big. Measure it, and if needed only rebuild when an equip hook fired or the
  hand/spell pointers changed.
- [ ] Look for other per-frame costs: linear `find_if` searches over all entities in hot paths, and
  `TESForm::GetById` inside loops.
- [ ] Host PC: run the server at below-normal priority, and document that the host's VR frame rate
  drives everyone's experience.

---

## 2. Sync correctness ("NPCs and animals feel buggy")

### 2.1 Combat and NPCs
- [ ] **Remote NPCs run their AI on both clients.**
  - The hook that stops AI for NPCs owned by the other player (`Actor::Process`, AE 37356) has no VR
    address, so both copies act.
  - This is probably the biggest cause of "NPCs feel buggy".
  - Fix: find the SE id (`VR_POINTERS_TODO.md` section 1, first row).
- [ ] **NPCs don't pick targets properly in co-op.**
  - `CombatController::SortTargetSelectors` (33282) is unknown on VR, so the hook is off.
  - Result: NPCs owned by one player react badly to the other player.
  - Fix: find the SE id, then re-enable the hook.
- [ ] **Kill sync is a coin flip.** Read the `Death sync:` logs from the next session (0), then fix
  what they show.
- [ ] **Hits from VR weapons.** A VR sword hit lands on the attacker's copy of the NPC. Check that
  damage reaches the NPC's owner, and that the health change is sent back when the NPC is owned by
  the other player.
- [ ] **Projectile details missing.** `Projectile::LaunchData` has the wrong layout on VR, so remote
  arrows and spells don't carry the spell, weapon or ammo (effects and damage can be wrong).
  - Fix: reverse the VR struct from the `Projectile::Launch` callers, like the EquipData fix.
- [ ] **Dragons on the remote side.** The client grid check still passes `IsDragon = false` for
  remote entities.
  - Fix: add the dragon flag to the spawn data so remote copies get the wide range too.
- [ ] **Actor ownership warnings.** Look into `Actor for ownership transfer not found` and
  `OnNotifyActorTeleport: failed to retrieve actor` once the churn fix is confirmed.
- [ ] **Shared follower loops** (the Lydia case). A follower owned by one player gets pulled by the
  other player's game.
  - Decide the rule (the follower's owner = the player it follows), then apply it during
    ownership transfer.

### 2.2 Items and inventory
- [ ] **Item pickups by NPCs** (`Actor::PickUpObject`, 37521) are not hooked on VR.
- [ ] **Items added to actors** (`AddInventoryItem`, 37525) are not hooked on VR. It crashed with the
  wrong address, so remote copies never get new items.
  - Fix: find both SE ids.
- [ ] Gold amount (37527) is unknown, so it always reads 0.

### 2.3 VR body
- [ ] **Finger and grip pose.** Remote hands are always open. Add finger curl (a few bytes per hand)
  to `VRPose`.
- [ ] **Bow aiming and spell casting in the hands.** Check that remote casting effects and the bow
  draw show up. They are not driven by animations on VR.
- [ ] **Legs when moving.** Check that smooth locomotion plays a walk or run on the remote copy,
  rather than sliding.
- [ ] **Player height and scale** differences between VR players.
- [ ] **Face "looks off"** (FaceGen on VR). Parked earlier, still open.

### 2.4 World, quests, dialogue
- [ ] **Message boxes show up for both players** ("player 1 opens text, player 2 sees the popup").
  Find which sync sends it, then limit it to the player it belongs to.
- [ ] **Dialogue voice and subtitles.** SpeakSound (37542) and the subtitles function (52626) are off
  on VR.
- [ ] **Quest NPCs out of sync** (Bastianus Axius). Check quest sync with a party on the next test.
- [ ] **Waypoint sharing** (40535/40536) is off on VR.
- [ ] **Weather and time.** `WeatherService` and `CalendarService` exist; confirm they work on VR.
- [ ] **Shared horse.** Parked on purpose ("funny, minor"). InitiateMountPackage (37905) is left
  off.

---

## 3. Stability

- [ ] **39 VR addresses still missing** (`VR_POINTERS_TODO.md` sections 1-3).
  - Work through them in batches, ordered by the features above; each batch is one session.
  - Method: size-sequence alignment, then check the code in `code.bin` with capstone.
- [ ] **85 addresses never confirmed** (`VR_POINTERS_TODO.md` section 5: 53 matched by size, 32 from
  the old diff table). Verify them with the same static method, starting with the diff-table ones.
- [ ] **CEF crash guard audit.** Any overlay call on VR is a hard crash. Search every
  `OverlayService` / `ExecuteAsync` / `CefListValue` path and add `m_pOverlay` guards everywhere at
  once, instead of waiting for each crash.
- [ ] **Intermittent crash:** a script event sent to a freed temporary reference during cell attach
  (see `KNOWN_ISSUES.md`).
- [ ] **Papyrus stand-ins.** Replace the Papyrus-native workarounds (SetNoBleedoutRecovery,
  SetFactionRank) with the real functions once their SE ids are found.

---

## 4. Convenience: make it shareable

The goal is that a new player installs one package, edits one line, and plays, all without taking
the headset off.

### 4.1 No keyboard needed in VR
- [ ] **Auto-connect on game load** when `connect.txt` exists, so F6 is no longer needed. The F6/F7
  keys are debug keys and don't exist in a release (master) build.
- [ ] **Auto-party:** everyone on the server joins one party (server setting, on by default for small
  servers). Quest sync and ownership depend on the party, and F7 is currently the only way in.
- [ ] **HUD notifications** for connecting, connected, connection failed (with the reason), player
  joined or left, party joined, and player down. Some exist already; make them consistent.
- [ ] **Reconnect automatically** after a drop, with a HUD message.
- [ ] **[big] An in-game menu without CEF.** There is no overlay on VR, so there's no chat, player
  list or settings. Options, cheapest first:
  1. HUD messages only;
  2. a small ESP with a "Skyrim Together" power or MCM page that calls into the client
     (connect or disconnect, player list as message boxes);
  3. an OpenVR overlay for ImGui.

### 4.2 Versions and compatibility
- [ ] **Version check with a clear message.** `AuthenticationRequest` has a `Version` field; make
  sure it changes with every protocol change. Mixed builds currently break silently: the equipment
  change today changed the protocol.
  - Reject the connection with "Server is build X, you have build Y".
- [ ] **Show the build version** in the log header and in a HUD message on connect.
- [ ] **Mod list comparison on connect.** The server already receives every client's plugin list.
  Send back the host's plugins that a player is missing, and show "N plugins differ, see log".
  This explains "Failed to retrieve Actor X, possibly missing mod".
- [ ] **Startup checks with plain-language HUD errors:**
  - uGridsToLoad = 5;
  - SKSE VR loaded;
  - VR Address Library present;
  - fewer than 255 plugins;
  - `SkyrimTogether.esp` with the 1.70 header.

### 4.3 Packaging and install
- [ ] **One release zip** with the client, its DLLs, `SkyrimTogether.esp`, the server, a default
  `STServer.ini`, `VR_MULTIPLAYER_GUIDE.md`, and a `connect.txt` template.
- [ ] **Install script** (PowerShell):
  - finds the MO2 instance and copies the tool into it;
  - adds the MO2 executable entry;
  - creates `%LOCALAPPDATA%\SkyrimTogetherVR\connect.txt` from a template on first run;
  - checks the requirements listed in 4.2.
- [ ] **Update script:** replace the files with the rename-aside trick, so MO2 never needs closing.
  Optionally check for a newer GitHub release.
- [ ] **Host script:** start the server with the right working directory, and print the public
  IP:port to give to friends.
- [ ] **Log collector:** a `collect-logs.bat` that zips `tp_client.log`, the server log, the newest
  crash log and the build version. Players send one file.
- [ ] **Guide for non-FUS modlists:** what's required, what's known to conflict, and the plugin
  limit.
- [ ] **Licensing before sharing.** Tilted Online is GPL-3: publish the source for every shared
  build, and keep the upstream license and credits. Check the Skyrim Together Reborn project's rules
  on redistributing forks.

---

## 5. Polish

- [ ] Remote player name above the head, or at least a HUD message with the name when near.
- [ ] Clean remote spawn: fade in instead of popping or falling. Place on the ground if the
  interpolated Z is invalid ("mammoth fell from the sky").
- [ ] Death and bleedout: HUD message "X is down", and optionally revive by activating the downed
  player.
- [ ] Sensible defaults for VR in `STServer.ini` (difficulty sync, PvP off, time scale).
- [ ] Trim the log to what's useful for a bug report: no per-variable behavior dumps at info level.

---

## 6. Working method (to cut the "restarted hundreds of times" cost)

- [ ] **A test script per build:** a short checklist of scenarios to play, and which log lines
  prove each one, handed over together with each deploy.
- [ ] **Batch several fixes per test session** (as agreed). Keep each change's log lines so a
  single session answers every question.
- [ ] **[big] A test bot client:** a small headless tool that connects to the server and replays
  movement, equips and attacks. Much of the sync could then be tested with one headset, without
  waiting for the friend.
- [ ] Send the new client to the friend with every deploy, and remember the server needs updating
  whenever the protocol changes.
- [ ] No debugger attached by default (see 1.1).
