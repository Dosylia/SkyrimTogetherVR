# Skyrim Together VR: TODO

Plan for getting from "co-op works" to "smooth, and easy for other people to set up".
Items are ordered by priority inside each section. Details on existing bugs are in
`KNOWN_ISSUES.md`, and missing VR addresses are in `VR_POINTERS_TODO.md`.

Status tags: **[untested]** built but never checked in game · **[measure]** needs numbers
before any change · **[big]** several sessions of work.

---

## 0. Sessions of 2026-09-18 (three sessions, builds v1.8.0-35 to -58): results, and what the next session must answer

Deployed and untested since: `v1.8.0-58-g00cd9643-dirty.67dc30f` (same-named levelled variants synced, PvP sword
hits, shouts and powers, cast/projectile/drift logs). Every build goes into the tools folder; the friend gets the exe
and pdb from there. Both players run `collect-logs.bat` after each session and note the time of each problem.

**Confirmed working on 2026-09-18:** skills and level up screens while connected; the sword in the other player's
hand; spell casting and the held spell in the hands; arrows seen leaving the other player's bow; Lydia follows,
fights, and attacks the other player; sync of same-kind levelled bandits.

- [ ] **VR menu tab (in-headset menu) is still blank for the host. Priority: a menu that exists must work.**
      The friend's tab rendered (`100% of the page has content`, 20:38); the host's reported `0%` on every open
      (20:40, 20:42, 20:56, old build) and was not opened once in the session that carried the fix. The fix holds
      `enterGame`/`activate` until the page has loaded (`OverlayService::PushUiState`); CEF dropped them before,
      so the page stayed on its start-up view. Next session: open the tab on the host, then read
      `VRDashboard: 2 s after opening` and `logs\dashboard_frame.bmp`. If still blank with the page loaded, the
      page is not the problem and the upload is.
- [x] **Skills and level up screens black** while connected. The cause was the unpaused-menu patch itself:
      it worked in solo (hook inactive) and never while connected. On VR `StatsMenu` is out of the unpause
      allow-list and pauses like vanilla. Confirmed 2026-09-18. The other player sees you standing still in there.
- [x] **Spells off / spell beams below the hands / white flicker.** Body sync at frame end plus re-resolving the
      skeleton when something is attached below a bone (the held spell art hangs off the magic node). Sword and
      casting confirmed 2026-09-18.
- [x] **Arms following the VR pose:** confirmed; the `no VR pose data` line never named a player.
- [ ] **One player stops seeing the other** (21:56, cured by a reconnect). Both copies were alive and receiving
      actions the whole time, so it is not a removed actor. It coincides with the host equipping a staff (`29B75`,
      enchantment `B602E`) at 21:56:14, and the friend reported the host's "light scepter" breaking things.
      Logs now: `InterpDiag` (buffered movement ahead of playback), `Remote spell cast`, `Remote projectile
      launched`, every spell projectile on the sender. Needs the friend's description: invisible, frozen, or elsewhere.
- [ ] **Different animals on each screen.** The owner's base form now travels on both spawn paths
      (`CharacterSpawnRequest.BaseId`, `AssignCharacterResponse.BaseId`). Same name or same race: synced normally.
      A different creature (fox at a rabbit's reference, spider at a bear's): adopted and positioned by its owner,
      but the owner's animation data is withheld (`ForeignGraph`), so it slides instead of freezing. Refusing it
      instead was tried on 2026-09-18 and gave each player private bandits; never again. What remains is
      ownership: the bear the friend was fighting vanished when the host walked out of range and dropped it
      (see the ownership rework below). Levels differ between variants; cosmetic.
- [ ] **Sliding instead of walking.** `AnimDiag` readings from 2026-09-18 were polluted by a respawn loop (Lydia
      removed and re-assigned 11 times a second, fixed in `DiscoveryService`: remote actors get the grace period
      unless their 3D is gone). `AnimDiag busiest actors` now names loops. Re-read after the next session.
- [ ] **NPC under the ground for one player only** (Durak, a rabbit). `SinkDiag` measures how far the game moves a
      remote actor down between placements; it only ever named an Ice Wraith at a wolf's ground position. Next
      diagnostic: read the havok capsule with TiltedEvolutionVR's VR offsets (`aaf5d83`: controller at
      `MiddleProcess+0x250`, `+0x360` bhkRigidBody, `+0x10` hkpRigidBody, position `+0x1A0`, filter `+0x4C`).
- [ ] **Dropped items not visible** to the other player. Sender logs `drop: true`, receiver logs
      `Remote actor ... drops item`. Check both on the next drop.
- [ ] **Pause menu: "weird things happen to both players".** No trace in either log. Needs a description.
- [ ] **Nocked arrow not shown** on the other player's bow (the arrow leaves fine). The nocked arrow is an
      animation attachment and VR players never play the draw animation.
- [x] **[untested] Bandits naked.** `Actor::IsWearingBodyPiece` answers from the container entries on VR (a worn
      armour flagged as a body piece), so the once-a-second re-equip check is on again (2026-09-19).
- [ ] **Crash when quitting** still happens: both players on 2026-09-18 21:58, in whatever DLL is freeing at exit
      (`po3_ENBLightForEffectShaders`, `EnchantmentEffectExtender`). Not a gameplay crash; ask before attributing.
- [ ] **Grabbing an NPC with HIGGS isn't visible** to the other player. Analysis:
    - A grabbed NPC is a ragdoll on the grabber's game only. The owner of that NPC keeps simulating it standing, and
      the grabber's copy is overwritten by the owner's position.
    - Moving the reference can't drive a ragdoll: TiltedEvolutionVR measured four different writes doing nothing to a
      ragdolled body, which positions itself from its physics every frame. They exclude actors from their HIGGS sync.
    - What it needs: HIGGS's grab/pull/drop callbacks (its `IHiggsInterface001`, found through RTTI in
      `higgs_vr.dll`), an ownership transfer of the NPC to the grabbing player for the duration, and the ragdoll
      pose streamed like the VR pose (root plus the main bones) and applied at frame end on the other client.
- [ ] **Ownership churn.** Many `Transferring ownership` lines, some with position (0, 0, 0) for actors already
      gone, and `already spawned` re-sends (those are benign: the server re-sends a player's spawn on every cell
      crossing). See the upstream ownership rework below.

**Still to check:** VRIK menu only for the caster, killed NPCs stay dead, dragon and dialogue fixes, weapons at
spawn, reconnect after a drop, shouts (ported, untested), PvP sword hits (new, untested).

---

### From TiltedEvolutionVR and upstream, not done yet (decisions)

- [x] **[untested] Body sync on one thread, at frame end** (done 2026-09-14, see section 0 and
      `KNOWN_ISSUES.md`).
- [x] **[untested] Shouts and powers reach the other player.** Ported from TiltedEvolutionVR `20a62f9` on
      2026-09-18: `SpellItem` spell type (the `unk6C[3]` block is costOverride, flags, spellType), POWER /
      LESSER_POWER / VOICE_POWER pass the concentration filter, voice casts resolve from the sent form id.
- [ ] **TiltedEvolutionVR `aaf5d83`, item and body drift.** A remote body is moved by teleporting, and a teleport
      into a loose object makes havok fling it (their collision-layer table patch stops that); an NPC's capsule can
      be shoved 54 units off its body by a player and stay there (they read the capsule and warp it back). 1200
      lines, address-heavy; take it when under-the-ground or item drift is next.
- [ ] **TiltedEvolutionVR `1660eb0`, ownership blacklists and former-owner updates.** Read 2026-09-19: a follow-up
      to upstream #887 (ownership epochs); every change needs the epoch fields, so it only comes with the rework.
- [x] **[untested] Server hand-off of abandoned actors** (2026-09-19, `HandOffAbandonedActors`): every 2 s, an actor
      whose owner is out of its range while another player is in range goes to that player (two sweeps in a row
      required; owner told to relinquish, candidate told to claim). Log: `Handoff:` on the server. A paused or
      loading owner is covered too: the mod's update runs off the Papyrus VM, which a pause suspends, so a paused
      client goes silent, and an owner silent for 3 s counts as away (a silent player is never a candidate).
- [ ] **TiltedEvolutionVR `29f99ed`, two havok crash guards** (`SkyrimVR.exe+0AB1ABA` ragdoll add,
      `+03AD7B1` shadow scene listener on a temporary with no 3D). None of our dumps have those addresses; port
      them the day one does, they name the reference.
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
- Nobody has measured the cost of the hooks that run _inside_ the engine (animation update, equip,
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
- [ ] **[untested] PvP sword fights** (added 2026-09-18). A VR player never plays an attack animation, so the
      victim's game never sees the swing; with `bEnablePvp=true` (or `TogglePvp` in the server console) a hit on a
      remote player is sent as a health change and applied on their side. Damage only: no stagger, and blades do
      not clash (that needs PLANCK-style weapon physics).
- [x] **Projectile details missing.** The `LaunchData` pointers were never read on VR, so the shooter id stayed
      0 and no projectile was ever sent. Read since 2026-09-18 behind a check (readable memory, and the form table
      maps the id back to the same pointer); arrows confirmed seen by the other player the same evening. The
      first launches and every spell projectile are logged (`Projectile launch`).
- [x] **[untested] Dragons on the remote side.** `CharacterSpawnRequest.IsDragon` travels with the spawn and the
      client grid check uses it (2026-09-19).
- [ ] **Actor ownership warnings.** Look into `Actor for ownership transfer not found` and
      `OnNotifyActorTeleport: failed to retrieve actor` once the churn fix is confirmed.
- [ ] **Shared follower loops** (the Lydia case). A follower owned by one player gets pulled by the
      other player's game. The 2026-09-18 thrash (removed and re-assigned 11 times a second) was a different bug,
      fixed in `DiscoveryService`; Lydia followed and fought fine afterwards. The tug of war itself is still open.
    - Decide the rule (the follower's owner = the player it follows), then apply it during
      ownership transfer.

### 2.2 Items and inventory

- [x] **[untested] Item pickups by NPCs and players** (37521, 40533) are hooked again.
- [x] **[untested] Items added to actors and containers** (37525, 19708) are hooked again.
- [x] Gold amount (37527) resolves again.

### 2.3 VR body

- [ ] **Finger and grip pose.** Remote hands are always open. Add finger curl (a few bytes per hand)
      to `VRPose`.
- [x] **Spell casting in the hands** confirmed 2026-09-18. **Bow:** arrows leave the bow, the nocked arrow and
      the draw are not shown (no draw animation on VR players); see section 0.
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
- [ ] **[untested] Mount sync on** (InitiateMountPackage, address found by the friend and checked). Mount a horse:
  the other player sees you ride it, and only one player sits on it. `Rider not found` or `Mount not found`
  warnings mean a mount event couldn't be matched.

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
- [ ] **In-headset menu:** the normal Skyrim Together UI as a SteamVR dashboard tab
      (`Systems/VRDashboard.cpp`). Renders for the friend, still blank for the host; top of section 0.
    - [ ] Adapt the page layout for the dashboard (large text, no empty full-screen areas).
    - [ ] A small always-visible overlay for chat and notifications while playing.

### 4.2 Versions and compatibility

- [x] **Version string per build** (2026-09-19): `git describe` plus a hash of the uncommitted changes, set as
      the `BUILD_COMMIT` compile define in the root `xmake.lua` `on_load`, so one build always carries it
      (`build/BuildInfo.h` only holds fallbacks now). A client-only change still forces a server rebuild, since the
      string must match; restart the server after every deploy.
- [ ] Add a protocol number that changes with every message change, so unrelated client changes stop forcing
      server restarts.
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
- [x] Every build goes straight into `E:\FUS\tools\Skyrim Together VR` (rename the old exe aside); the friend
      gets the exe and pdb from there. The server must be rebuilt and restarted with every deploy for now (the
      version string covers both).
- [ ] No debugger attached by default (see 1.1).
