# Skyrim Together VR: TODO

Plan for getting from "co-op works" to "smooth, and easy for other people to set up".
Items are ordered by priority inside each section. Details on existing bugs are in
`KNOWN_ISSUES.md`.

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
- [ ] **NPC under the ground for one player only** (Durak, a rabbit; last seen 2026-09-19). Not seen since, and
      nothing was changed for it on purpose. Two changes since then could have taken it away by accident: the AI
      step fix (remote actors' animation graphs advance again, so their ground snap runs) and the health clamp on
      player copies. `SinkDiag` stays in; if it names nothing for another two sessions, close this. The havok
      capsule read (TiltedEvolutionVR `aaf5d83`: controller at `MiddleProcess+0x250`, `+0x360` bhkRigidBody,
      `+0x10` hkpRigidBody, position `+0x1A0`, filter `+0x4C`) is only worth doing if it comes back.
- [~] **Body there but not drawn (EMERGENCY; 2026-09-20 21:12/21:20/21:27, 2026-09-21 20:43/20:45 and many
      more, every reconnect that evening was a cure for it).** Three wrong guesses first: the health floor, the
      body scale, and the invisibility actor value. The copy probe killed all three. At every moment either
      player could not be seen, the copy read perfect: metres away, full health, not dead, not bleeding out, not
      disabled, 3D present with every child and a parent, both scales exactly 1.000, invisibility flat 0.00. The
      refusals added for invisibility never fired once, because there was nothing to refuse.
      What is left, and what the fix of 2026-09-21 21:xx acts on: only player copies vanish, and player copies are
      the only bodies we write bone transforms into, thirty times a second; it grew far worse the day the fingers
      added thirty more writes per frame. Each posed bone's local rotation was built by multiplying the previous
      frame's value by a correction, never re-derived, so error accumulated with nothing to shed it, and a bone
      matrix that stops being a rotation takes the skinned body out of the render while leaving the actor
      untouched. It is now derived fresh each frame from the parent's world rotation, which cannot drift and
      repairs a bone that already has. On top, a body whose bones the renderer cannot use (row length off 1, wild
      scale, anything not finite) is no longer posed at all until the game's animation puts it right, which brings
      it back without a reconnect and logs which bone and what value. `CopyDiag` also now says whether the body
      hangs from the same scene as the player, which is the one remaining alternative cause.

- [ ] **Dropped items not visible** to the other player. Sender logs `drop: true`, receiver logs
      `Remote actor ... drops item`. Check both on the next drop. Reported with it (2026-09-20): when he drops
      something, his copy's body stays in place but "all his bones try to violently leave it". That is the VR
      pose fighting an animation: the drop plays a throw or ragdoll-style animation on the copy while the
      pose keeps writing the sender's bone rotations over it, so every bone jerks between the two each frame.
      Worth checking against `VRBodySync` whether the copy is in a hit, ragdoll or "drop" animation state during
      those frames and skipping the pose then, as it already does for dead and bleeding-out bodies.
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
- [x] **[untested] Game died at startup on the first build after the upstream merge (2026-09-22 21:33).** The
      merge made the VR build load SKSE itself. Upstream added a `LoadScriptExtender()` call to `RunTiltedInit`;
      before the merge that function existed but was spelled `LoadScriptExender`, a typo, and nothing ever called
      it, so VR had always let the `d3dx9_42` preloader FUS ships bring SKSE VR up once the game was ready.
      SKSE VR initializes from its DllMain, so calling it there loads every SKSE plugin against a game that has
      not finished starting: `sksevr.log` stops at `checking plugin CombatMusicFixNG.dll`, that plugin logged its
      version at 21:33:01.561 and the process died 7 ms later on a null write in ntdll, with only SKSE and that
      plugin on the stack. No mod on this machine had changed in days. The call is now `#ifndef SKYRIMVR`, which
      restores exactly what worked before the merge and leaves SE on upstream's path. It also stops
      `g_ScriptExtenderStarting` being flipped, which is the flag that keeps EngineFixesVR out of this launcher
      (our own comment in `FileMapping.cpp` says it crashes under it), so that was a second failure waiting a few
      plugins later in the alphabet. `IsScriptExtenderLoaded` on VR now asks the process for the sksevr module
      rather than reporting on a handle we no longer hold, so `CheckInstall` stops claiming SKSE is missing.
      Worth watching on the next merge: upstream calling a function of ours that had been dead code.

## 0. Everything from 2026-09-24, ordered. Nothing dropped.

Ordered by what costs a session first, then by how cheap the fix is. "Debug plan" means there is no fix yet and
the task is to find the cause rather than guess at one.

### Shipped today, all unplayed. Confirm these before anything new goes in.

- [x] **[untested] Essential NPCs hammered with Kill().** 236 attempts on Commander Caius in two minutes, 38 on
      another actor, every one returning `dead: false`, each re-entering the death transition. Bleeding-out actors
      are no longer killed at all, and after three refusals an actor is left alone with one warning.
- [x] **[untested] Downed NPCs lay on the floor and never got up.** Bleedout is neither dead nor alive, and two
      places only tested `IsDeadOrDying()`: `HookActorProcess` suppressed a downed remote actor so it could not
      play its get-up, and `InterpolationSystem` force-positioned it to follow its standing owner. Both now treat
      bleedout like dying.
- [x] **[untested] The server stored damage as healing.** Damage is a negative delta and the server subtracted it,
      so the health it hands out on spawns, hand-offs and respawns drifted upward.
- [x] **[untested] Animation replay backlog.** One action is played per frame and nothing bounded the queue, so a
      looping owner built a backlog that could never drain: 717 refused `Unequip` replays on one guard while 30
      other actors starved. Capped at 96, oldest dropped.
- [x] **[untested] Dragging a corpse was invisible.** `PoseActor` writes bone *world* transforms and the renderer
      follows the bones, so writing the root position alone moved nothing. The offset now reaches every bone.
- [x] **[untested] Ownership churn crashed the receiver.** The grab detector claimed any dead body more than 32
      units from its network position; corpses sit that far apart routinely, so it fired 207 times across 58
      bodies in one session and the entity churn left `AnimationSystem::Update` casting a freed form. Removed.

### 1. Costs a session, cause known, do next

- [x] **[untested] The `Unequip` loop: found and stopped (2026-09-24).** `InventoryService::RunNakedNPCBugChecks`
      walks every actor once a second and calls `ResetInventory` on any it thinks is naked. Re-equipping emits
      equip and unequip animation events, which travel to the other players as actions to replay, and nothing
      ever stopped: if `IsWearingBodyPiece()` stays false -- a body slot the check does not understand, an outfit
      a mod manages itself -- the same actor is reset every second for as long as it is loaded. One per second
      matches the 717 refused replays on a single Redoran Guard almost exactly. It now gives up after three
      attempts per actor and says which actor it gave up on. Capping the replay queue treated the symptom; this
      is the source.
- [x] **[untested] The receiver could replay the wrong spell (2026-09-24).** It read `magicItems[CastingSource]`
      first -- whatever is staged in that hand on the receiving side -- and only fell back to the transmitted id
      when the slot was empty, so a quick spell switch or a late equip message replayed the previous spell. The
      transmitted id is now tried first, with the staged item as the fallback for a spell this game cannot
      resolve, and the fallback says so in the log.


- [ ] **The `Unequip` loop itself.** Capping the queue treats the symptom. Something makes one actor emit the same
      unequip action hundreds of times; the suspect is the once-a-second naked-NPC re-equip check fighting whatever
      unequips it, and `Actor::IsWearingBodyPiece` was rewritten during the merge to read container flags.
      **Plan:** on the owner's side, log each time the re-equip check acts on an actor, with what it saw and what
      it did. One session then says whether our check is the source. Cheap, and it is the last known cause of the
      stream starving.
- [x] **[untested] Spell hits are now authoritative against another player (2026-09-24).** A damaging spell on a
      remote player used to end in `MagicTarget::AddTarget` applying nothing and sending nothing, so whether it
      hurt was decided entirely by the target's own game re-simulating the projectile. A sword already avoids
      this: the attacker's game is the only one that knows the swing connected, so it sends the hit as a health
      change. Spells now do the same, under the same conditions -- PvP on, cast by this player, never applied
      locally -- and only for **instant** damage-health effects, because a damage-over-time effect arrives once
      and then ticks on its own. Anything not recognised as damage-health keeps today's behaviour of sending
      nothing, so the failure mode is the status quo rather than a wrong number.
      **Known inaccuracy:** the magnitude sent is the effect's, before the target's resistances, where a sword
      sends damage the attacker's game has already mitigated. Spells will therefore hit a little harder than they
      should until the target's side applies resistance. Watch `PvP: spell X hit remote player Y for N`.
- [ ] **Superseded: spell hits are not authoritative.** The caster sends the cast and the projectile; whether it *hit* is
      decided separately on each machine, so a spell can land on one screen and miss on the other. Sword hits
      already avoid this by sending the damage. **Plan:** do the same for spells against a remote player, reusing
      the health-change path. Medium, no new addresses, and the most likely explanation for "I damage him, he
      takes nothing".
- [ ] **The receiver can replay the wrong spell.** It uses whatever is equipped in that casting slot and only
      falls back to the transmitted spell id when the slot is empty, so a quick spell switch replays the old one.
      **Plan:** trust the transmitted id first, fall back to the slot. Small.

### 2. Crashes with no cause yet: debug plans, not fixes

- [ ] **Elbios, on opening the wrist menu.** `TweenMenu` at 21:27:52.184, `InventoryMenu` 174 ms later, dead 6 ms
      after that on a null byte read. No frame of ours anywhere in the stack; it holds `handleVRButtonUpdate`,
      `SkyUI-VR::DispatchControllerState`, HIGGS and FBT callbacks, which is controller input reaching a Scaleform
      menu mid-initialisation, and there is a matching firsthand report on FBT's support page. Our contribution is
      timing only: `TweenMenu` and `InventoryMenu` are kept unpaused.
      **Debug plan:** have him open and close the wrist menu a dozen times in one session. If it reproduces, drop
      those two from the unpause list (brief hub menus, so the cost is small) and see whether it stops. Do not
      remove them pre-emptively: a paused player stops sending updates and goes silent to everyone else, which
      trades a confirmed problem for an unconfirmed one.
- [ ] **Seen, on the load after death.** Save/load request built at 21:28:07.734, `Loading Menu` while still
      connected, access violation 68 ms later with a `MovementHandlerArbiter*` in the registers. Same connected-load
      path as the long-standing "level-up and death end at the main menu" item.
      **Debug plan:** the `SaveLoad` probes already installed name the caller and the stack; read the next
      occurrence from them rather than from the dump.

### 3. Known broken, plan already written, waiting on a session

- [ ] **Invisible player, worse indoors.** Plan in section 0a: measure render state (fade, cull) rather than actor
      state, and recover automatically instead of reconnecting. Nothing built yet.
- [ ] **Friend's health bar.** Plan in section 0a, blocked on the one-minute PvP test only the players can run.
      Note the damage itself works: the log shows hits landing and the player going down, so what is missing is the
      feedback, not the damage.

### 4. Position accuracy: two problems that look alike and are not

- [ ] **The other player's hands sit slightly off (palms together, seen lower).** Structural, not a bug. `VRPose`
      carries bone **rotations only** and `PoseActor` keeps each bone's own translation, so anything VRIK does by
      moving a bone rather than turning it -- height calibration, crouch, arm length -- cannot be reproduced, and
      the arm lands at the receiver's own skeleton's height.
      **Plan:** send the root-relative translation of a few bones, hands and head first since those are what people
      touch, the same way the corpse root position was added. Start with two and measure the cost. Medium, and it
      is the honest fix; nothing else closes that gap.
- [ ] **Your own sword hitting where the blade is not.** The screenshots of 2026-09-24 are a first-person view of
      the player's own weapon striking the Earth Stone with the impact well off the blade. This client never writes
      the local player's bones, it only reads them, so this is not sync and not ours: it is the weapon collision
      offset of the VR setup (VRIK or HIGGS).
      **Debug plan:** reproduce in solo with the mod disconnected. If it still happens it is a VRIK/HIGGS setting
      and belongs in their configuration. Do that before any time is spent here.

- [x] **[untested] Swept the null-extension crash family out of every engine hook (2026-09-25).**
      `Actor::GetExtension()` returns null by design for an actor this client did not allocate, and 62 call sites
      dereferenced it without checking. Two of those killed both players: `HookActorProcess` on 2026-09-23 (a
      Redoran Guard during a crime response) and the same family on 2026-09-24.
      Every dereference inside a hook the engine calls with an arbitrary actor is now guarded -- 14 sites across
      `Actor.cpp`, `References.cpp`, `SubtitleManager.cpp`, `InvisibilityEffect.cpp`, `ActorMagicCaster.cpp` and
      `TESObjectREFR.cpp`. All were read-only questions about whether an actor is one of ours, so the fallback is
      the same everywhere: no extension means not ours, condition false, engine path taken.
      **Not done by making GetExtension() return a dummy**, which would have fixed all 62 at once: 23 sites
      *write* through it (`SetRemote`, `SetPlayer`, `GraphDescriptorHash`), and a shared dummy would swallow those
      silently. One deref remains inside a hook, in `PlayerCharacter.cpp`, and it is on the local player, who
      always has an extension.

- [x] **[untested] Two unchecked lookups in the message handlers, from the audit (2026-09-25).**
      `OnBeastFormChange` ran `std::find_if` for the player's entity and dereferenced the result without checking
      it against `end()`. That is undefined behaviour whenever the player has no entity -- before it is created,
      or after teardown -- so a werewolf or vampire lord transformation at either moment would have taken the game
      with it. `OnNotifyNewPackage` passed the result of `Cast<TESPackage>` straight to `SetPackage` without
      checking it, so a form id that resolves to something that is not a package (a mod mismatch between the two
      games) handed it null.
      Swept the rest of the client for the same shape: the two other dereferenced `find_if` results, in
      `CharacterService` and `ObjectService`, are both already guarded. `OnBeastFormChange` was the only one.

### Bot coverage (2026-09-24)

- [x] **Unattended test runs.** `Tools\VRun-bot-tests.ps1` starts a server, puts a standalone bot in the world
      to hold it open, runs a script and reports pass or fail, with no human and no headset.
      `-Script <name> -Repeat <n>`. Exit code 0 means every check passed.
- [x] **Scripts so far:** `auto-suite.txt` (presence, health, death, respawn, movement, reconnect -- 8 checks),
      `death-recovery.txt` (three death and respawn rounds back to back, which is where things used to come
      apart), `churn.txt` (four join and leave round trips, down to a one-second turnaround, aimed at the audit's
      duplicate-spawn and non-idempotent-removal findings), `health-sign.txt`, `suite-soak.txt`.
      `churn.txt` passes 11 checks and the server logs nothing during it, so those two findings are **not**
      reproducible this way yet -- it stands as a regression guard rather than a reproduction.
- [ ] **What the bot still cannot cover, and what to do about it.** It has no game behind it, so nothing visual is
      testable: invisible bodies, dragged corpses, hand positions, the health bar. Those need a headset. What
      could be added without one: an NPC-ownership churn test (two bots taking turns owning an actor), a test that
      a looping animation does not starve the stream, and a test that a spell hit reaches the target's health.
      The last one needs the bot to be able to cast, which it cannot yet.

### 5. Smaller, known, unglamorous

- [ ] **`Actor::ForceState` (AE 37313) does not resolve**, confirmed by the new unresolved-address logging, so that
      hook has never installed. Ask Seenfront. The same logging cleared 16113, 80061, 104788, 104359, 36564 and
      40412 as either handled or harmless.
- [ ] **The crime alarm guard is installed but has never fired.** Address confirmed at `0x1405e5dc0`; no crime has
      been committed since it landed. Needs one session of actual crime.
- [x] **[untested] `This isn't a crime faction! 4018279` (fixed 2026-09-24).** The Solstheim crime faction was
      written as the form id `0x04018279`, which is only right when Dragonborn.esm loads fourth. On this modlist
      it loads second, so the lookup failed on both deaths logged that day -- meaning every co-op death. There is
      no plugin-name lookup bound in this client, so the load index is now found by trying them (256 form lookups,
      once, cached), and the log says which index it found. The nine Skyrim.esm factions were always fine: their
      index is 0x00.
      **Still a decision, not a bug:** every multiplayer death clears bounties in every hold. Nobody asked for
      that, and it is a real gameplay change. Worth deciding whether co-op death should pay fines at all.
- [ ] **Superseded: `This isn't a crime faction! 4018279`.** Every multiplayer death calls `PayCrimeGoldToAllFactions` with a
      hard-coded faction id that depends on load order, and it failed on both deaths logged. Clearing bounties on
      every co-op death is also a gameplay decision nobody asked for. Small fix, worth a decision first.
- [x] **[untested] NPC health now has an authoritative correction (2026-09-25).** An NPC's health on the
      receiving side was only ever the sum of the damage deltas that happened to arrive: a delta lost to a starved
      stream, a hit applied on one side only, an owner handover mid-fight, and the number drifted away from the
      owner's for as long as the actor lived, with nothing to repair it. Health snapshots were applied to player
      copies only.
      The owner's snapshot is now used for NPCs too, but **as a correction rather than a constant override**, and
      only when the gap is 25 or more: a small difference mid-combat is normal and is left alone, and the delta
      path still carries deaths, which a continuous override would fight every frame. Dead actors are untouched.
      Every correction is logged (rate limited to one line per five seconds) as
      `NPC X health corrected from A to its owner's B`.
      **What to watch:** if that line appears constantly rather than occasionally, the stream is dropping deltas
      and this is papering over it -- which would itself be the finding.
- [ ] **Superseded: NPC health has no authoritative correction.** Snapshots are ignored for NPCs in favour of accumulated
      deltas, so once a value diverges nothing repairs it. Larger work, and it interacts with the essential-NPC
      handling above.
- [ ] **Duplicate spawn messages are discarded rather than refreshing state**, so a newer spawn carrying updated
      ownership, cell or death state is dropped. Explains some of the "already spawned" warnings.

---

## 0a. The three open complaints, with a plan each (2026-09-23)

- [ ] **Invisible player: stop guessing at actor state, look at render state.** Five occurrences now, and every
      time `CopyDiag` reads perfect: present, metres away, right scene, invisibility 0.00, scales 1.000, spine
      rotation clean, 3D root with its children. Four causes were proposed and all four were disproved by that
      probe (health floor, body scale, the invisibility actor value, bone drift). They share a blind spot: they
      are all *actor* state. A body can be flawless as an actor and still not be drawn, and the render state is
      the one thing never measured. New clue from 2026-09-23: it happens far more indoors, and fade and LOD
      behave differently in interiors.
    - **Stage A is built and deployed (2026-09-24, `7bb3c6d`), and it does not guess.** `CopyDiag` now prints the
      eight words just past the end of a VR `NiNode` (0x140 to 0x15C) for the copy **and** for the player, whose
      body is always drawn. The fade offset was not taken on trust: CommonLibVR puts `BSFadeNode`'s data at
      +0x128, but VR keeps `NiNode::children` at +0x138, so +0x128 is still inside `NiNode` here and cannot be the
      fade. Reading it anyway would have been the fifth wrong offset on this bug. Instead: whatever the player
      reads while visible is the "drawn" value, and the word that differs when a body vanishes is the fade.
      **What to do with the next occurrence:** find a `CopyDiag` line from while the body was invisible and compare
      its `copy words` against `player words` on the same line. One should differ. That names the offset, and
      Stage B can then force it.
    - **Stage A (superseded plan).** Add to `CopyDiag`, for the copy's 3D root: whether it is a `BSFadeNode` and its
      `currentFade` (CommonLibVR puts the runtime block at +0x128 and `currentFade` at +0x08 inside it, so
      +0x130), the cull flag on the root and on each skinned child, and the copy's parent cell and worldspace
      against the player's. Log the raw values first and cross-check the offsets against neighbours the way
      `charController` was checked, before any of them is acted on; VR layouts have burned us twice this week.
    - **Stage B, recover without a reconnect.** When a copy is close and reads as not drawn (fade at or near
      zero, or culled) for more than about three seconds, clear it: force the fade up, clear the cull, and if
      that does not take, rebuild the 3D. Automatic, no key press, and a log line every time it fires. Even if
      the cause is something else, this ends the reconnect ritual, and the log then says how often it was needed.
    - Stage B is only worth shipping with Stage A's numbers in the same build, so one session answers both.

- [ ] **Friend's health bar: the message is right, the widget refuses it.** Proven on 2026-09-23: our message is
      the game's own (a `HUDData` with type 0xB at +0x10, level at +0x20, flags 0x0101 at +0x22, the handle at
      +0x28, sent to `WSEnemyMeters` as `kUpdate`, which is 0 and matches what the probe sees the game send).
      It was pointed at the friend **107 times** that session and drawn **once** — and the once was the moment
      she burned him with fire.
    - **Hypothesis:** the widget only draws for an actor the game holds as a valid hostile target, and friendly
      fire briefly made him one. That single data point fits nothing else.
    - **Decisive test, one minute in game:** make the two players hostile (PvP) and watch whether the bar becomes
      reliable. If it does, the game's enemy meter structurally cannot show a friendly player and no amount of
      message-fixing will change it.
    - **Then it is a choice, and it is the user's:** drive a different element the HUD already has, read
      `EnemyHealth::Update` to find what it tests and satisfy that without making anyone hostile, or accept a
      minimal element of our own. The last one was refused before on immersion grounds, so it only comes back if
      the game's own widget is proven incapable.

- [x] **[untested] NPCs only fight the player who angered them.** Built 2026-09-23, never played. Mechanism, not mystery: combat targets belong to whoever
      owns the NPC. A crime makes the guards hostile to that player on that player's machine. The other player's
      hits arrive as `RequestHealthChangeBroadcast`, which carries only the actor id and a health delta and
      **no attacker**, so the owner never learns who hit its NPC and never puts them in combat.
    - **Fix, and it needs no new addresses.** The server already knows which player sent the broadcast: add that
      player's id to `NotifyHealthChangeBroadcast`, and on the owner's client, when an NPC we own takes real
      damage attributed to player X, call `Actor::StartCombatEx(copy of X)`. That function is already written
      (Papyrus `StartCombat` plus `CombatController::SetTarget`) and is currently dead code, never called.
    - **Cost:** one field in one message, so client and server both rebuild, and one call site.
    - **Care:** gate it on an actual health decrease, or an NPC will turn on a player who healed or buffed it,
      and rate limit it so a flurry of arrows does not restart combat every frame.
    - **Built as planned.** `NotifyHealthChangeBroadcast` carries `AttackerPlayerId`, filled in by the server from
      the sending player. `ActorValueService::StartCombatWithAttacker` on the owner's client resolves it to that
      player's copy and calls `StartCombatEx`. Damage only (the client adds the delta, so a hit is negative),
      never on a player or a dead or downed actor, only on an actor this client owns, and at most once every
      three seconds per actor. Logged as `Combat: actor X now also fights player N`, so one session says whether
      it fires. Protocol change, so client and server both move to `036b42a`.

---

- [x] **[untested] Two of the merge's missing VR addresses turned out not to be addresses (2026-09-23.)** Checked
      against the local `CommonLibVR` checkout and a fresh clone of `cmpayc/TiltedEvolutionVR`:
    - **`AIProcess::GetCharController` (AE 39856) needs no address.** In CommonLibVR it is a field read, not a
      call: `middleHigh ? middleHigh->charController.get() : nullptr`, with `middleHigh` at +0x08 and
      `charController` at +0x250. Our `MiddleProcess` now exposes that field and the function reads it. The offset
      is confirmed three ways: CommonLibVR's VR headers, the note from TiltedEvolutionVR (`aaf5d83`), and the fact
      that every neighbouring offset this struct already asserts matches that header exactly (rotation 0B0,
      activeEffects 1A0, commandingActor 218, leftHand 220, rightHand 260).
    - **The physics timestep global (AE 389089) is not needed either.** `hkStepInfo::UpdateDeltaTime` is our own
      code and only *prefers* it: it falls back to the movement delta the caller passes, then to the stored step,
      rejecting anything non-finite or <= 0.0001s. `HookActorProcess` already passes the frame delta, so VR now
      calls it with a zero first argument and gets a valid step from the fallback.
      Together these make upstream's havok fix (#901) live on VR, which is the sliding-NPC fix. Watch for: remote
      NPCs that now stand still correctly but fight their network position, and `ForcePosition` taking its
      "no usable step yet" branch for freshly created controllers, which it could not do before.
      **Both addresses arrived on 2026-09-23 anyway** and are now in `VRAddressOverrides`: AE 39856 ->
      VR 0x140685270 (`AIProcess::GetCharController`) and AE 389089 -> VR 0x141ec8278 (the timestep global,
      SE id 512261). Both builds now use the engine's own function and the real global rather than the
      reconstructions above. The field read stays behind `GetCharController` as a fallback and as a check: if the
      engine and `MiddleProcess+0x250` ever disagree, the client says so once, which is the only thing that would
      catch a wrong `charController` offset. **Still missing: AE 20231 (`TESObjectREFR::SetLeveledCreature`)**,
      which is what levelled-NPC reconciliation needs before it can be turned back on at all.
    - **`GarbageCollector::Add`: the ids were mismatched overloads.** CommonLibVR has
      `RELOCATION_ID(35492, 36459)` for `Add(TESObjectREFR*, bool)`. Upstream calls AE **36460**, the
      `TESBoundObject*` overload, and the merge paired that with SE 35492, so we called the reference overload
      with a base form and a junk second argument. The singleton is right, though: CommonLibVR gives
      `RELOCATION_ID(514180, 400329)`, exactly what we have.
    - **`cmpayc/TiltedEvolutionVR` does not have the ones still missing.** Its generated map covers the 3086 AE
      ids its own client uses; 39856, 389089, 20231, 36460 and 36459 are all absent, because that fork never calls
      them either. The one hit is AE 14375, which it derives (auto-diff) to VR `0x19C0C0` and names
      `s_SetLeveledNpc` from its own `TESNPC.cpp` call site, while upstream calls the same id
      `CreateTemplateActorBase`. Same address, two names, still unverified as the same function.
      What that fork does have is the toolkit that derives these (`Tools/vr_addresses`: derive, match, callgraph,
      candidates, pe), which is the route to the remaining three rather than hand-reading the disassembly.

- [x] **[untested] Levelled NPC reconciliation is back on for VR (2026-09-23), after being off since it crashed
      Seen on 2026-09-22.** All three engine calls are now accounted for. Watch the next join closely: this is the
      code path that crashed him, and the one part still taken on trust is `CreateTemplateActorBase`.
    - `TESObjectREFR::SetLeveledCreature`: **address supplied 2026-09-23**, AE 20231 -> VR 0x1402b8f10
      (SE 0x1402a77a0, SE id 19826). This is the step that actually applies the pick, so until now the feature
      could not have worked even without the crash.
    - `TESActorBaseData::CreateTemplateActorBase`: AE 14375 -> VR 0x19c0c0. Not proven by running it, but
      corroborated: cmpayc/TiltedEvolutionVR derived the same address independently and declares it
      `TESNPC* thiscall(TESNPC*, TESNPC*)` where upstream declares `TESActorBase* fastcall(TESActorBase*,
      TESActorBase*)`. On x64 those pass in the same registers and return the same way, and TESNPC derives from
      TESActorBase, so the two declarations are compatible. This is the residual risk.
    - `GarbageCollector::Add`: **not called on VR at all.** Upstream calls AE 36460, the TESBoundObject overload;
      the merge paired it with VR 35492, which CommonLibVR shows is the TESObjectREFR overload taking
      `(TESObjectREFR*, bool)`. We called it with a base form and a junk second argument, which is what crashed
      Seen inside `BSExtraDataList::GetExtraDataWithoutLocking`. Skipping it leaks one temporary base form per
      reconciliation, which is a much better bargain. Restore it if the TESBoundObject overload's VR address
      turns up.
      Superseded detail from when this was off:
    - `TESObjectREFR::SetLeveledCreature` (AE 20231) has no VR address. That is the step that actually applies the
      pick, so even before the crash the feature could not work: it logged
      `SetLeveledCreature has no VR address` and carried on to the rest.
    - `GarbageCollector::Add` (AE 36460, given VR 35492 in the merge) is **confirmed wrong**. It crashed Seen on
      join at 22:14:26, in `BSExtraDataList::GetExtraDataWithoutLocking`, while disposing the temporary base
      `FF000C6E` ("Reaver Highwayman") that the log had queued for reconciliation a moment earlier; the crash
      registers still held the owner's pick `301E828` ("Reaver Outlaw") and a `GarbageCollector*`.
    - `TESActorBaseData::CreateTemplateActorBase` (AE 14375) resolves to a VR address our own override table
      labels `TESNPC::SetLeveledNpc`. Plausibly the same function under two names, never verified.
      Both entry points (`CharacterService::ApplyLeveledNpcPick` and `LeveledNpcSystem::ApplyPick`) now return
      early on VR, so no actor is disabled and re-enabled for a reconciliation that cannot finish, and a levelled
      NPC that rolled differently on each side is covered the way it was before the merge, by the stand-in and
      ghost handling. To turn it back on, all three addresses have to be confirmed first; that is a job for the
      address work, not for another play session.

- [~] **[untested] Seeing the other player handle a body (HIGGS grab).** Built 2026-09-22, never played.
      It needs nothing from HIGGS and no protocol change, which is why it went in at once: `higgs_vr.dll` exports
      only the two SKSE entry points, so its `IHiggsInterface001` can only be reached through SKSE messaging, and
      guessing at the interface would crash the game. Instead the body itself is watched.
    - **Sending.** `VRBodySync::CaptureBodyPose` reads the upper-body bones of a dead actor this machine owns,
      within 600 units of the player, and `AnimationSystem::Serialize` puts them in the movement snapshot that
      actor already sends. The `VRPose` field has been on every reference update since the player pose was built,
      so nothing new travels. A body that is not moving sends nothing at all; sending carries on for two seconds
      after it comes to rest, because the receiver only buffers two points and the final pose has to be repeated
      to be the one that lands. After that the body is left to its own ragdoll on both sides.
    - **Receiving.** The frame-end pass no longer skips a dead body when bones arrive for it. Dead *player*
      copies are still skipped: posing them fights the death animation and leaves the body standing in the air.
      The bone-usability guard from the invisible-body fix applies unchanged.
    - **Ownership.** `CharacterService::RunBodyGrabUpdates`, every 250 ms: a dead body owned by the other player
      that this machine has physically moved more than 32 units from where the network is playing it back is
      claimed through the ordinary `RequestOwnership`, at most once every three seconds per body. The corpse path
      tolerates 64 units of drift before it snaps a body back, so the claim goes out before the snap. Living NPCs,
      player copies and summons are never taken this way.
      To watch for on the first session: a body that stutters between the two sides while a claim is in flight,
      a claim that is refused and leaves the grab fighting the owner, and whether the receiving side's ragdoll
      re-derives itself from physics hard enough to ignore the written transforms. That last one is the real
      unknown and is what the first test is for; if it shows, the copy has to be put into ragdoll, or its
      physics frozen, while it is driven.

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
- [ ] **[big] Merge upstream TiltedEvolution (studied 2026-09-22).** We forked at `5a99a0b6`, 28 Feb 2026.
      Upstream's branch is `dev`, not `main`.

    | | Count |
    | --- | --- |
    | Upstream commits we lack | 72 |
    | Our commits since the fork | 62 |
    | Files upstream changed | 148 |
    | Files we changed | 199 |
    | Files both sides changed | 51 |
    | Files that actually conflict (trial merge) | 26 |

    - **What we would gain, all of it things we are actively fighting:** havok corruption on remote actors leaving
      a zero timestep on new controllers, which makes them glide (`#901`, 22 Sep) and is our sliding bug, named
      and fixed; a whole leveled-NPC reconciliation system with a canonical pick, which is our wolf-on-one-side
      troll-on-the-other, and would let us delete the ghost stand-in code rather than merge it; versioned server
      ownership grants replacing optimistic client ownership, plus blacklists that no longer persist, which is our
      ownership churn; dragons failing to spawn for party members; dialogue sync when the speaker does not own the
      NPC, which is our doubled dialogue; inventory not broadcasting on pickpocket; respawn overrides for interior
      cells and the camera sticking after a respawn; separate client logs per instance; an incompatible-version
      popup instead of a silent refusal.
    - **Where it hurts:** the conflicts sit exactly where both sides did surgery. Client `CharacterService.cpp`
      (we changed 636 lines, they changed 732), server `CharacterService.cpp` (199 against 482), `Actor.cpp`
      (243 against 119), `InventoryService.cpp` (217 against 155). Both sides also changed the same messages
      (`CharacterSpawnRequest`, `AssignCharacterResponse`, `NotifyEquipmentChanges`), so the merged build is a new
      wire format: everyone updates client and server together, once.
    - **Where it does not hurt:** almost all the VR work is in files upstream never touches. `VRBodySync`, the
      pose messages, the address overrides, the crash recovery, the crime guard, the VR dashboard and the launcher
      come through untouched.
    - **Order that turns risk into deletion:** take the 97 upstream files we never touched first, which is free;
      then the ownership rework, because several of our workarounds exist only to paper over what it fixes and
      should be deleted rather than merged; then the leveled-NPC system, same reasoning against the ghost code;
      then the small havok fix. Keep the VR code as is throughout.
    - **Cost:** two to four sessions of merge work and two or three play sessions to shake out what it breaks. It
      will make the mod unstable for a few days, so not on an evening anyone wants to play.
    - **The cheap alternative, and the thing to do first:** cherry-pick `fbf72883` alone. Four files, about forty
      lines, and it targets the worst bug we have left. It will conflict in `HookActorProcess`, which both sides
      rewrote, but that is one small and understandable conflict rather than twenty-six.

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
- [x] **VRBodySync searches for bones every frame.** Remote bodies were already cached per 3D (the Rig); the
      local capture still walked the whole skeleton at every send. Cached since 2026-09-20 (dropped when the root
      or a node's vtable changes, or after 5 s).
- [ ] **Spawn bursts on cell change.** Entering an area creates dozens of actors in one frame, each
      with a full inventory rebuild (remove everything, then add and equip item by item).
    - Fix: limit spawns and inventory applies to a few per frame, and queue the rest.
- [ ] **Remote movement is shown 300 ms late** (`CharacterService::RunRemoteUpdates`).
    - Fix: base the delay on measured ping and jitter, clamped to 100-200 ms. Keep the 300 ms delay
      for NPCs if they get jittery.
- [ ] `RunNakedNPCBugChecks` looks at every actor's worn items once a second. Measure it, then limit
      it to a few actors per tick.
- [ ] The new equipment snapshot reads the whole player inventory once a second, and FUS
      inventories are big. Measured since 2026-09-20 ("equipment snapshot" in the perf line); if it shows,
      only rebuild when an equip hook fired or the hand/spell pointers changed.
- [ ] Look for other per-frame costs: linear `find_if` searches over all entities in hot paths, and
      `TESForm::GetById` inside loops.
- [x] Host PC: `host-server.ps1` starts the server at below-normal priority (2026-09-20) and says why.

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

### 2.3 VR body

- [x] **[untested] Finger and grip pose.** Since 2026-09-20 the pose carries the 30 finger bones (five fingers of
      three bones per hand) as rotations relative to the parent bone, sent only when they change or once a second
      (HasFingers); the receiver keeps the last set and writes them below the posed hand. Finger bones are
      flattened (no node), so they are found by shape in the flat bone array: a chain of three entries under the
      hand, five per hand, in array order. Both sides log whether that shape was found (`local finger bones
      found` / `finger bones not found by shape`). If a skeleton mod hangs other three-deep chains off the hand,
      the search gives up and the hands stay on the animation. Encoding change: client and server.
- [ ] **Legs when moving.** Check that smooth locomotion plays a walk or run on the remote copy,
      rather than sliding.
- [x] **[untested] Player height and scale** differences between VR players. Since 2026-09-20 the pose carries
      the skeleton root's world scale (times 1000, on change and once a second), which folds in the actor's own
      scale and VRIK's body scale; the receiver sets "NPC Root [Root]" local scale so the copy's world scale
      matches. No menu: VRIK already holds the number. Logged on both sides (`local body scale ...`, `body scale
      of remote actor ...`). VRIK's separate arm and hand scales are not carried; a second pass on those nodes if
      they show. Encoding change: client and server.
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

- [ ] **CEF crash guard audit.** Before the VR menu is created, any overlay call is a hard crash. Check
      that every `OverlayService` / `ExecuteAsync` / `CefListValue` path is guarded.
- [ ] **Intermittent crash:** a script event sent to a freed temporary reference during cell attach
      (see `KNOWN_ISSUES.md`).

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
- [x] **Update script:** `update.bat` (drag the zip onto it) swaps client and server files with the rename-aside
      trick, refuses while the game runs, finds a `Server` folder next to the client folder. Tested with a held-open
      exe. GitHub release check: not done.
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

- [x] **[untested] Friend's name and health above their head, on the game's own enemy meter.** The message was
      read off the game's own sends on 2026-09-20 18:56: an update for WSEnemyMeters carrying a HUDData with type
      0xB, the actor's level at +0x20, two flag bytes 1,1 at +0x22 and the actor handle at +0x28 (0 clears), sent
      from SE 0x1408D5130+0x284; EnemyHealth::Update then draws it. Proven in play that evening (Seen saw Emma's
      bar once), but rarely: the first rules held off for 8 s after any target the game set, and the game
      re-points on every hit, so in a dungeon it almost never got a turn. Since 21:5x the friend's bar comes up
      when you look at them (18 degrees to acquire, 32 to hold, out to 3000 units) or whenever they are under 60%
      health however you are facing, the hold-off after the game's own target is 2 s, and it refreshes four times
      a second so it does not fade. One line per change of target says which rule brought it up and how far off
      centre they were.
- [ ] Clean remote spawn: fade in instead of popping or falling. Place on the ground if the
      interpolated Z is invalid ("mammoth fell from the sky").
- [ ] Death and bleedout: HUD message "X is down", and optionally revive by activating the downed
      player. Needs a new message: a downed player bleeds out and never reports a death state.
- [ ] Sensible defaults for VR in `STServer.ini` (difficulty sync, PvP off, time scale).

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
