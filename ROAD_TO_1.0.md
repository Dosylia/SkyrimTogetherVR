# Road to 1.0 stable

Details live in `VR_TODO.md`. This is the short version.

## What 1.0 means (proposed)

- Three full co-op sessions in a row, no crash
- Nothing slides, nobody invisible, bodies agree on both screens
- A new player installs and joins without taking the headset off

## Done

**Port (March to 13 September)**
- VR build targets and launcher
- ~3000 VR game addresses found
- Game starts, menu works, server connects
- First co-op: armor, movement, interaction

**Playable (14 to 20 September)**
- Auto-connect, auto-party
- Menus don't pause the world
- VR body sync: head, hands, fingers, legs, body size
- Levelled NPCs match on both screens
- PvP sword hits, shouts, arrows
- Friend's name and health bar
- Quit crash and fade fixed
- Level-up no longer drops to the main menu
- Test bot (`STBot`)
- Release packaging, `update.bat`, Vortex route

**Upstream merge (22 September)**
- Ownership rework, havok fix, levelled NPC system

**Sync and stability (23 to 30 September)**
- Dragged dead bodies seen by both
- NPCs fight everyone who hits them
- Hips synced
- Dropped items reach the floor
- Invisible player: copy re-announced
- Out-of-range copies come back correct
- Crash sweep across every engine hook
- Copy-removal crash contained
- Sword weight in haptics
- Pose compression fixed
- Walking outdoors re-sends nearby players
- Player sliding: graph lookup fixed
- Crash tools, bot test pairs
- Dropped items remembered by the server
- Dropped items moved by hand, seen moving
- Lurker and Netch animations synced

## Steps to go

1. **Confirm in play:** the 30 `[untested]` items in `VR_TODO.md`, sliding first
2. **Creature sliding:** capture the Seeker, add it as a replacer
3. **Crashes:** wrist menu crash, loading after death; confirm death no longer drops to the main menu (same cause as level-up, now fixed)
4. **Performance:** measure first, then spawn bursts and the 300 ms remote delay
5. **Followers:** end the tug of war over a shared follower
6. **World sync:** quest NPCs, message boxes, weather and time, mounts
7. **VR interactions seen by both:** objects placed by plugins moved by hand, nocked arrow
8. **Bodies:** handling the other player's body
9. **NPCs below the floor**
10. **VR weapon hits land where the blade is**
11. **Shareable:** in-headset menu, install script, startup checks, non-FUS guide
12. **Versions:** protocol number, so small changes stop forcing server restarts
13. **Licensing:** GPL source for every shared build, upstream credits
14. **Polish:** spawn fade-in, "X is down" message, VR server defaults
