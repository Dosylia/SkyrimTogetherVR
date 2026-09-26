# Skyrim Together VR: TODO

## Goals, in the order they are worth fixing (set 2026-09-25)

1. **No more crashes.**
2. **The game completely in sync between all players.**
3. **VRIK interactions seen by everyone.**
4. **Dead bodies in sync at all times** -- dragging first, then handling another player's body and living NPCs.
5. **NPCs below floor level** -- they should always be in sync.
6. **VR weapon hits land where the weapon is.**

Everything below is ordered against these. An item that is shipped but has not been played is marked
`[x] [untested]` and is **not** finished; it is owed a session. Items that the people playing have confirmed are
deleted outright, not ticked -- the git history keeps them.

### Where the crash work stands (2026-09-25, end of night)

- [ ] **Correction (2026-09-26): there were no mid-session timeouts, and the churn was not a fault.**
      The reading of Seen's 21:26 bundle that said "his client dropped five times and that is what broke the
      world" was wrong, and the fixes that came out of that night stand on their own evidence rather than on it.
      What the reason codes actually mean, read from `TiltedConnect/Client.cpp`:
      - `0 kTimeout` is reported only when the **old state was `Connecting`** -- a connection attempt that never
        completed. His two at 21:16:01 and 21:16:16 are failed attempts to connect after loading a save, not a
        live connection dropping.
      - `4 kAborted` comes from `Client::Close()` -- **this client closing the connection itself**. All three of
        his were deliberate local closes.

      And the 45 actors handed back at 21:19:01 were not a fault either. His grid changes run
      (5,7) -> (6,7) -> (7,7) -> (8,6) -> (9,6) -> (10,7) -> (11,7) between 21:16 and 21:19: about 25,000 units
      across Solstheim in three minutes. The game unloaded the cells behind him and the client relinquished what
      was in them, which is what it is supposed to do. **Lydia was four cells behind him**, at x~29000 while he
      stood at x~47000, which is the whole of "Seen does not see Lydia at all" at 21:21.

      So: no timeout problem is established, and one reported symptom is explained as correct behaviour. What
      remains genuinely unexplained from that session is the bandit launched into the sky (21:19), the dead body
      in the wrong place (21:20) and the spriggan that never lands a hit (21:25) -- all of which happened while he
      was crossing cells fast, which is worth treating as the common thread instead.

      The `Silence:` line added on 2026-09-25 is kept: it costs nothing and a client that stops talking is still
      worth knowing about. Its justification is now curiosity rather than a diagnosis.


### Goal 1, continued: the diagnostics were a stall source of their own (2026-09-25)

Counted in Seen's 21:26 bundle. A client that stalls stops talking; a client that stops talking is timed out; a
timeout hands 45 actors away at once, and that churn is where the crashes and the nonsense live. So the cost of
watching had become part of what was being watched.


  What is deliberately kept: the `MessageBoxMenu` probe, because it only fires for a message box and the level-up
  and respawn boxes are still open questions; `AnimDiag` and `InterpDiag`, which are periodic summaries rather
  than per-event work; and the new `Silence`, `SinkDiag` and `BodyGrabDiag` lines, which are cheap and are the
  only evidence for three open goals.

### What is still listed here

Only this week's work is still ticked. 44 items shipped between 2026-09-14 and 2026-09-23 were deleted on
2026-09-26: they have survived many sessions without being re-reported, which is as close to "confirmed" as an
unplayed item gets, and the git history keeps every word of them. What is left ticked is from 2026-09-25 and
-26 and is genuinely unplayed -- that is the list to confirm on the next session.

## Session of 2026-09-26, 10:43-11:30 (both logs plus the server's): five causes, all named

The longest session so far and the most productive one, because for the first time the server's own log settled a
question the two client logs could only argue about.

### The invisible player is not a fade, not a misplacement. The copy is deleted and never asked for again.

Three logs, one minute, and the whole thing falls out:

    server  11:20:17  WorldDiag: player 'Queen Emma' entered interior cell 34FD2; removed for 'Seen the strong'
    server  11:20:21  WorldDiag: player 'Seen the strong' entered interior cell 34FD2; sent again for 'Queen Emma'
    Emma    11:20:21  Character with remote id 20 is already spawned.
    Emma    11:20:22  Temporary Remote Deleted FF00119B / Actor removed, form id: FF00119B
    Emma    11:20:58  MagicService::OnNotifyRemoveSpell: could not find actor server id 20
    Emma    11:21:01  ObjectService::OnActivateNotify: could not find actor server id 20
    Emma    11:21:31  (the copy is back, because a *second* load door fired the whole sequence again)

The server only re-sends a character when **that character** changes cell. It has no handler for the other case: a
player whose own cell unloads and takes every remote copy standing in it down with it. Seen's spawn arrived one
second before that teardown, hit "already spawned" and only nudged the old copy's position -- and then the old copy
was deleted. Seventy seconds of Seen being invisible, and only a second load door fixed it.

That also explains why this always reads as "the person going first cannot see the person following": the one who
goes through the door first is the one whose cell unloads.

- [x] **[untested] The client announces its cell again when it tears down a remote copy for a local reason.**
      `CharacterService::CancelServerAssignment` now calls `DiscoveryService::RequestCellReannounce`, which waits
      1.5 s (a load door deletes copies over several frames) and then re-dispatches `CellChangeEvent`. The server
      re-sends every character in the cell, onto an empty slot this time. The server's leave-cell cleanup bails as
      soon as it finds a player still in the cell, which is us, so nothing else is disturbed.

### Dwarven spheres are not fighting: the animation replay queue has no bound on the path that fills it

`ReplayDiag` prints how many actions are queued. On Seen's client a Dwarven Centurion Master reached **5,123** and
Neloth **17,411**; Emma's worst was 2,689. `AnimationSystem::Update` plays at most one action per frame, so 17,411
queued is six minutes of replay: the automaton was playing what it did before the fight started, all the way
through the fight. That is "not fighting at all".

The cap was written on 2026-09-24 and put in `AddAction` -- the single-action path. Actions also arrive in the
movement snapshot (`OnReferencesMoveRequest`) and in the spawn replay chain, and neither was bounded. The session
report's "Animation backlog trimmed 0" was true and meant nothing.

- [x] **[untested] The cap is `AnimationSystem::TrimBacklog` now and every push path calls it.** The server caps a
      replay chain at 32, well under the 96 here, so a fresh spawn is never trimmed.

### The health bar after a death: the copy is pinned at the floor and nothing ever lifts it

Seen died at 10:48:29. His copy on Emma's side was held at 25 health by the "a copy never goes down" rule while his
own went to zero, the copy was rebuilt at 10:48:43, a stale death delta drove it to -275 and it was floored to 25
again -- and then it **stayed at 25 for ninety seconds**: 25 at 10:48:46, 66 at 10:49:16, 66 at 10:49:46, 230 at
10:50:16. No correction line in any of it.

Because only *changes* go on the wire. Seen's health went -275 -> 315 while Emma had no copy of him to receive it,
and after that it did not change, so nothing was ever sent again. What Emma saw was the copy's own regeneration
crawling the bar back up, which reads exactly like watching somebody heal.

- [x] **[untested] Health, magicka and stamina are re-sent every 3 s for a player whether or not they moved.**
      Three floats per player per three seconds, and every copy becomes self-correcting: a missed change, a floored
      value, a copy that regenerated on its own, all repaired within one period instead of never.

### The friend's health bar "needs to be initialised once", measured

Emma's words, and the count behind them: the session started at 10:43:50 and `WSEnemyMeters` first appeared in the
menu list at **10:47:58**, four minutes later. It was missing from 205 of 575 probes -- a third of the session.
The crash fix of 2026-09-25 refuses to post to a menu that is not open, correctly, and says nothing; the meter is
not part of the HUD until something has shown it once, and until then there is no bar over anyone's head.

- [x] **[untested] When the meter is wanted and the menu is not there, the UI is asked to show it.** `kShow` with
      no data, at most once every 2 s, which allocates nothing from the pooled factory -- that allocation is what
      made the update dangerous. The next tick 250 ms later finds the menu open and posts the real target.

### Dragging a Dwemer automaton did nothing, and could not have

`CaptureBodyPose` starts with `FindBones`, which searches by human bone name ("NPC L Hand [LHnd]" and the rest). A
Dwarven sphere, a spider or a centurion fails it, the function returns false, and **nothing whatever is sent** --
not the bones, not even where the thing is.

- [x] **[untested] A body with no readable skeleton sends its root position alone (`VRPose::NoBones`).** The
      receiver shifts the whole node tree, and the flattened bone array with it, by the difference: rotations are
      left to the local animation, only where it is changes, which is all a drag is. Added in all four places this
      time -- message, sender, interpolation rebuild, receiver.
- Only for a **dead** automaton: the sender's gate is `IsDead()` and within 600 units, because a living NPC has no
      ragdoll to grab. If the spiders being dragged were alive, this changes nothing for them.

### Sliding: the measurement was not measuring what I thought

100% on Emma's client (54,392 of 54,392) and 99.2% on Seen's. The instrumentation added the same morning named
four early returns out of `SaveAnimationVariables` and fired **zero** times, which I was about to read as "the
reads are working".

It is not what it meant. The two outermost conditions -- no animation graph manager, and an index past the end of
the graph list -- skip the whole function without touching any of the four, and an untouched `AnimationVariables`
is an *empty* one, which compares equal to the last empty one. So "identical" may mean "unchanged" or may mean
"never filled", and which it is settles the diagnosis.

- [x] Every exit is named now, including those two, and the successful path reports how many booleans, floats and
      integers it read. **A zero there means the sender never fills them; a healthy count means the sender is fine
      and the fault is downstream of it.** One session answers it.
- [x] Four of those early returns leaked a `BSAnimationGraphManager` reference every time they fired -- the
      function's own exit releases it and they returned straight past it. They release it now.

### The script extender warning, finally read rather than guessed at

`Looked for 'sksevr_.dll', built from exe version ''`. Not a trimming bug this time: `VersionDb` has no loaded
version string at that point, so there was nothing to build a filename out of at all. Both failures share a cause
-- guessing at a filename the loader can simply be asked for.

- [x] **[untested] The loaded modules are enumerated and the one called `sksevr_*.dll` is found by prefix.** No
      version string involved. It logs which file it found, so the next log says so in one line either way.

### Emma's crash at 11:30:43: not named, and not guessed at

`c0000005, execute at 0x0` -- a call through a null function pointer, from `SkyrimVR.exe+0x3b2823`, with four
frames in the same `+0x3b0xxx` range (a recursive traversal) and **no mod frames on the stack at all**. It is on
thread 16928, the renderer frame-end thread where the body sync runs, 0.7 s after that thread posed a remote body.
That is suggestive and it is not evidence; the game exe is Steam-encrypted here so the caller cannot be named
offline. Two remote actors had been deleted in the seven seconds before it (FF001193 at 11:30:36, FF001191 at
11:30:39), which is the shape of a use-after-free but not proof of one.

- Left open deliberately. Nothing shipped for it this round.

### Goal 1: the "drives a game menu directly" class is closed (2026-09-26)

The enemy-meter crash was not a null pointer, it was **building a UI message by hand and posting it to a menu
that was not there**. Swept for others of the same shape: `SetEnemyMeterTarget` is the only place in the client
that constructs a `HUDData` and posts it. Everything else that talks to the HUD goes through
`Utils::ShowHudMessage`, which calls the game's own notification function -- the sanctioned path every mod on the
list uses, and not ours to second-guess. One member, fixed, class closed.

### Goal 2: a copy that goes out of range is never put right when you come back (2026-09-26)

Found by a new bot test rather than by a session, and confirmed in the server's own log.

**The chain.** The server range-filters updates by grid: `uGridsToLoad` is 5, so `IsCellInGridCell` allows two
cells. A bot walked five cells away and the server reported `0 character updates sent, 321 withheld` -- correct,
and the point of range filtering. Meanwhile the real actor carries on being moved by its owner. Walk back, and
the server re-sends the spawn to put the copy right. **The client threw that away**: the copy had never stopped
existing, so the spawn hit "Character with remote id N is already spawned" and nothing moved. Those warnings are
all over Seen's logs.

A copy left where it was last seen, while the real thing moved, is a body in the wrong place and a follower that
looks frozen -- which is two of the four reports from 21:19-21:25 on 2026-09-25.

- [x] **Proved on the server side (2026-09-26).** `-Pair range` now has one bot lose health *while out of
      range*, where nothing can be sent about it, and requires the other side to know the new value once it walks
      back. It does: the watcher's record shows `spawn 1 health 55.0` on the return. So the server re-sends a
      character with its **current** values when it comes back into range, and the only thing standing between
      that and a correct copy was the client throwing the message away.

- [ ] **The server's two range paths are asymmetric, and the other half is still open.**
      - When the **character** moves out of a viewer's grid, `OnCharacterExteriorCellChange` sends that viewer a
        removal. That half works.
      - When the **viewer** moves, `PlayerService::OnShiftGridCellRequest` re-sends what is now in range and
        simply `continue`s past what is now out of it -- **no removal is ever sent**.

      So a viewer keeps copies of everything it walked away from. For a player that self-corrects, because the
      other player moving fires the first path. For anything that stays put -- a corpse above all -- it never
      does. The client-side refresh above makes the stale copy correct itself on return, which is the safe half of
      the fix; sending removals is the other half and is **not** done yet, because removals are entity churn and
      churn is where the crashes have been.

### Goal 2 / the flying bandit: two points that are not a journey (2026-09-26)

**Correction to the entry written earlier today.** That one said `delta` could go negative and walk an actor
backwards without bound. It cannot: `aTick - first.Tick` is **unsigned**, so a playback tick behind the oldest
point wraps to an enormous positive value and the existing upper clamp catches it. `Lerp` with delta in [0, 1]
can only ever place an actor *between* the two points it is given. The lower clamp was added anyway and is
harmless, but it fixed nothing, and the explanation that came with it was wrong.

Which leaves the real question: if the actor can only be placed between two points, then two points were
875,458 units apart.

They were. **The buffer records no worldspace and nothing clears it when one changes.** A point from before a
worldspace change and one from after sit side by side, and the actor is walked between them -- Solstheim to
somewhere else, two hundred cells, in a tenth of a second. 195 actors displaced at once, at 19:04:11 on
2026-09-23, is every actor in the world at the moment somebody crossed a boundary.

- [x] **[untested] `AddPoint` drops what is buffered when the next point is more than a cell away.** A cell is
      4096 units and points arrive about a tenth of a second apart, so nothing legitimate moves that far between
      two of them: a worldspace change, a fast travel, or bad data. The actor arrives at the new place instead of
      flying to it. One rule covers all three, and it needs no new field on the wire.
- [x] **The guard says when it fires (2026-09-26), because the reason for it is a hypothesis.**
      `JumpDiag: a buffered point was N units from the one before it, at (x, y, z); dropped the buffer so the
      actor arrives instead of flying. K since the last line.`
      **How to read it:** cross-check the times against somebody changing worldspace. If they line up, the cause
      is confirmed. If they do not, the guard is still right -- two points no actor could travel between should
      never be interpolated -- but the reason written here is not, and the real one is still out there.
- **What was checked and did not hold up.** The save load at 19:04:20 on 2026-09-23 was suggested as the trigger;
      it came **nine seconds after** the report, which covers the preceding ten. So the load did not cause it.
      The window itself shows a flood of "Death sync: health broadcast killed actor 39FB7 (now dead: false)" and
      an ownership transfer carrying `worldspace: 0, cell: 0, position: (0, 0, 0)` -- a null position, which the
      new guard also catches, since Solstheim to the origin is about 47,000 units. Whether the 875,458 was a
      worldspace change, a null position or something else is **not established**, and the guard does not depend
      on knowing.
- **Not bot-testable.** The bug is in the client's interpolation, which the bot does not implement -- it reads
      positions, it does not smooth between them. The suite confirms the guard does not disturb ordinary movement
      (the range test walks 20,000 units in ~300-unit steps), and that is all it can say.

### The next session's logs read themselves now (2026-09-26)

Eight measurements were shipped this week and none of them was worth much if reading them meant grepping for an
hour. `Tools\VR\session-report.py` now knows all of them and, more to the point, says what each **number**
means rather than whether the line appeared.

Running it on the 21:26 bundle immediately paid for itself twice:

- [ ] **An actor was found 875,458 units below where it had last been placed** (2026-09-23, character 30195BD;
      2,348 units on Lydia in the same session). Goal 5 has had evidence sitting in the logs for three days.
- [x] **And it caught me writing a false conclusion.** The report first announced that VR archery never sets the
      attack state -- states 0 and 2 only -- which would have meant the nocked arrow must be attached by hand.
      Then: whose projectiles were those? The player's own launches in that window are `weapon 0, ammo 0,
      spell 12FD0`. **Seen cast spells and never drew a bow.** The absent bow states meant nothing at all.
      The analyser now checks for a bow shot before drawing that conclusion and says INCONCLUSIVE otherwise. A
      tool that states a verdict the data does not support is worse than no tool, because it is believed.

- [x] **I had also duplicated a diagnostic without checking.** A `SinkDiag` has existed since 2026-09-23,
      measuring the same thing with different words; mine, added on 2026-09-26, sat five lines below it. The
      original survives -- its numbers are comparable across sessions, which is the whole point of a measurement
      -- and has gained the `has controller` field that mine had.

### A new field in VRPose must be added in three places, not two (2026-09-26)

**The hip sync shipped on 2026-09-25 has never done anything, and I said it was working.**

`InterpolationSystem::Update` does not hand the received pose to the body sync. It **rebuilds** one from the two
buffered points, copying the fields it knows about: bones, fingers, root position, scale. `HasHips`/`HipOffset`
and `HasHandCheck`/`LeftHandOffset`/`RightHandOffset` were added to the message, to the sender and to the
receiver -- and not to that rebuild. So they were dropped between the wire and the body, silently, and the
receiver simply never saw them. Nothing logged, because the code that logs runs on the rebuilt pose.

That is why the session report reads `Hips (receiver) 0` and `Hands compared 0` while the sender is filling both.

- [x] **[untested] Both are carried through the rebuild now.** Hips are blended like the movement they are (the
      whole body is offset by it, so a step in it is a step in the body); the hand measurement takes the newest
      point that has one, because averaging two readings of where somebody's hand was would agree with neither.
- **The rule this leaves:** a new field in `VRPose` needs the message, the sender, **the interpolation rebuild**
      and the receiver. Three of the four are obvious and the third is not, and missing it fails silently in the
      most expensive way -- a feature that is shipped, believed, and does nothing.

### Goal 2: a corpse that still walks, and why (2026-09-26)

A death is broadcast to **every** player rather than only those in range, deliberately, so that nobody holding
the actor can miss it. The receiving client then applies it only when the copy's ownership epoch matches the
message's -- and `NotifyOwnershipTransfer` **is** range-filtered (`SendToPlayersInRange`). So:

1. A player walks out of range.
2. The actor changes hands. That player never learns the new epoch.
3. The actor dies. The death reaches them, and their own handler throws it away, because their copy's epoch is
   one behind.
4. They walk back. The epoch is refreshed by the re-sent spawn -- but the death is long gone.

They are left with a corpse still walking around, for the rest of the session.

- [x] **[untested] The stale-copy repair now applies death state.** The spawn is the last thing that still knows,
      and it carries `IsDead`. Position, health, weapon state, factions and death are all repaired now; inventory
      is still only compared.
- [x] **The arrival-side drops are counted now (2026-09-26), so the next session decides this instead of me.**
      A dropped message and a character we simply do not have were indistinguishable from inside those handlers:
      both ended in the same `return`. The id lookup and the epoch test are separate now, and a message for a
      character this client *does* hold, refused only for its epoch, says so:
      `EpochMiss: dropped a death state change for character N -- the message is epoch 3 and this copy is epoch 2.
      K of these since the last line.` Wired into actor values, the health broadcast, death and equipment.
      **How to read it:** none at all means the equality test is harmless and should stay. A steady drip -- above
      all on death -- means copies are going stale in normal play and the test should become "not older" rather
      than "equal".
- [ ] **The receiving-side epoch equality check is the underlying issue and is left alone for now.** Six handlers
      require the copy's epoch to equal the message's -- health, death, equipment, inventory. The server has
      already checked the sender's ownership before broadcasting, so re-checking on arrival mostly provides a way
      to drop valid state. A monotonic test (accept anything not *older*, and take the newer epoch) would be
      better than equality. Not changed without evidence: the equality check presumably exists to reject messages
      that overtake a transfer, and the spawn refresh does heal the gap on return.
- [x] **`-Pair deadfar`**: one bot walks five cells out, dies where nobody can watch, and walks back. The other
      must have it dead. Passes.

### Goal 2: factions are the third thing repaired on a stale copy (2026-09-26)

- [x] **[untested] A copy that comes back into range now gets its factions put right.** Faction changes are
      range-filtered like health and equipment, so a character that turned hostile while this client was away is
      still friendly on the copy -- and a copy in the wrong faction is a creature that squares up to somebody and
      never swings. That is the shape of the Burned Spriggan of 2026-09-25, which spent a fight trying to attack
      Lydia and never landing one. The stale-copy gate now repairs position, health, weapon state and factions;
      inventory is still only compared.

### The pairs run five at a time now: 400 s to 277 s (2026-09-26)

- [x] **`Tools\VRun-all-pairs.ps1`.** The pairs ran one after another because a watcher resolves "other" to
      whoever else is in the world, so two pairs on one server would see four bots. They do not have to share a
      world: the server's range check returns false for a different worldspace before it looks at anything else,
      so a pair in its own worldspace is invisible to the others. Same scripts, same assertions, wall time only.
      Batched three pairs at a time, because `GameServer:uMaxPlayerCount` is 8 and its own description says going
      above that is not recommended -- that setting belongs to the server people play on, so the batch is sized to
      it rather than the other way round. **Whole suite: 277 s for ten scripts, from 400 s for nine.**

- [x] **The bot was inventing characters out of other people's deaths.** Running five pairs at once turned up a
      third copy in `churn2` -- `1 health unknown`, no name. A death is broadcast to **every** player rather than
      only those in range, deliberately, so that nobody holding the actor can miss it; a client that does not hold
      it is meant to ignore the message. The bot's handlers called `Actor(id)`, which *creates*, so every death
      anywhere in the world conjured a character it had never been given. Same for value and equipment notifies.
      Only a spawn may invent a character now; the rest update or return.
      Worth noting what this nearly became: the first reading was "a removal is being missed under load", which
      would have been a product bug reported on the strength of a test artifact. The three copies were named in
      the report, and their names said otherwise.

- [x] **The version pre-flight is real now.** It was added on 2026-09-25 and never wired in, and skew cost time
      twice more the same day -- a client built mid-change while the server stayed behind. Both harnesses now
      compare the bot against the server's log before running and say exactly what to rebuild.

### The silent drop is now impossible to miss (2026-09-26)

Five bugs of one shape in a fortnight -- four in the bot, one in the client -- all invisible for the same
structural reason: **the protocol has no negative acknowledgement.** The send succeeds, the server discards the
message, and the only symptom is that the world quietly disagrees. Fixing each instance does nothing about the
sixth, so the shape itself is what got fixed.

- [x] **The server says when it throws a message away.** `ReportEpochDrop` in `Code/server/EpochDrop.h`, rate
      limited per kind: *"Dropped an actor value change for actor 1: the sender's ownership epoch is 2 and this
      server holds 1. Nothing was applied and the sender was not told."* Wired into actor values, max values,
      death state and equipment.
      The equipment one already logged -- at `spdlog::debug`, which never reaches the file. A silent drop whose
      only record is an invisible log line is not a diagnostic.

- [x] **The harness fails a run in which the server threw anything away.** This is the part that matters.
      Proved by breaking the bot on purpose: with a deliberately wrong epoch, **`health-sign.txt` still reported
      "1 of 1 runs passed"** while every health change was being rejected, because the script asserts on the
      bot's own bookkeeping rather than on what arrived. The harness now reads the server's log for the run and
      calls that a failure. Restored, and the whole suite is clean.

      So a test can no longer pass while the server is discarding the very messages it is meant to be exercising.
      That was true of `health-sign.txt` from the day it was written, and it is the third time this month a green
      result has turned out to mean nothing.

### Swept and clean, so nobody looks again

- Every `Notify*` the server sends has a client sink, except `NotifySetTimeResult` (an admin command's reply,
  nobody listens, harmless).
- No `Request*` the client sends is unhandled by the server. `RequestObjectInventoryChanges` is vestigial: an
  `#include` in the client and a forward declaration on the server, never constructed.

### The same rule, turned on the real client -- and it found one (2026-09-26)

The epoch trap caught the bot four times, which made it worth asking whether the **client** has the same hole
anywhere. It does, or did: one of the seven request messages that carry an epoch.

- [x] **[untested] The periodic equipment snapshot has never reached the server.**
      `InventoryService` sends `RequestEquipmentChanges` from two places. The event-driven one, when the player
      equips something, sets the epoch. The **snapshot** -- the safety net that sends the whole worn inventory
      when it notices it has drifted -- did not, and `OnEquipmentChanges` refuses any equipment change whose epoch
      does not match the owner's. It logged "Equipment snapshot sent: N worn entries" every time and the server
      threw every one away.

      This is the mechanism that is supposed to repair equipment when a change event has been missed, and missing
      change events is exactly what happens while a player is out of range, because equipment travels in range
      only. So the repair for the "not swapping weapons" family existed, ran, logged that it had run, and did
      nothing at all.

      The other six are correct; each was checked individually rather than counted.

### The epoch rule, swept to the end (2026-09-26)

Every `Request*` message the bot sends was checked against the rule rather than waiting for the next one to bite.
Seven request messages carry an `OwnershipEpoch`; the bot sends four of them.

- [x] `RequestActorValueChanges` -- was dropped, fixed 2026-09-25.
- [x] `RequestDeathStateChange` -- added with the epoch, 2026-09-25.
- [x] `RequestEquipmentChanges` -- was dropped, fixed 2026-09-26.
- [x] **`RequestOwnershipTransfer` -- was dropped, fixed 2026-09-26.** The bot logged "Server handed us actor X;
      handing it back (a bot never owns anything)" and the hand-back was refused every time, so the bot went on
      owning every actor it had just announced it was refusing. **Not exercised by the suite**: a world of bots has
      no NPCs, so the server never hands one over. The fix is right -- `OnOwnershipTransferRequest` demonstrably
      requires the epoch -- but it is unproven until a bot runs beside real players.
- `RequestHealthChangeBroadcast` carries no epoch by design: the whole point is hurting somebody else's actor.
- `RequestInventoryChanges`, `RequestActorMaxValueChanges` and `RequestOwnershipClaim` carry one, and the bot does
  not send them. Anything that starts sending one needs the epoch first.

### The harness now names a version skew instead of calling it a failure

- [x] A bot refused at the door exits 2 with no checks run, and the summary said "0 of 1 runs passed" -- which
      reads as a broken test and is a stale build. It cost time twice. Exit 2 now says so explicitly, and says
      what to do: rebuild the server, the runner and the bot together.
      The cause both times was binaries built minutes apart while the working tree moved between them. The
      version is a hash of the uncommitted diff, so it is only stable while nothing changes -- which means the
      three binaries have to be built in one go, not one at a time as the work proceeds.

### Bot: equipment, and the same silent-drop trap for the third time (2026-09-26)

- [x] **Every equipment change the bot ever sent was discarded by the server.** `RequestEquipmentChanges` carries
      an `OwnershipEpoch` and the bot did not set it, exactly as with `RequestActorValueChanges` on 2026-09-25.
      That is the same trap in a third message, and it is worth naming as a pattern: **anything the bot asks the
      server to change needs the epoch, and the server drops it silently when it is missing.** Any new bot command
      that sends a Request* message should be checked against that before it is believed.
- [x] **The bot can hear equipment now.** It could send changes and never receive one, so nothing could be
      asserted about whether they arrive. `NotifyEquipmentChanges` is tracked per actor and scripts can ask
      `equipped <who> <baseId>`.
- [x] **Conditions can be negated: `not <condition>`.** Without it a script could say "the sword appeared" but
      never "the sword was put away", which is half a test -- and the equip test had exactly that hole in it,
      complete with a log line claiming to check something it did not.
- [x] **`-Pair equip`**: one bot draws a sword and sheathes it, the other must be told about both. Equipment
      travels in range only, the same filter that made a returning player come back with the wrong health, and
      nothing had ever checked that it arrives even at close range. It does.

### Bot: it can finally test range at all


### Goal 4: dead bodies. The cause is known and needs no further logs.

- [ ] **Dragging a body you do not own is invisible to everyone, by construction.**
      The send path is gated on ownership -- `AnimationSystem` reads a dead actor's bones only for "a body this
      machine owns". The thing that used to claim a body you had grabbed, `RunBodyGrabUpdates`, was **removed on
      2026-09-24** because its test ("a dead body more than 32 units from its network position") is true of
      corpses that have merely settled: it fired 207 times across 58 bodies in one session and crashed the
      receiver. Nothing replaced it. So a corpse owned by the other player can be dragged all night and nobody
      else will see a thing.
      This is the 20:45 Lurker of 2026-09-25, and it explains it without Emma's log: Seen joined first and owned
      most of the world, including, in all likelihood, that Lurker.

      **The fix is a claim, and the claim needs a test that cannot false-positive.** A settling ragdoll comes to
      rest in a second or two; a body being dragged keeps moving for as long as someone drags it. So the
      candidate is *sustained* motion within arm's reach, not distance from a network position.
      **Measurement shipped, claims nothing:** `VRBodySync::ObserveRemoteBodyMotion` watches dead bodies this
      client does not own, within 400 units, and logs
      `BodyGrabDiag: remote body N has been moving here for M ms, D units travelled, P from the player`
      after a full second of continuous movement. One session says how often that is true when nobody is
      grabbing anything -- which is the number the old version never had -- and the threshold follows from it.
      Only then does a claim go in, and it goes in rate limited.

### Goal 5: NPCs below floor level

- [ ] **Never measured until now.** `ForcePosition` puts a remote actor exactly where the network says on every
      frame, so an actor found *below* that target on the way in has sunk locally between frames -- which is the
      bug exactly: the owner sees an NPC on the floor, the other player sees it in the cellar. An owner genuinely
      falling drags the target down too, so a real fall shows no gap; only a local divergence does.
      **Measurement shipped:** `SinkDiag: N remote actors were below where the network put them; worst X (name)
      by D units, has controller yes/no`, every 10 s.
      **Second measurement, 2026-09-26.** `Actor::ForcePosition` gives up on moving the character controller when
      physics step timing is not ready yet, on the stated assumption that "interpolation will catch the controller
      up once physics timing becomes available". That is an assumption, and an actor below the floor is what it
      looks like when it is false: the reference sits where the network says while the controller, which is what
      keeps a body on the ground, is somewhere else. `ControllerDiag: N of M remote placements could not move the
      character controller with the reference`, every 10 s. Occasional is a controller waiting a frame for its
      first step, which is what the code expects. Consistently high is the assumption failing, and then SinkDiag's
      `has controller` field says for which actors.

      **The other suspect is already in the code.** `HookSetPosition` moves every non-player actor with
      `aUpdateCharController = false`, under a comment reading "It just works TM". The character controller is
      what keeps a body standing on the ground; moving the reference without it is a good way to end up under the
      floor. The `has controller` field in the line above is there to test that idea against a real session
      before anything is changed, because that flag is upstream code and flipping it blind is how a night gets
      lost.

### Test suite

- [x] **Faster again, and steadier with it (2026-09-26).** The two long fixed waits are gone: a watcher that
      sits for 42 seconds and then looks is slower *and* flakier than one that waits for the thing it came to see.
      - `range`: the health the other bot lost while out of range can only arrive on the spawn the server re-sends
        when it walks back, so waiting for that value **is** waiting for the round trip.
      - `churn2`: "present" looks the same mid-churn as after it, so the two halves now agree on a marker -- the
        acting bot sets health 77 when its last round is done, and the watcher waits for that. 65 s to 47 s.
      - `--host-timeout` is 2 in the harness rather than 8: that timeout is how long a bot looks for a **human**
        host before going in standalone, and in a harness run there is never a human.

      Nine scripts including five two-sided ones: **410 s**, against 415 s for seven before. `range` and `churn2`
      were each run three times in a row afterwards to confirm the tighter timing did not buy flakiness.

- [x] **Faster with no loss of coverage (2026-09-25).** The harness waited a fixed 5 s for the server and 14 s for
      the watching bot; both are polls now -- the port, and the bot's own "In the world" line. `-Script` takes a
      list, so `-Script "a.txt,b.txt"` runs both against one server and one watching bot instead of standing the
      whole thing up again per script. Whole suite, seven scripts including three two-sided ones: **345 s**.
      The rest is scripted scenario time, which is the coverage itself, so nothing further was trimmed.


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

- [ ] **Different animals on each screen.** The owner's base form now travels on both spawn paths
      (`CharacterSpawnRequest.BaseId`, `AssignCharacterResponse.BaseId`). Same name or same race: synced normally.
      A different creature (fox at a rabbit's reference, spider at a bear's): adopted and positioned by its owner,
      but the owner's animation data is withheld (`ForeignGraph`), so it slides instead of freezing. Refusing it
      instead was tried on 2026-09-18 and gave each player private bandits; never again. What remains is
      ownership: the bear the friend was fighting vanished when the host walked out of range and dropped it
      (see the ownership rework below). Levels differ between variants; cosmetic.
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
- [ ] **[untested] Whiterun gate: is the second vanish cause the worldspace change?** Logging in place on both
      sides (`WorldDiag` on the server for every remove and re-send of a player's copy on a cell or worldspace
      change, and for every grid shift; `WorldDiag: server removed player copy` on the client with our worldspace
      and cell). Test: run `STBot.exe scripts\stand.txt`, then walk through the Whiterun gate and back out. The
      copy must go when you are inside and come back when you are out. If it does not come back, the server
      log names the decision that withheld it.
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
- [ ] **Quest dialogue heard twice (15:05).** Upstream syncs the other player's dialogue lines and subtitles; with
      both in the same conversation each hears both. Decide: only the speaker's own conversation.
- [ ] **Grey hills (15:01).** Missing distant terrain textures; a screenshot exists. Likely the game's LOD stream
      under memory pressure, not sync. Check once with the game alone.
- [ ] **Nocked arrow not shown** on the other player's bow (the arrow leaves fine). The nocked arrow is an
      animation attachment and VR players never play the draw animation.
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

## 0. Session of 2026-09-25: the bot was testing itself, and one real bug fell out of fixing that

No play session. Everything here was found and proved with two bots and no headset.

### The bot's own tests were not testing the server

Every change the bot asked the server for -- health, and therefore death -- was **silently discarded**. The server
checks `IsCurrentOwner(player, epoch)` on each request and rejects `epoch == 0`, and the bot never set one. The
value it then asserted on was its own bookkeeping, recorded locally when it sent the message. Nothing crossed.

That means `health-sign.txt`, `death-recovery.txt` and `auto-suite.txt` proved only that the bot could add and
subtract, from the day they were written until today. They pass now for a better reason, but `expect health me`
is still a local read: **the only script that tests the wire is `relay-watcher.txt`**, which asserts solely about
the other client.


### Now verified across the wire, having previously only been asserted


### The bug that came out of it


### Session of 2026-09-25, 21:15-21:26 (Seen's logs): one cause behind most of it

Reported: a bandit launched into the sky (21:19), a dead body in the wrong place (21:20), Lydia not visible at
all and frozen (21:21), a Burned Spriggan trying to attack and never landing one (21:25), and the health bar
working perfectly. Almost all of it is downstream of one thing.


- [ ] **Why does a fresh connection time out at all?** This is now the top question. A client that has just
      joined takes the whole world at once -- 30 to 40 `Spawn Actor` lines land in a single millisecond -- and the
      one that joined first owns the most, so it has the most to hand back when it stalls. Nothing here measures
      how long that burst takes.
      **Plan:** time the spawn burst and the authentication-to-first-update window, and log the gap between
      updates on the client when it exceeds a second. Until that exists, every symptom above can recur and the
      logs will only show the aftermath.

### Session of 2026-09-25, 20:44-20:51 (Seen's logs)


- [ ] **The dragged Lurker was not seen (20:45).** Seen's log has **no `posing body` line anywhere on
      2026-09-25** -- the last are from the day before -- so no dead-body pose reached him at all this session.
      What his log cannot say is whether Emma sent any. It also shows both Lurkers 2.3 to 3.5 cells away and
      `InterpDiag starved` (stale 3 s, then 11 s, then 16 s), so the server may simply have been withholding
      their updates for range, in which case there was nothing to see and nothing is broken.
      **Next:** Emma's log for the same minute. The sender's line is the other half of `posing body`. If she sent
      and he did not receive, it is the range filter; if she never sent, it is the capture side.

- [ ] **Lydia, downed, lying sideways on the floor and running (20:50, Seen's side).** Lydia (`A2C94`) is Emma's
      actor, so Seen has a copy. Nothing in his log names her at 20:50, but the animation diagnostics for that
      minute are loud: `moveStart 16/27 failed`, `turnStop 13/23 failed`, `CyclicCrossBlend 8/15 failed`, and
      `(no event) 99/99 failed`. A copy playing a run while its body is prone is the shape of movement arriving
      for an actor whose own state says it is down.
      **Next:** log bleedout and get-up transitions for remote actors, with the state at the moment movement is
      applied. Nothing built yet -- the fix of 2026-09-24 made bleedout behave like dying in `HookActorProcess`
      and `InterpolationSystem`, and this may be that change's other side.


### VRIK and HIGGS interactions the other player cannot see (2026-09-25)

The general complaint, and it is one problem wearing several hats: **this mod replicates the body, and the things
the body does to the world travel only where something was built for them specifically.** Bones, fingers, scale
and now hips cross. A shot arrow crosses because a shot is its own projectile message. A dragged corpse crosses
because that was built by hand in September. Everything else a hand does is invisible.

- [ ] **Nocked arrow not shown on the other player's bow.** Cause found 2026-09-25, not a mystery any more.
      The arrow *leaving* works because `CombatService` sends a `ProjectileLaunchRequest` and the other side spawns
      a projectile directly -- the bow animation is not involved at any point. The arrow *on the string* is an
      animation attachment, and the remote copy's attack state never leaves 0 because nothing syncs it. The game's
      own value for this is `kBowAttached`, in bits 28-31 of `ActorState::flags1`, now readable as
      `ActorState::AttackState()`.

      The open question is whether VR archery moves that state at all, since a VR player never plays the draw
      animation. **Measurement written, not yet in a build:** `RunLocalUpdates` logs `VRArchery: local attack
      state N` on every change.
      - If the state does move through a draw and release, sync it and let the receiver's graph attach the arrow.
      - If it never leaves 0, there is nothing to replicate and the arrow has to be attached on the receiving
        side by hand, which is a much larger job -- and the state would then be wrong for bashing and blocking
        too, which is worth knowing either way.

- [ ] **Objects moved by hand are not seen moving.** `ObjectService` syncs activation, locks and script
      animations, and **no positions at all**. Pick a cup up with HIGGS, throw it, and on the other screen it never
      left the table. The identity and ownership machinery is already there -- objects have server ids and an
      `ObjectComponent` -- so this is a missing message rather than a missing subsystem.

      **Measurement written, not yet in a build:** `ObjectService::OnUpdate` samples every tracked object every
      250 ms and logs `ObjectMove: <formid> moved N units in 250 ms, D from the player` for anything past 8 units,
      at most four per sample.
      It answers the three things a design needs: whether objects a player handles are even in the set the server
      assigned us, how often they move, and by how much. Deliberately measured first -- the corpse-drag detector
      of 2026-09-22 was written on an assumption instead and fired 207 times across 58 bodies that had merely
      settled, which ended in a crash.

- [~] **Dragged corpses** -- built 2026-09-22, offset fixed 2026-09-24, still unplayed. See the HIGGS grab section
      further down. This is the one member of the family that already has an implementation.

### Full body tracking: hips (reported 2026-09-25, feet track and hips do not)


### Removal and ownership churn: checked, and clean


### Same shape, not touched, needs evidence first

- [ ] **`CharacterService::OnRequestRespawn` does not restore stored health either.** When somebody who is *not*
      the owner asks, that handler serialises the character out of stored state and sends it as a fresh spawn --
      the same copy-from-storage path the player respawn bug travelled on. For players it is now safe, because
      `PlayerService` restores the health before the notify goes out. For an **NPC** resurrected by its owner
      nothing restores it, so the copy other clients build should arrive at the health it died on.
      Not changed: no session has shown it. What would show it -- a resurrected NPC (a follower getting back up,
      a scripted resurrect) appearing on the other screen as a corpse or at a wrong health. If that is seen, the
      fix is the same fifteen lines. The reason for holding off is that a respawn there is not always a death:
      forcing full health would be a guess about every other caller.
- [ ] **`NotifyRespawn` in `PlayerService` is sent inside the inventory lookup**, which reads as a bug and is not
      one: `CharacterService` emplaces an `InventoryComponent` on every character unconditionally, so the lookup
      never fails. Checked 2026-09-25, left alone deliberately. Noted so it does not get "fixed" twice.

### Harness

- **`STServer.dll` and `SkyrimTogetherServer.exe` are version-locked.** Rebuild the DLL alone and the server exits
      immediately, code 1, no log line, no message. Always build `SkyrimServerRunner` with it.
- **Any tracked source edit changes the version hash**, so `STBot` must be rebuilt too or it is refused at the
      door. The refusal is now loud, but the rebuild is still manual.

### Still blocked on a play session

Unchanged from 2026-09-24: the invisible-body render-word comparison, the hand-offset reach comparison, the PvP
health-bar test (now with a named suspect), and humanoid corpse dragging. Nothing below needed a headset today.

## 0. Everything from 2026-09-24, ordered. Nothing dropped.

Ordered by what costs a session first, then by how cheap the fix is. "Debug plan" means there is no fix yet and
the task is to find the cause rather than guess at one.

### Shipped today, all unplayed. Confirm these before anything new goes in.


### 1. Costs a session, cause known, do next


- [ ] **The `Unequip` loop itself.** Capping the queue treats the symptom. Something makes one actor emit the same
      unequip action hundreds of times; the suspect is the once-a-second naked-NPC re-equip check fighting whatever
      unequips it, and `Actor::IsWearingBodyPiece` was rewritten during the merge to read container flags.
      **Plan:** on the owner's side, log each time the re-equip check acts on an actor, with what it saw and what
      it did. One session then says whether our check is the source. Cheap, and it is the last known cause of the
      stream starving.
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
      **Measurement shipped first (2026-09-25), because the cause is not known.** Three things can move the copy's
      hand away from where the sender had it, and they need different fixes: the skeleton root sitting at a
      different height (VRIK height calibration moves it by translation), different bone lengths (a different
      skeleton, or a scale this does not capture), or VRIK moving the hand itself by translation, which rotations
      can never reproduce. `CopyDiag` now prints `copy reach [...]` and `player reach [...]`: the skeleton root's
      local Z, the head and both hands' heights above the 3D root, and the upper-arm and forearm lengths.
      **How to read it:** same root height and same arm lengths but a different hand height means VRIK is moving
      the hand directly, and only sending hand translations will fix it. A different root height means the body
      is standing at a different height and the root offset is the fix -- cheaper, and it would move everything
      consistently. Different arm lengths mean the skeletons differ and neither would fully close the gap.
      Guessing between those three would have meant shipping a change to how every remote body looks with no way
      to tell whether it helped.
- [ ] **Your own sword hitting where the blade is not.** The screenshots of 2026-09-24 are a first-person view of
      the player's own weapon striking the Earth Stone with the impact well off the blade. This client never writes
      the local player's bones, it only reads them, so this is not sync and not ours: it is the weapon collision
      offset of the VR setup (VRIK or HIGGS).
      **Checked rather than assumed, 2026-09-25:** `VRBodySync::SetRemotePose` has exactly one caller,
      `InterpolationSystem`, which runs on remote actors; and there is no hook anywhere in this client on the melee
      hit path -- no `HitData`, no weapon swing, no hit handler. There is no code here that could move where your
      own blade lands.
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
- [x] **[untested] A duplicate spawn now refreshes the ownership epoch (2026-09-25).** A spawn for a character
      that already exists here was discarded whole. The server re-sends one when something about the character
      changed, and the epoch is the part that matters: upstream's ownership rework refuses any claim whose epoch
      does not match the server's, so a stale epoch meant every later attempt to take that actor was rejected and
      nothing said why. It is refreshed now, and a change is logged.
      **Position, cell and death state are deliberately still ignored** -- they arrive continuously through the
      movement and death paths, and forcing them from a spawn message would fight those. So the audit's finding
      is only partly addressed, on purpose.
      `churn.txt` still passes and still does not reproduce the original symptom, so this is reasoned from the
      code rather than from a repro.
- [ ] **Superseded: duplicate spawn messages are discarded rather than refreshing state**, so a newer spawn carrying updated
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


---


- [x] **[untested] Levelled NPC reconciliation is back on for VR (2026-09-23), after being off since it crashed
      Seen on 2026-09-22.** All three engine calls are now accounted for. Watch the next join closely: this is the
      code path that crashed him, and the one part still taken on trust is `CreateTemplateActorBase`.
    - `TESObjectREFR::SetLeveledCreature`: **address supplied 2026-09-23**, AE 20231 -> VR 0x1402b8f10
      (SE 0x1402a77a0, SE id 19826). This is the step that actually applies the pick, so until now the feature
      could not have worked even without the crash.
    - **`CreateTemplateActorBase` is now proven by running it (2026-09-25).** The host's log shows
      `Applied leveled NPC pick` three times on 2026-09-24 (18:52:14, 19:27:25 twice), and that line is written
      only after both `CreateTemplateActorBase` and `SetLeveledCreature` have returned. No crash followed the
      19:27 pair. An earlier note here said reconciliation never ran; that was read from the friend's log, not the
      host's, and was wrong. The address is good.
    - `TESActorBaseData::CreateTemplateActorBase`: AE 14375 -> VR 0x19c0c0. Was not proven by running it, but
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

- [ ] **TiltedEvolutionVR `aaf5d83`, item and body drift.** A remote body is moved by teleporting, and a teleport
      into a loose object makes havok fling it (their collision-layer table patch stops that); an NPC's capsule can
      be shoved 54 units off its body by a player and stay there (they read the capsule and warp it back). 1200
      lines, address-heavy; take it when under-the-ground or item drift is next.
- [ ] **TiltedEvolutionVR `1660eb0`, ownership blacklists and former-owner updates.** Read 2026-09-19: a follow-up
      to upstream #887 (ownership epochs); every change needs the epoch fields, so it only comes with the rework.
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

- [ ] Benchmark 60 s at the same save and spot, standing still and then walking, in four setups:
    1. FUS without Skyrim Together;
    2. with Skyrim Together, not connected;
    3. connected alone to a local server;
    4. co-op.

    Keep the four summaries in this file, so every later change is compared against real numbers.

- [ ] Don't attach the debugger watcher during normal sessions. It stops the game on every thread
      start and exit (DynDOLOD alone runs 600+ threads).

### 1.2 Likely wins (do after 1.1 confirms them)

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

---

## 2. Sync correctness ("NPCs and animals feel buggy")

### 2.1 Combat and NPCs

- [ ] **Hits from VR weapons.** A VR sword hit lands on the attacker's copy of the NPC. Check that
      damage reaches the NPC's owner, and that the health change is sent back when the NPC is owned by
      the other player.
- [ ] **Actor ownership warnings.** Look into `Actor for ownership transfer not found` and
      `OnNotifyActorTeleport: failed to retrieve actor` once the churn fix is confirmed.
- [ ] **Shared follower loops** (the Lydia case). A follower owned by one player gets pulled by the
      other player's game. The 2026-09-18 thrash (removed and re-assigned 11 times a second) was a different bug,
      fixed in `DiscoveryService`; Lydia followed and fought fine afterwards. The tug of war itself is still open.
    - Decide the rule (the follower's owner = the player it follows), then apply it during
      ownership transfer.

### 2.2 Items and inventory


### 2.3 VR body

- [ ] **Legs when moving.** Check that smooth locomotion plays a walk or run on the remote copy,
      rather than sliding.
- [ ] **Face "looks off"** (FaceGen on VR). Parked earlier, still open.

### 2.4 World, quests, dialogue

- [ ] **Message boxes show up for both players** ("player 1 opens text, player 2 sees the popup").
      No code syncs message boxes. The likely path is activation sync: the other client replays the activation
      with the remote player as activator, and the object's script shows its message. Note which object it was
      next time before changing activation sync.
- [ ] **Quest NPCs out of sync** (Bastianus Axius). Check quest sync with a party on the next test.
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

- [ ] **In-headset menu:** the normal Skyrim Together UI as a SteamVR dashboard tab
      (`Systems/VRDashboard.cpp`). Renders for the friend, still blank for the host; top of section 0.
    - [ ] Adapt the page layout for the dashboard (large text, no empty full-screen areas).
    - [ ] A small always-visible overlay for chat and notifications while playing.

### 4.2 Versions and compatibility

- [ ] Add a protocol number that changes with every message change, so unrelated client changes stop forcing
      server restarts.
- [ ] **Startup checks with plain-language HUD errors:**
    - [x] uGridsToLoad = 5 (stops the automatic connection);
    - [x] SKSE VR loaded (warning only);
    - [x] `connect.txt` missing;
    - VR Address Library present;
    - fewer than 255 plugins;
    - `SkyrimTogether.esp` with the 1.70 header.

### 4.3 Packaging and install

- [ ] **Install script** (PowerShell):
    - finds the MO2 instance and copies the tool into it;
    - adds the MO2 executable entry;
    - creates `%LOCALAPPDATA%\SkyrimTogetherVR\connect.txt` from a template on first run;
    - checks the requirements listed in 4.2.
- [ ] **Guide for non-FUS modlists:** what's required, what's known to conflict, and the plugin
      limit.
- [ ] **Licensing before sharing.** Tilted Online is GPL-3: publish the source for every shared
      build, and keep the upstream license and credits. Check the Skyrim Together Reborn project's rules
      on redistributing forks.

---

## 5. Polish

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

- [ ] **[untested] Character:** wait for the host's own `CharacterSpawnRequest`, reuse its `AppearanceBuffer` and `ChangeFlags`
      (the bot is a clone of the host, so the receiving client builds the face from data it already accepts), then
      `AssignCharacterRequest` 200 units from the host with the same worldspace and cell, Nord race, iron sword and
      Flames in the inventory, health 100. Then `EnterExteriorCellRequest` from the host's grid.
- [ ] **[untested] First scenario, the bug of 2026-09-19** (`Code/bot/scripts/copy-respawn.txt`): spawn, walk past the host, take damage to -4, respawn, keep walking.
      Pass: the host's log shows `copy would have spawned with the owner's health -4 ... started at 1` and the copy
      stays visible after the respawn.
- [ ] **Phase 2, real animation:** record the host's own outgoing packets into `logs\session.stpcap` (a log like
      `tp_client.log`, capped and rotated, not a toggle) and let the bot replay a recording with its server ids
      remapped, so the copy walks, fights and casts like a real player.
- [ ] **Where it runs:** on the host's PC against the local server. Never on the friend's server during play.
