# Reading a crash

Three tools, from cheapest to most involved. Each one answers a different question, and the first two are
usually enough.

## 1. Which of our functions was running — `explain-crash.py`

Reads the register dump the client writes into `tp_client.log` and names the functions. Needs the exe and
PDB that produced the crash.

```
python Tools/VR/explain-crash.py                  # newest crash in the deployed client's log
python Tools/VR/explain-crash.py --log <path>     # a log from elsewhere, e.g. a friend's bundle
```

## 2. What faulted, and was it an actor we deleted — `explain-dump.py`

Reads a `.dmp`. Prints the exception, the registers, which function `Rip` was in, and then the
`RecentDeletes` ring: the last 32 remote copies this client deleted, what still had a claim on each one, and
whether any register holds one of them.

```
python Tools/VR/explain-dump.py                        # newest crash_UTC_* dump under ~/Downloads
python Tools/VR/explain-dump.py <path to .dmp>
python Tools/VR/explain-dump.py <path> --map <.map>
```

A `MATCH` line means the faulting code was holding an actor we deleted, and the teardown path is the cause.
`NO MATCH` means it was not, and the search moves elsewhere. That distinction is the point of the whole
tool: on 2026-09-27 the crash was blamed on weapon-touch caching, then on a bounds read, then on the
friend's hardware — three times in one day, each inferred from a crash happening shortly after a batch of
copies was torn down. An address settles what timing cannot.

## 3. Whose code is on the stack — `minidump.py`

The layer underneath, usable on its own when the question is just "is any of this ours".

```
python Tools/VR/minidump.py <path to .dmp>
```

`from minidump import Dump` gives `modules()`, `exception()`, `context()`, `threads()` and `read(addr, n)`.

## The build has to match

The launcher replaces the game executable, so a dump shows **one** ~90 MB module named
`urSovngarde.exe` (`SkyrimTogetherVR.exe` before the rename of 2026-10-09) holding the game's code and ours together. Module names cannot separate them. What
can is the linker map (`build/windows/x64/release/urSovngarde.map`, produced by the `/MAP` flag in
`Code/immersive_launcher/xmake.lua`): it lists only our symbols, so an address it names is ours, and the
game's image sits inside `?game_seg@@3PAEA`.

The map must come from the build that crashed. A stale map does not fail — it names the wrong functions and
reads the wrong addresses, and looks entirely plausible doing it. Run against a September 13 dump, a current
map reported "0 actor deletions recorded this session" for a build written two weeks before `RecentDeletes`
existed. `explain-dump.py` now compares the map's timestamp against the dump's module timestamp and refuses
rather than guessing, but if you use `minidump.py` or `mapsym.py` directly, that check is yours to make.

`deploy-client.ps1` keeps one `.old-*` copy of the previous client; past that, rebuild the commit.

## What is not worth trying again

`dbghelp` will not load the PDB for this image. `SymLoadModuleEx` reports the module deferred and then
reports every known function as not found. Several hours went into that on 2026-09-27; the linker map is the
way.

The game executable on disk is Steam-encrypted, so an address inside `?game_seg@@3PAEA` cannot be
disassembled offline. Naming an unknown game-side caller needs an in-game hook or the address database.
