# Skyrim Together VR: TODO

Plan for getting from "co-op works" to "smooth, and easy for other people to set up".
Items are ordered by priority inside each section. Details on existing bugs are in
`KNOWN_ISSUES.md`, and missing VR addresses are in `VR_POINTERS_TODO.md`.

Status tags: **[untested]** built but never checked in game · **[measure]** needs numbers
before any change · **[big]** several sessions of work.

---

## 0. First thing next session: check what is already built

Built on 2026-09-14 and not played yet. Both PCs need the new `SkyrimTogetherVR.exe`; the host restarts the
server (new `STServer.dll`, with `host-server.bat`). No protocol change.

- [ ] **[untested] Auto-connect and auto-party.** Load a save with `connect.txt` in place: about 5 s later
  `Skyrim Together: connecting to ...` then `connected (build ...)`, and both players in one party with no
  key pressed. Log: `VRConnectService:`, and on the server `No party on the server, creating one`.
- [ ] **[untested] Reconnect.** Close the server mid-session: `connection lost, trying again in 5 s`, then it
  reconnects once the server is back. F6 disconnects without retrying.
- [ ] **[untested] No false "connection failed" on load.** Every save load used to raise a
  `non_default_install` connection error (the vanilla plugin list check), which showed as a failed
  connection. It is off on VR. A wrong `uGridsToLoad` is reported on the HUD before connecting.
- [ ] **[untested] Plugin differences** are listed in the server log when a player joins:
  `Plugins differ between 'A' and 'B'`.
- [ ] **[untested] Menus don't pause the game while connected** (inventory, magic, skills,
  containers, favorites, console), like on flat Skyrim Together. Open the inventory: the world keeps
  moving and the other player doesn't see you frozen. Message boxes still pause. The intro movie is
  skipped. Address found by the friend (favorites) and TiltedEvolutionVR, checked in the VR code.
- [ ] **[untested] VRIK menu no longer opens for the other player.** Open VRIK's settings (its power): only
  you get the menu. Spells and effects from `vrik.esp`, `Arctals VRIK Tweaks.esp`, `higgs_vr.esp` and
  `SpellWheelVR.esp` aren't synced (idea from TiltedEvolutionVR). Probably the "popup shows for both players".
- [ ] **[untested] Killed NPCs stay dead.** Kill an NPC the other player owns: it must not stand back up or
  stay red on the compass. Replayed animations no longer overwrite the life state of a dying or dead body
  (measured bug in TiltedEvolutionVR), and corpses can rotate while they fall.
- [ ] **[untested] Upstream fixes ported:** dragons spawning for party members (server range check), NPC
  dialogue synced when the talking player doesn't own the NPC (reads `MenuTopicManager` at SE offsets,
  unverified on VR), respawn timers on a steady clock (camera stuck after a stuttery respawn), the Slow
  effect syncs again, and no auto-party on a public server.
- [ ] **[untested] Weapons at spawn.** Don't switch anything; the other player should see your weapon and
  spells as soon as you appear. Log on the viewer: `Equipment sync: remote actor ... equips` right after
  `Applied 3D for actor`.
- [ ] **[untested] VR menu.** Open the Skyrim Together dashboard tab. It should show the UI on a dark panel.
  Log: `VRDashboard: page frame sent to SteamVR (result 0), N% of the page has content`. If it's still empty,
  that number says whether the page drew anything.
- [ ] **[untested] No crash when quitting the game.** Quit normally: no `crash occurred` at the end of
  `tp_client.log`, and no new `crash_UTC_*.dmp` in `overwrite\Root`.
- [ ] **Spells leave the hand at an offset.** A temporary `CastDiag` log records the magic node position
  against the posed hand for the first remote casts. Read it before changing anything.
- [ ] **[untested] Dragon and NPC churn.** A dragon or mammoth near the edge of the loaded area
  should keep its health and die for both players.
  Log: `Actor removed, form id` should no longer repeat every few seconds for the same NPC.
- [ ] **[untested] Kill and respawn.**
  - Kill an enemy that the other player "owns". Log: `Death sync:`.
  - Die and respawn. There should be no black screen. Log: `PlayerService:`.
  - A killed NPC should fall (ragdoll) on both screens, even when killed far from the other player.
- [ ] **[untested] Sync hooks enabled from the TiltedEvolutionVR address table.** Remote NPCs should no
  longer act on both clients (no double attacks, no NPC walking off on one screen). Also check item
  pickups, dialogue voice and subtitles, map markers, summons, and waiting or sleeping.
- [ ] **Remote player's face out of the helmet + body flicker, in one interior only** (Gallows Rock, cell
  `15273`). Fine outdoors, and present on the `8670d4c4` build too, so not a regression.
  Next: check whether other interiors do it, then compare what that cell has before touching code.
- [ ] **Read the new `Perf last 30 s:` lines** from a normal co-op session and write the numbers into
  section 1.1.

### From TiltedEvolutionVR and upstream, not done yet (decisions)

- [ ] **[big] Body sync on one thread, at frame end.** TiltedEvolutionVR measured that writing remote bones
  from the animation job pool (what `VRBodySync` does) lets the renderer read half-written bones: flicker and
  black bands. It poses from `BSGraphics` frame end (id 77246, call at +0x15 on VR, verified in `code.bin`
  and already hooked by another plugin, so a SwapCall would chain), writes the skeleton's flattened bone
  array too (skinning reads `BSFlattenedBoneTree` +0x158, 0x80 per bone, not the nodes; facial bones only
  exist there, which is how a turned head "leaves the face behind"), and skips actors outside a 50 degree
  view cone because posing a culled actor tears its skin. Interiors cull by room, which fits the Gallows
  Rock-only glitch. Our own implementation, several sessions of work.
- [ ] **Shouts and powers reach the other player.** Our spell sync drops everything but concentration spells,
  so a shout is never replayed. TiltedEvolutionVR fixed it (merged): read `SpellItem` spell type (the
  `unk6C[3]` block is costOverride, flags, spellType) and replay POWER/LESSER_POWER/VOICE_POWER, resolving
  voice casts from the sent form id. Check first whether shouts are missing for the other player.
- [ ] **[big] Upstream ownership rework** (versioned server grants, 8 commits, protocol change). Fixes
  former owners overwriting an NPC and ownership blacklists that never expire, the likely cause of the
  shared follower tug of war. A dry run conflicts in exactly our VR files (`Actor.cpp`, `CharacterService`,
  `InventoryService`, `NotifyEquipmentChanges`). The pickpocket inventory fix depends on it.
- [ ] Upstream per-dungeon respawn positions (needs cell editor IDs, which VR may not keep) and the
  Companions "Brotherhood" quest patch plugin (needs the 1.70 header and an MO2 slot). Low value for now.

---

## 1. Lag and stutter (top priority)

What we know:
- The mod's own per-frame update (World::Update) measures about 0.1 ms, which is negligible.
- Frames run at 22-28 ms, with spikes up to 40 ms, and the other PC runs at about 28 ms all the
  time.
- Nobody has measured the cost of the hooks that run *inside* the engine (animation update, equip,
  inventory), of logging, or of spawning remote actors. That's where the remaining suspects are.

### 1.1 Measure first [measure]
- [x] **[untested]** `World::ReportPerformance` writes a summary every 30 s: average, 95th and 99th
  percentile frame time, frames over 50 ms, mod update average and max.
- [x] **[untested]** Thread-safe counters (`PerfCounterScope`) around the VRBodySync pose apply,
  `SetActorInventory` and `CreateCharacterForEntity`, reported in the same line. Still to add if the
  numbers point there: `BehaviorVar::Patch` and the equip hooks.
- [ ] Benchmark 60 s at the same save and spot, standing still and then walking, in four setups:
  1. FUS without Skyrim Together;
  2. with Skyrim Together, not connected;
  3. connected alone to a local server;
  4. co-op.

  Keep the four summaries in this file, so every later change is compared against real numbers.
- [ ] Don't attach the debugger watcher during normal sessions. It stops the game on every thread
  start and exit (DynDOLOD alone runs 600+ threads).

### 1.2 Likely wins (do after 1.1 confirms them)
- [x] **Logging:** the console window only gets warnings and errors, and the per-actor behavior variable
  dumps, the ambiguous-signature list (120+ lines per launch), the per-second `Perf spike` lines and the
  spawn bookkeeping lines are at debug level or replaced by the 30 s summary. The file logger stays
  synchronous on purpose: crash reports depend on it being flushed.
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
- [x] **[untested] Remote NPCs run their AI on both clients.** `Actor::Process` (37356) now has an
  address from the TiltedEvolutionVR table.
- [x] **[untested] NPCs don't pick targets properly in co-op.** `SortTargetSelectors` now resolves too.
- [x] **[untested] Kill sync is a coin flip.** Deaths now go to every player, and remote bodies play the
  death animation and ragdoll. Read the `Death sync:` logs after the next session.
- [ ] **Hits from VR weapons.** A VR sword hit lands on the attacker's copy of the NPC. Check that
  damage reaches the NPC's owner, and that the health change is sent back when the NPC is owned by
  the other player.
- [ ] **Projectile details missing.** `Projectile::LaunchData` has the wrong layout on VR, so remote
  arrows and spells don't carry the spell, weapon or ammo (effects and damage can be wrong).
  - Fix: reverse the VR struct from the `Projectile::Launch` callers, like the EquipData fix.
- [ ] **Dragons on the remote side.** The client grid check still passes `IsDragon = false` for
  remote entities.
  - Fix: add the dragon flag to the spawn data so remote copies get the wide range too. (Reading the race
    from `TESNPC` locally would avoid the protocol change, but its `raceForm` offset isn't verified on VR.)
- [ ] **Actor ownership warnings.** Look into `Actor for ownership transfer not found` and
  `OnNotifyActorTeleport: failed to retrieve actor` once the churn fix is confirmed.
- [ ] **Shared follower loops** (the Lydia case). A follower owned by one player gets pulled by the
  other player's game.
  - Decide the rule (the follower's owner = the player it follows), then apply it during
    ownership transfer.

### 2.2 Items and inventory
- [x] **[untested] Item pickups by NPCs and players** (37521, 40533) are hooked again.
- [x] **[untested] Items added to actors and containers** (37525, 19708) are hooked again.
- [x] Gold amount (37527) resolves again.

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
  No code syncs message boxes. The likely path is activation sync: the other client replays the activation
  with the remote player as activator, and the object's script shows its message. Note which object it was
  next time before changing activation sync.
- [x] **[untested] Dialogue voice and subtitles** (37542, 52626) are on again.
- [ ] **Quest NPCs out of sync** (Bastianus Axius). Check quest sync with a party on the next test.
- [x] **[untested] Waypoint sharing** (40535/40536) is on again.
- [ ] **Weather and time.** `WeatherService` and `CalendarService` exist; confirm they work on VR.
- [ ] **Shared horse.** Parked on purpose ("funny, minor"). InitiateMountPackage (37905) is left
  off.

---

## 3. Stability

- [ ] **14 VR addresses still missing** (`VR_POINTERS_TODO.md` sections 1-3), mostly byte patches.
  - Work through them in batches, ordered by the features above; each batch is one session.
  - Method: size-sequence alignment, then check the code in `code.bin` with capstone.
- [ ] **About 100 addresses never confirmed against VR code** (`VR_POINTERS_TODO.md` section 5). Most
  were matched independently by two methods; confirm them in the disassembly when a crash points near one.
- [ ] **CEF crash guard audit.** Before the VR menu is created, any overlay call is a hard crash. Check
  that every `OverlayService` / `ExecuteAsync` / `CefListValue` path is guarded.
- [ ] **Intermittent crash:** a script event sent to a freed temporary reference during cell attach
  (see `KNOWN_ISSUES.md`).
- [x] **Papyrus stand-ins** for SetNoBleedoutRecovery and SetFactionRank are replaced by the real
  functions.

---

## 4. Convenience: make it shareable

The goal is that a new player installs one package, edits one line, and plays, all without taking
the headset off.

### 4.1 No keyboard needed in VR
- [x] **[untested] Auto-connect on game load** when `connect.txt` exists (`VRConnectService`). F6 still
  toggles; the F6/F7 keys are debug keys and don't exist in a release (master) build.
- [x] **[untested] Auto-party:** the server creates a party for the first player (`bAutoPartyCreate`,
  default on) and `bAutoPartyJoin` adds everyone else.
- [x] **[untested] HUD notifications:** connecting, connected (with the build), connection lost and
  retrying, refused (in plain words: wrong version with both builds, wrong password, plugins,
  uGridsToLoad), player joined (with the location) or left, party joined or left.
- [x] **[untested] Reconnect automatically** after a drop: 5, 10, 20, 30, then every 60 s.
- [x] **[untested] In-headset menu:** the normal Skyrim Together UI as a SteamVR dashboard tab
  (`Systems/VRDashboard.cpp`).
  - [ ] Adapt the page layout for the dashboard (large text, no empty full-screen areas).
  - [ ] A small always-visible overlay for chat and notifications while playing.

### 4.2 Versions and compatibility
- [ ] **Version check follows the git tag only.** The server already refuses a different `BUILD_COMMIT` and
  the HUD now names both builds, but the string comes from `git describe` at configure time
  (`build/BuildInfo.h`), so it goes stale until xmake reconfigures. Add a protocol number that changes with
  every message change.
- [x] **Build version** in the first log line and in the connected notification.
- [x] **[untested] Mod list comparison on connect:** the server logs the plugins that differ between the new
  player and each player already there. Showing it on the joiner's HUD needs a new message.
- [ ] **Startup checks with plain-language HUD errors:**
  - [x] uGridsToLoad = 5 (stops the automatic connection);
  - [x] SKSE VR loaded (warning only);
  - [x] `connect.txt` missing;
  - VR Address Library present;
  - fewer than 255 plugins;
  - `SkyrimTogether.esp` with the 1.70 header.

### 4.3 Packaging and install
- [x] **Release zip:** `Tools\VR\make-release.ps1` packages the client folder (exe from the build), the
  server with a password-free `STServer.ini`, the game files as an MO2 mod, the scripts and the guide.
- [ ] **Install script** (PowerShell):
  - finds the MO2 instance and copies the tool into it;
  - adds the MO2 executable entry;
  - creates `%LOCALAPPDATA%\SkyrimTogetherVR\connect.txt` from a template on first run;
  - checks the requirements listed in 4.2.
- [ ] **Update script:** replace the files with the rename-aside trick, so MO2 never needs closing.
  Optionally check for a newer GitHub release.
- [x] **Host script:** `host-server.bat` starts one server from its folder and prints the address to give.
- [x] **Connect setup:** `setup-connect.bat` writes `connect.txt` (no byte order mark, port added if missing).
- [x] **Log collector:** `collect-logs.bat` zips the client and CEF logs, the newest Crash Logger file and
  crash dump from the last day, and the build, onto the Desktop.
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
  player. Needs a new message: a downed player bleeds out and never reports a death state.
- [ ] Sensible defaults for VR in `STServer.ini` (difficulty sync, PvP off, time scale).
- [x] Trim the log to what's useful for a bug report (see 1.2).

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
