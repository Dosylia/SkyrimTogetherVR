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
- [x] **[untested] Same symptom on 2026-09-19 evening, second half of the cause.** Three logs (host, Seen, Elbios)
      side by side: every reconnect was manual (`Disconnected from server 4` is the client closing on purpose), the
      other copies were fed and at the right height until each one, and "the player is still here, we can hurt each
      other, we just can't see him" is a copy in its unrecoverable bleedout. Two paths still put it there: the
      owner's own death arrives in `OnActorValueChanges` as a health at or below zero and was applied as is
      (`corrected from 30 to the owner's -4`, 19:52:54), and his respawn gives everyone a fresh copy built from the
      server's stored values, which are the ones from his death (`corrected from -4 to the owner's 125` on the
      copy spawned one second earlier, on both other clients, 19:52:58 and 19:53:03). A copy born down never gets
      up when the real health arrives. Now: `StandingValues` clamps a player copy's spawn health to 1, and the
      owner-health path clamps to 1 (`kept at 1 health instead of the owner's -4; a copy never goes down`). Built
      pinned to the friend's server string (`v1.8.0-62-g9163b26b-dirty.db0f49e`) so all three could swap the exe
      without touching the server; the pin is a one-off in `modules/version.lua`, reverted after each build.
      Still unexplained: Seen's reconnect at 19:44:08 (nobody had died yet), and Elbios's game ending five
      seconds after his second respawn (19:57:39) with no crash log in his bundle.
- [x] **[untested] One player stops seeing the other until "he reconnects".** The reconnect was the cure the
      players applied, not the cause. His copy on the other side was in the data the whole time (3D, actions,
      equipment, even taking an arrow) but lying in the grass: `OnActorValueChanges` skipped health for every remote
      actor, so a player's copy only ever *lost* health (damage deltas arrive, healing never did), and copies are
      essential with no bleedout recovery, so at zero the copy went down for good. Since 2026-09-19 the owner's
      broadcast health is applied to remote player copies, a delta can't take a copy below 1, and a local hit never
      reports a kill on a copy. Log: `Remote player X copy health corrected ...`, `... kept at 1 health ...`,
      `... would have been downed by a local hit`. Seen 09-18 21:56, 09-19 09:14 and 10:26, always after fights.
- [ ] **Different animals on each screen.** The owner's base form now travels on both spawn paths
      (`CharacterSpawnRequest.BaseId`, `AssignCharacterResponse.BaseId`). Same name or same race: synced normally.
      A different creature (fox at a rabbit's reference, spider at a bear's): adopted and positioned by its owner,
      but the owner's animation data is withheld (`ForeignGraph`), so it slides instead of freezing. Refusing it
      instead was tried on 2026-09-18 and gave each player private bandits; never again. What remains is
      ownership: the bear the friend was fighting vanished when the host walked out of range and dropped it
      (see the ownership rework below). Levels differ between variants; cosmetic.
- [x] **[untested] Sliding instead of walking: wrong function skipped on VR.** `MotionDiag` settled it on 2026-09-20:
      every moving remote NPC copy on the receiving side had bones that never changed (201 of 201 samples at
      07:58), so the animation graph was not advancing. Cause: the remote-actor skip (`HookActorProcess`) hooks
      SE id 37356, the actor's AI step; TiltedEvolutionVR's VR table mapped that id to `0x5e0e20`, which the
      public VR database identifies as SE 36365, the actor's whole per-frame update. Skipping it froze the graph.
      The override now points at `0x6226a0`, the database's address for 37356 (it sits 0x40 after 37354 exactly
      as in SE). Watch for: remote NPC copies fighting their network position (AI now runs on them locally, as
      on SE upstream where the interpolation wins), and ragdolls of remote corpses.
- [x] **Stuck in the level-up menu (2026-09-20 07:58).** The empty-box drop caught the VR level-up choice panel,
      which the game opens data-less and fills afterwards. The drop is now limited to the one caller of the load
      phantom (`SkyrimVR.exe+0x168507`).
- [ ] **NPC under the ground for one player only** (Durak, a rabbit). `SinkDiag` measures how far the game moves a
      remote actor down between placements; it only ever named an Ice Wraith at a wolf's ground position. Next
      diagnostic: read the havok capsule with TiltedEvolutionVR's VR offsets (`aaf5d83`: controller at
      `MiddleProcess+0x250`, `+0x360` bhkRigidBody, `+0x10` hkpRigidBody, position `+0x1A0`, filter `+0x4C`).
- [ ] **Dropped items not visible** to the other player. Sender logs `drop: true`, receiver logs
      `Remote actor ... drops item`. Check both on the next drop.
- [x] **World frozen at load (2026-09-20, 01:34 to 02:36).** Music on, nothing moving, still rendering, no menu
      visible. Not the connection, the bot or the copy fix: a probe of the game's pause counter and menu stack showed
      a `MessageBoxMenu` with the pause flag sitting in the stack from the end of every save load (and one at the
      main menu), with no text and no buttons. A hook on `UIMessageQueue::AddMessage` (VR address from the public
      VR address database, id 13631 = SE 13530) named the sender: `SkyrimVR.exe+0x168507`, the game itself, a show
      request with no data. Such a box displays nothing and cannot be dismissed; when it opened before the
      connection it paused the world, when it opened after, our menu hook unpaused it but it swallowed the first
      controller press (which sent the game to the main menu). The client now drops a MessageBoxMenu show that
      carries no data (`UI.cpp`, logged as `dropped, a box with no content can only pause the world`). Why the game
      started sending it on the 20th and not on the 19th is not proven; the likely trigger is the VR controller
      state at those moments (asleep while the player was at the keyboard), which the VR layer reports through this
      box. The diagnostics stay in for now: `Probe` every 5 s, `Menu queued`, `UI message for MessageBoxMenu`.
- [x] **[untested] VR tab Disconnect reconnects by itself.** Now routed through `VRConnectService::Toggle`. Was: It closes the socket through the overlay client, which the
      auto-reconnect treats as a dropped line (`attempt 2` five seconds later, 01:55:37). Route it through
      `VRConnectService` so a chosen disconnect stays disconnected.
- [ ] **[untested] Whiterun gate: is the second vanish cause the worldspace change?** Logging in place on both
      sides (`WorldDiag` on the server for every remove and re-send of a player's copy on a cell or worldspace
      change, and for every grid shift; `WorldDiag: server removed player copy` on the client with our worldspace
      and cell). Test: run `STBot.exe scripts\stand.txt`, then walk through the Whiterun gate and back out. The
      copy must go when you are inside and come back when you are out. If it does not come back, the server
      log names the decision that withheld it.
- [x] **[untested] Invisible attacker (2026-09-20 14:50).** A reference the owner drives but this game cannot find
      (`Failed to retrieve Actor 10DE94`, a Novice Conjurer, five times while "something" attacked) was simply
      not spawned. Now a copy of the owner's base stands in (`Stand-in: reference ...`), registered as a ghost;
      if the local reference loads later it is disabled (`Ghost: reference ... loaded after its stand-in`).
- [x] **[untested] Hand-off flips a creature's kind (troll on one side, wolf on the other, 14:50).** A client
      refuses a hand-off for a reference that has a ghost here (`Hand-off ... declined`).
- [ ] **Level-up and death end at the main menu, connected only (07:58, 11:46, 11:55, 14:42 x2, 15:08).** Solo
      the level-up is fine (15:35 test): the attribute box closes from the level-up code itself
      (`SkyrimVR.exe+0x8d93d2`) and play goes on. Connected, the box is closed by `SkyrimVR.exe+0xf207f7`
      ("type 3, data yes") and a save load starts 6 ms later (Loading Menu, Mist Menu), which ends at the Main
      Menu. The death case is the same: "respawning player" -> the box from `SkyrimVR.exe+0x168507` (the same
      helper that shows one after every save load) -> closed by `0xf207f7` -> load -> Main Menu. So one game
      routine at `0xf207f7` answers a message box with a save load whenever we are connected. Neither address is
      in the address library and the exe on disk is Steam-encrypted, so it cannot be read offline.
      `SaveLoad: load requested` (deployed 15:17, not yet hit) names the file and the caller at the next
      occurrence; that is the missing piece. Candidates: the box runs unpaused when connected (UnfreezeMenu)
      and gets `kUpdateUsesCursor`; both are ours. Since 16:17 every MessageBoxMenu message also logs the
      12 frames above the caller (`stack [...]`) and the first four words of the message data (`data words`):
      +0xf207f7 lies in the UIMessageQueue region (between ids 80061 and 80077) and +0x168507 in the TES helper
      `sub_1401575D0` (id 13213), both generic, so the frames above them name the feature. A one-off read of
      the two code regions from the running game (no debugger; scratch script `dump_live.py`) can be
      disassembled with capstone once the game sits at the main menu; the exe on disk is Steam-encrypted.
      Friend's reading (2026-09-20): the caller is a 0xC1-byte function, SE 0x140EC3C80 = VR 0xF20750 (RET at
      VR 0xF20810), whose last call is AddMessage at VR 0xF207F2. Too small to be the feature: a send-message
      wrapper; its callers (xrefs) are the next thing to read. Timing says the "load" is a quit to the main
      menu, not a save load: hide -> Mist Menu -> Main Menu took 1.2 s at 14:42 (twice) and 15:08, 31 s once
      at 11:46. Same shape as the game's own "return to main menu" (Loading, Mist, Fader, LoadWaitSpinner, HUD,
      the TES box, Main Menu).
      Seenfront's second reading (17:21): a queued request of type 0xD0000010, built by the constructor at
      SkyrimVR+0x594D80, can reach the hide through 5910D0 -> 5924A0 -> 584910 -> 591E80 -> 168160 -> F1BF10 ->
      F1D0E0 -> F20750 -> AddMessage (return RVAs to look for in the hide's stack, inner to outer: F207F7,
      F1D1D8, F1BF61, 168242, 591F69, 584A58, 592763, 591390). Two producers build it: the save-warning dialog
      callback at +0x595B60 (response 1 queues it, calling the constructor from +0x595BCF) and another result
      handler (from +0x591964). Also +0x168507 belongs to FUN_140168160, not to the function at +0x168020. The
      client now logs the constructor (`SaveLoad: request built`, with caller, stack, raw arguments and the
      object's first words) and the callback's response byte (`SaveLoad: warning callback`). A mechanism is
      verified, its involvement is not; the runtime stacks decide.
- [ ] **Distant dragon vanishes (15:03).** Read again with positions: the dragon hovered at (62387, 48916,
      z 7287) while the player stood at (55309, 56470), about two cells away diagonally, on the edge of the
      5x5 loaded grid. Both engines unloaded it at that edge (`Actor removed 2034EDC` / `3034EDC`), which is
      the game, not the sync: no ownership rule can draw an actor the engine has unloaded. What the sync adds
      is churn: each unload sends a transfer, the other side is told to take it while it is unloaded there too
      (`Actor for ownership transfer not found`), the server destroys the entity and the next load registers
      it under a new id (200087 -> 300087), and the party leader claims it back each time. A rule to keep the
      last owner would only cut the churn, not the vanish, so nothing was changed. Worth a solo check: does a
      dragon circling that far away pop out the same way without the mod.
- [ ] **Paused player goes silent (11:51).** Not confirmed. The world probe keeps running while the pause
      counter is 1 (02:05 and 15:35 logs), the local update sender has no pause gate, and connected menus run
      unpaused anyway, so a player in a menu should keep sending. Every hand-off seen on 2026-09-20 afternoon
      happened with the pause counter at 0 (range hand-offs), except one on Seen's side in the Journal Menu
      (14:58:45 -> 14:58:48). A loading screen is the one time nothing is sent. The probe line now ends with
      `last move sent N ms ago`; a value over 3000 on a paused player would confirm the silence. Also needed:
      the server log from the host (`Handoff: ... silent` vs `out of its range`), which lives in the host's
      Server folder.
- [x] **[untested] Empty hands at spawn ("spawn weapon not displayed", "we always spawn with empty hands").**
      The copy's hands were compared by the container's worn flags, and SetInventory sets those (it equips worn
      entries before the copy has its 3D), so every arrival logged "1 hand items wanted, 1 held now" and skipped
      the equip; the weapon only showed once the owner re-equipped it and a real equip event arrived. The client
      now keeps its own record of what it put in each copy's hands (empty on arrival) and equips from that
      (`hands on arrival: ... put there by us`). Real equip events update the record. Spells unchanged.
- [x] **[untested] Full body tracking (SkyrimVR FBT, Nexus 185070) shown to the other player.** The pose now
      carries seven lower-body bones (pelvis, thighs, calves, feet) behind a HasLegs bit. They are captured only
      while the FBT plugin is loaded on the sender (module name containing "fbt" or "fullbody", logged as
      `VRBodySync: full body tracking plugin ... is loaded`); everyone else sends the upper body as before and the
      copy's legs stay on the walk animation. The receiver needs nothing installed. Rotations only: a crouch that
      lowers the pelvis shows as bent legs on a body that does not sink. Encoding change, so client and server
      both change. Not tested: nobody here has trackers yet. The plugin is `SkyrimVR-FBT.dll` (1.0.3 download
      checked). Its fbt.ini also moves the pelvis (WaistMode 1, position + rotation) and bends the lower spine
      (SpineBridge); neither position is sent, so a tracked crouch shows as bent legs on a body that does not
      sink. The mod's kick damage on an NPC the other player owns is a separate test.
- [ ] **Quest dialogue heard twice (15:05).** Upstream syncs the other player's dialogue lines and subtitles; with
      both in the same conversation each hears both. Decide: only the speaker's own conversation.
- [ ] **Grey hills (15:01).** Missing distant terrain textures; a screenshot exists. Likely the game's LOD stream
      under memory pressure, not sync. Check once with the game alone.
- [ ] **Pause menu: "weird things happen to both players".** No trace in either log. Needs a description.
- [ ] **Nocked arrow not shown** on the other player's bow (the arrow leaves fine). The nocked arrow is an
      animation attachment and VR players never play the draw animation.
- [x] **[untested] Bandits naked.** `Actor::IsWearingBodyPiece` answers from the container entries on VR (a worn
      armour flagged as a body piece), so the once-a-second re-equip check is on again (2026-09-19).
- [ ] **[untested] Crash when quitting.** Read from the DLL's disassembly (2026-09-19): Enchantment Art Extender's
      equip handler reads the `UI` singleton (`SkyrimVR+0x1F83200`, our id 400327) and tests `numPausesGame`
      (`+0x160`); at quit the singleton is already destroyed while our disconnect handler was still deleting remote
      players and flipping actors back to local, which fires equip events. `IsProcessExiting()` now gates the
      disconnect handler, temporary-actor deletion and ghost re-enables. The ENB light plugin's quit crash is the
      same class (shader art freed on our teardown). The "black screen then crash" after the friend's respawn was
      the same quit crash after giving up. The black screen itself: VR's `FadeOutGame` takes its first argument
      with inverted polarity and as a dword (TiltedEvolutionVR, checked against the VR prologue); ours passed bools
      through, so the death fade did nothing and the respawn fade-in faded to black ("bright light, then black
      screen", 10:39 on the 19th). Fixed 2026-09-19, untested.
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
- [x] **PvP sword fights** confirmed 2026-09-19 (`bEnablePvp=true`). A hit on a remote player is sent as a health
      change and applied on their side. Bug found the same day: any hitter's damage was forwarded, so the host's
      spider bit the friend 90 times a second on top of his own spider; now only this player's own sword, arrow
      and spell hits travel. Still damage only: no stagger, and blades do not clash (PLANCK-style weapon physics).
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

### 6.1 Test bot: a second player without a second headset (planned 2026-09-20)

A console program, target `STBot` under `Code/bot`, built from the same tree (same version string), that connects
to a server exactly like a client and drives one player character from a script. Phase 1 changes no client or
server code; it only links `SkyrimEncoding` and `TiltedConnect`. Built 2026-09-20 01:20; the handshake, the mod
list, the host position from the host's log and the scouting were checked against the local server. The spawn,
movement, death and respawn paths are **[untested]** until a headset is in the game.

**Run it** (server, then the game joined to it, then the bot, from `build\windowsdelease`):
`STBot.exe ..\..\..\..\Codeot\scripts\copy-respawn.txt` (defaults: `--server 127.0.0.1:10578`, the
password from `config\STServer.ini` via `--password`, `--hostlog` the host's `tp_client.log`, `--spacing 200`).
Its log is `logsot.log` next to the exe. The clone carries the host's face, outfit and in-game name.

- [x] **Handshake:** `AuthenticationRequest` with the tree's `BUILD_COMMIT`, the password, a name, `Skyrim.esm` as
      the mod list (form ids below come from it), level, time. The server's mod policy is off for us.
- [ ] **[untested] Character:** wait for the host's own `CharacterSpawnRequest`, reuse its `AppearanceBuffer` and `ChangeFlags`
      (the bot is a clone of the host, so the receiving client builds the face from data it already accepts), then
      `AssignCharacterRequest` 200 units from the host with the same worldspace and cell, Nord race, iron sword and
      Flames in the inventory, health 100. Then `EnterExteriorCellRequest` from the host's grid.
- [x] **Never own anything:** the server makes the first player party leader, so the bot leaves any party it is
      asked to lead and stays as a plain member otherwise. Any (the leader claims every actor). Any
      `NotifyOwnershipTransfer` the server hands the bot is sent straight back (`RequestOwnershipTransfer`), otherwise
      the NPCs it would own freeze on the host's screen.
- [x] **Script primitives** (a text file, one line each): `log`, `wait s`, `walk x y [speed]`, `walk rel dx dy [speed]`,
      `follow distance seconds`, `equip hexid [left|both] [spell]`, `unequip hexid`, `health n`, `damage n`, `die`,
      `respawn` (`PlayerRespawnRequest`, health restored 500 ms later, like the client), `disconnect`, `reconnect`,
      `loop`, `stop`, `start x y`. Not yet: `cast`, interiors. Movement is `ClientReferencesMoveRequest` at 20 Hz with
      positions only: the copy slides, which is enough for spawn, range, hand-off, death and health tests.
- [ ] **[untested] First scenario, the bug of 2026-09-19** (`Code/bot/scripts/copy-respawn.txt`): spawn, walk past the host, take damage to -4, respawn, keep walking.
      Pass: the host's log shows `copy would have spawned with the owner's health -4 ... started at 1` and the copy
      stays visible after the respawn.
- [ ] **Phase 2, real animation:** record the host's own outgoing packets into `logs\session.stpcap` (a log like
      `tp_client.log`, capped and rotated, not a toggle) and let the bot replay a recording with its server ids
      remapped, so the copy walks, fights and casts like a real player.
- [ ] **Where it runs:** on the host's PC against the local server. Never on the friend's server during play.
