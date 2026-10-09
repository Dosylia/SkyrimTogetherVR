"""Turn an address in the SkyrimTogetherVR image into a function name, using the linker map.

Two things make the usual approach fail here:

  - dbghelp would not load the PDB (SymLoadModuleEx reported the module deferred, then reported every known
    function as not found), so symbolising through a debugger API was abandoned on 2026-09-27.
  - The launcher replaces the game's executable, so a crash dump shows one ~90 MB module at 0x140000000
    named urSovngarde.exe (SkyrimTogetherVR.exe before the rename of 2026-10-09) that holds the game's code *and* ours. No module name can separate them.

The linker map sidesteps both. It is a plain text list of every symbol the linker placed, with its final
address, and it only lists ours -- so an address it can name is our code, and an address past the end of the
game's buffer that it cannot name is ours but nameless. The game's image sits in the `.game` section, marked
by the symbol ?game_seg@@3PAEA; anything inside that span is the game and there is nothing to look up,
because the game exe on disk is Steam-encrypted and cannot be disassembled offline.

The map is produced by /MAP, which Code/immersive_launcher/xmake.lua passes for this reason. It must be the
map from the build that crashed -- a later build moves every address.

Usage:
    python mapsym.py 0x145634621 0x1451948d0 ...
    python mapsym.py --map <path to .map> 0x145634621
    python mapsym.py --find s_ring            # locate a symbol by name, for reading globals out of a dump

Also importable: `load()`, `symbolise()` and `find()`.
"""
import re
import os
import sys
import bisect

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# urSovngarde.map since the rename of 2026-10-09; an older build output holds SkyrimTogetherVR.map.
DEFAULT_MAP = os.path.join(REPO, 'build', 'windows', 'x64', 'release', 'urSovngarde.map')
if not os.path.exists(DEFAULT_MAP):
    DEFAULT_MAP = os.path.join(REPO, 'build', 'windows', 'x64', 'release', 'SkyrimTogetherVR.map')

LINE = re.compile(r'^\s+[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s')
STAMP = re.compile(r'^\s*Timestamp is ([0-9a-fA-F]{8})')


def timestamp(path=None):
    """The PE TimeDateStamp of the build this map describes, or None.

    A minidump records the same value for every module it lists, so the two can be compared to prove a map
    belongs to the build that crashed. Without that check a stale map does not fail -- it names the wrong
    functions and reads the wrong addresses, and looks entirely plausible while doing it.
    """
    path = path or DEFAULT_MAP
    if not os.path.exists(path):
        return None
    with open(path, encoding='utf-8', errors='replace') as f:
        for _ in range(40):
            line = f.readline()
            if not line:
                break
            m = STAMP.match(line)
            if m:
                return int(m.group(1), 16)
    return None


def load(path=None):
    """Return (sorted addresses, matching names) for every symbol in the map."""
    path = path or DEFAULT_MAP
    if not os.path.exists(path):
        raise SystemExit(
            "no linker map at {}\n"
            "Build the client (xmake build SkyrimImmersiveLauncherVR) or pass --map.\n"
            "It must be the map from the build that crashed -- a later build moves every address.".format(path))
    syms = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = LINE.match(line)
            if m:
                syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def symbolise(addr, addrs, names):
    """Name the symbol containing `addr`, or None when it is not ours."""
    i = bisect.bisect_right(addrs, addr) - 1
    if i < 0 or addr > addrs[-1] + 0x10000:
        return None
    return names[i], addr - addrs[i]


def find(pattern, path=None):
    """Every symbol whose (mangled) name matches `pattern`, as (address, name).

    Used to read our globals out of a crash dump without hardcoding an address: statics in an anonymous
    namespace get a mangled name like ?s_ring@?A0x40b04326@RecentDeletes@@3PAUEntry@2@A, and the anonymous
    namespace's hash changes between builds, so the name has to be matched rather than spelled out.
    """
    addrs, names = load(path)
    rx = re.compile(pattern)
    return [(a, n) for a, n in zip(addrs, names) if rx.search(n)]


def main(argv):
    path = None
    args = []
    mode_find = False
    i = 0
    while i < len(argv):
        if argv[i] == '--map':
            path = argv[i + 1]
            i += 2
        elif argv[i] == '--find':
            mode_find = True
            i += 1
        else:
            args.append(argv[i])
            i += 1

    if mode_find:
        for a, n in find(args[0] if args else '.', path):
            print("  {:#x}  {}".format(a, n))
        return

    addrs, names = load(path)
    print("map: {} symbols, {:#x} .. {:#x}".format(len(addrs), addrs[0], addrs[-1]))
    for arg in args:
        a = int(arg, 16)
        hit = symbolise(a, addrs, names)
        if hit is None:
            print("  {:#x} -> (outside our code -- the game)".format(a))
        else:
            print("  {:#x} -> {}  +{:#x}".format(a, hit[0], hit[1]))


if __name__ == '__main__':
    main(sys.argv[1:])
