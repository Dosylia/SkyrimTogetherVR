"""Explain a Skyrim Together VR crash dump: what faulted, whose code it was in, and whether the dead object
is an actor we deleted.

This exists because the same argument kept being had from timing alone. A crash half a second after a batch
of remote copies is torn down *looks* like the teardown's fault, and on 2026-09-27 that inference was wrong
three times running -- it was blamed on weapon-touch caching, then on a bounds read, then on the friend's
hardware, before the actual cause turned out to be an unguarded map mutated from a thread pool.

An address settles it. The client records every temporary remote deletion in RecentDeletes (see
Code/client/RecentDeletes.h), and this reads that ring straight out of the dump and compares it against
every register. If the faulting object's address is in the list, the deletion path is the cause. If it is
not, the deletion path is innocent and the search moves elsewhere.

Every register, note -- not just Rcx. The first version of the in-process report compared Rcx alone and
printed "not our actor deletion" for the crash of 2026-09-27 12:33 while Rdi held an actor deleted 1930 ms
earlier and printed three lines below it.

Usage:
    python explain-dump.py                        # newest crash_UTC_* dump under ~/Downloads
    python explain-dump.py <path to .dmp>
    python explain-dump.py <path> --map <.map>    # the map from the build that crashed

The map must come from the build that produced the dump. Tools/VR/deploy-client.ps1 keeps one `.old-*` copy
of the previous client next to the deployed one; a map from a later build names the wrong functions rather
than failing, so if the names look absurd, that is why.
"""
import os
import sys
import glob
import struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from minidump import Dump  # noqa: E402
import mapsym  # noqa: E402

ENTRY = '<QIIIIQ'
ENTRY_SIZE = struct.calcsize(ENTRY)  # 32: Address, FormId, Handles, RefCount, Flags, Stamp
RING_COUNT = 32                      # RecentDeletes::kCount

CLAIMS = [
    (1 << 0, 'the player is fighting it'),
    (1 << 1, 'another actor is fighting it'),
    (1 << 2, 'still in the AI process lists'),
    (1 << 3, 'still has 3D'),
    (1 << 4, 'dead or dying'),
    (1 << 5, 'it is a teammate'),
]


def describe_claims(flags, handles):
    what = [text for bit, text in CLAIMS if flags & bit] or ['nothing obvious']
    return "{}, {} outstanding handle(s)".format(', '.join(what), handles)


def newest_dump():
    home = os.path.expanduser('~')
    found = []
    for pattern in (os.path.join(home, 'Downloads', 'crash_UTC_*', '*.dmp'),
                    os.path.join(home, 'Downloads', 'crash_UTC_*.dmp'),
                    os.path.join(home, 'Downloads', '**', '*.dmp')):
        found += glob.glob(pattern, recursive=True)
    if not found:
        return None
    return max(found, key=os.path.getmtime)


def check_map_matches(d, map_path):
    """Whether the linker map describes the build that produced this dump.

    Worth its own check because a mismatch is silent and convincing. Run against a dump from 2026-09-13 --
    two weeks before RecentDeletes was written -- a current map pointed at an address that happened to be
    readable and the tool reported "0 actor deletions recorded this session" rather than "this build has no
    ring". A wrong answer delivered confidently is worse than no answer.
    """
    want = mapsym.timestamp(map_path)
    # urSovngarde.exe since the rename of 2026-10-09; a dump from an older build names SkyrimTogetherVR.exe.
    got = d.module_timestamp('urSovngarde.exe') or d.module_timestamp('SkyrimTogetherVR.exe')
    if want is None or got is None:
        return True, None
    if want == got:
        return True, None
    import datetime

    def fmt(t):
        return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime('%Y-%m-%d %H:%M UTC')

    return False, ("the linker map is from a different build than this dump\n"
                   "    map:  {:#x}  {}\n"
                   "    dump: {:#x}  {}\n"
                   "  Every address would be named wrongly. Use the map from the build that crashed --\n"
                   "  deploy-client.ps1 keeps one `.old-*` client, or rebuild that commit.".format(
                       want, fmt(want), got, fmt(got)))


def read_ring(d, map_path):
    """The RecentDeletes ring, newest first, or None when the dump does not contain it."""
    ok, why = check_map_matches(d, map_path)
    if not ok:
        return None, why

    ring = mapsym.find(r'\?s_ring@\?A0x[0-9a-f]+@RecentDeletes@@', map_path)
    nxt = mapsym.find(r'\?s_next@\?A0x[0-9a-f]+@RecentDeletes@@', map_path)
    if not ring or not nxt:
        return None, "this build has no RecentDeletes ring (the map does not mention it)"

    ring_addr, next_addr = ring[0][0], nxt[0][0]
    ranges = d.ranges()
    nb = d.read(next_addr, 4, ranges)
    rb = d.read(ring_addr, RING_COUNT * ENTRY_SIZE, ranges)
    if nb is None or rb is None or len(rb) < RING_COUNT * ENTRY_SIZE:
        return None, ("the ring is at {:#x} but that memory is not in this dump -- the crash handler did not "
                      "capture the data segment".format(ring_addr))

    count = struct.unpack('<I', nb)[0]
    entries = [struct.unpack(ENTRY, rb[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE]) for i in range(RING_COUNT)]
    ordered = []
    for i in range(1, min(count, RING_COUNT) + 1):
        e = entries[(count - i) % RING_COUNT]
        if e[0]:
            ordered.append(e)
    return (count, ordered), None


def main(argv):
    path, map_path = None, None
    i = 0
    while i < len(argv):
        if argv[i] == '--map':
            map_path = argv[i + 1]
            i += 2
        else:
            path = argv[i]
            i += 1

    path = path or newest_dump()
    if not path or not os.path.exists(path):
        raise SystemExit("no dump given and none found under ~/Downloads")
    print("dump: {}".format(path))

    d = Dump(path)
    tid, code, addr, params, ctx_rva = d.exception()
    ctx = d.context(ctx_rva)

    print("fault {:#010x} at {:#x}".format(code, addr), end='')
    if code == 0xC0000005 and len(params) >= 2:
        kind = {0: 'read', 1: 'write', 8: 'execute'}.get(params[0], params[0])
        print(" -- access violation, {} of {:#x}".format(kind, params[1]), end='')
    print("\nfaulting thread: {}".format(tid))

    matched = True
    try:
        matched, why = check_map_matches(d, map_path)
        if not matched:
            print("WARNING: {}".format(why))
        else:
            addrs, names = mapsym.load(map_path)
            hit = mapsym.symbolise(ctx['Rip'], addrs, names)
            print("Rip {:#x} -> {}".format(
                ctx['Rip'], "{}+{:#x}".format(*hit) if hit else "the game's code, not ours"))
    except SystemExit as e:
        print("(no linker map, so no function names: {})".format(str(e).splitlines()[0]))

    print("registers: " + ", ".join("{}={:#x}".format(k, v) for k, v in ctx.items()))

    if not matched:
        return 1

    ring, why = read_ring(d, map_path)
    if ring is None:
        print("\n  {}".format(why))
        return 0

    count, entries = ring
    print("\nRecentDeletes: {} actor deletions recorded this session, newest first".format(count))

    # A deleted actor's address sitting in any register is the whole point of the ring.
    live = {name: value for name, value in ctx.items() if name != 'Rsp'}
    hits = []
    newest_stamp = entries[0][5] if entries else 0
    for shown, (address, form_id, handles, refcount, flags, stamp) in enumerate(entries):
        where = [name for name, value in live.items() if value == address]
        age = (newest_stamp - stamp) / 1e6 if stamp and newest_stamp else 0.0
        mark = "   <<<< IN {}".format(', '.join(where)) if where else ""
        if where:
            hits.append((address, form_id, where, handles, flags))
        if shown < 12 or where:
            print("    {:#018x}  form {:X}  {:+.0f} ms  [{}]{}".format(
                address, form_id, -age, describe_claims(flags, handles), mark))

    print()
    if hits:
        address, form_id, where, handles, flags = hits[0]
        print("  MATCH: the faulting code was holding actor form {:X} in {} -- one we deleted.".format(
            form_id, ', '.join(where)))
        print("  It was deleted while: {}".format(describe_claims(flags, handles)))
        print("  This is a use-after-free on the remote-copy teardown path, not a coincidence of timing.")
    else:
        print("  NO MATCH: no register holds an actor we deleted. The deletion path is not this crash.")
        addresses = [e[0] for e in entries]
        if addresses:
            target = ctx['Rcx']
            near = min(addresses, key=lambda a: abs(a - target))
            print("  (nearest deleted address to Rcx is {:#x}, {:#x} away -- far enough to mean nothing)".format(
                near, abs(near - target)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
