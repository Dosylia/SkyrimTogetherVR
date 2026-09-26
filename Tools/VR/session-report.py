"""Answer the open questions from one play session, instead of hunting through the log for them.

Every fix shipped since 2026-09-23 logs a line when it fires. This reads a session's log and says, for each
one, whether it happened -- and for the invisible-body bug it does the comparison that names the fade offset.

    python Tools\\VR\\session-report.py
    python Tools\\VR\\session-report.py --log "C:\\Users\\a\\Downloads\\SkyrimTogetherVR-logs-...\\tp_client.log"
    python Tools\\VR\\session-report.py --since "2026-09-25 20:00"

It reads only; it changes nothing. Run it on your own log and on the friend's, since several of these questions
are about what the *other* side saw.
"""
import argparse
import os
import re
import sys
from collections import Counter

DEFAULT_LOG = r'E:\FUS\tools\Skyrim Together VR\logs\tp_client.log'

# What each shipped fix writes when it fires, and what its absence means. The wording matters: "did not fire" is
# not the same as "is broken", and several of these only fire when the player does the thing.
CHECKS = [
    ('Corpse dragging, sending',   r'is being moved here',
     'you dragged a dead body and its bones went out'),
    ('Corpse dragging, receiving', r'posing body [0-9A-F]+ from its owner',
     'you saw a body the other player was moving'),
    ('Spell damage to a player',   r'PvP: spell [0-9A-F]+ hit remote player',
     'a damaging spell of yours was sent as damage rather than left to their game'),
    ('Melee damage to a player',   r'PvP: hit remote player',
     'a sword or arrow of yours was sent as damage'),
    ('NPC health corrected',       r'health corrected from .* to its owner',
     'an NPC had drifted from its owner and was pulled back'),
    ('Naked-NPC check gave up',    r'Naked NPC check gave up',
     'the equip loop that starved the stream was stopped on some actor'),
    ('Animation backlog trimmed',  r'Animation replay backlog over',
     'a queue grew past 96 actions and stale ones were dropped'),
    ('Essential NPC left alone',   r'refuses to die',
     'an NPC that cannot be killed stopped being hammered with Kill()'),
    ('Levelled NPC reconciled',    r'Applied leveled NPC pick',
     'a levelled NPC was rebuilt to match the owner'),
    ('Crime alarm ran',            r'Crime alarm ran',
     'the crime alarm guard is live and a crime was witnessed'),
    ('Crime alarm caught a null',  r'Crime alarm walked a null actor',
     'the guard saved a session that would have crashed'),
    ('Solstheim crime faction',    r'Solstheim crime faction found',
     'the load-order search found it, so paying fines works'),
    ('NPC fights a second player', r'now also fights player',
     'an NPC you both hit turned on the other player too'),
    ('Wrong-spell fallback',       r'did not resolve here; falling back',
     'a spell id could not be resolved and the staged one was used'),
]

# The questions this week's measurements were shipped to answer. Presence alone is not an answer for any of them,
# so each has an analyser below that reports the numbers instead.
MEASURED = [
    ('Hips (sender)', r'local hips at'),
    ('Hips (receiver)', r'hips wanted at'),
    ('Hands compared', r'hands -- owner L\('),
    ('VR archery state', r'VRArchery: local attack state'),
    ('Objects moved by hand', r'ObjectMove:'),
    ('Actors below the floor', r'SinkDiag:'),
    ('Character controller', r'ControllerDiag:'),
    ('A body handled here', r'BodyGrabDiag:'),
    ('Client went silent', r'Silence: sent nothing'),
    ('Arrival-side drops', r'EpochMiss: dropped'),
    ('Stale copy moved', r'without an update and the server has re-sent it'),
    ('Stale copy: factions', r'came back in different factions'),
    ('Stale copy: death', r'and this copy had it the other way'),
    ('Stale copy: inventory', r'came back with a different inventory'),
    ('Impossible jumps caught', r'JumpDiag:'),
]

PROBLEMS = [
    ('Crashes',                    r'VectoredExceptionHandler: crash occurred'),
    ('Unresolved addresses',       r'did not resolve on this build'),
    ('charController offset wrong', r'charController offset is wrong'),
    ('Bones the renderer cannot use', r'renderer cannot use'),
    ('Local VR pose unreadable',   r'could not read the local VR pose'),
    ('Starved interpolation',      r'updates had no future point'),
]

COPY_WORDS = re.compile(r"copy words \[([^\]]*)\].*player words \[([^\]]*)\]")
TIMESTAMP = re.compile(r'^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})')


def read(path, since):
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        for line in fh:
            if since:
                m = TIMESTAMP.match(line)
                if m and m.group(1) < since:
                    continue
            yield line


HIPS_LOCAL = re.compile(r'local hips at \(([-0-9.]+), ([-0-9.]+), ([-0-9.]+)\)')
HANDS = re.compile(r'hands -- owner L\(([-0-9.]+), ([-0-9.]+), ([-0-9.]+)\) R\(([-0-9.]+), ([-0-9.]+), ([-0-9.]+)\); '
                   r'copy L\(([-0-9.]+), ([-0-9.]+), ([-0-9.]+)\) R\(([-0-9.]+), ([-0-9.]+), ([-0-9.]+)\)')
SINK = re.compile(r'SinkDiag: (\d+) placements found a remote actor lower .* worst ([0-9.]+) units on ([0-9A-F]+)')
CONTROLLER = re.compile(r'ControllerDiag: (\d+) of (\d+) remote placements')
EPOCH_MISS = re.compile(r'EpochMiss: dropped (.+?) for character ([0-9A-F]+) .* epoch (\d+) and this copy is epoch (\d+)\. (\d+) of these')
ARCHERY = re.compile(r'VRArchery: local attack state (\d+)')
# The player is form 14; a bow shot carries ammo, a spell does not.
BOW_SHOT = re.compile(r'Projectile launch: shooter 14, base [0-9A-F]+, weapon [0-9A-F]+, ammo (?!0,)[0-9A-F]+')
SILENCE = re.compile(r'Silence: sent nothing for (\d+) ms; menus \[([^\]]*)\]')
OBJECT_MOVE = re.compile(r'ObjectMove: ([0-9A-F]+) moved ([0-9.]+) units')
JUMP = re.compile(r'JumpDiag: a buffered point was ([0-9.]+) units from the one before it, at \(([-0-9.]+), ([-0-9.]+), ([-0-9.]+)\).*?(\d+) since')
BACKLOG = re.compile(r'newest buffered tick (-?\d+) to (-?\d+) ms ahead of playback, (\d+) updates had no future point')
BODY_GRAB = re.compile(r'BodyGrabDiag: remote body ([0-9A-F]+) has been moving here for (\d+) ms, ([0-9.]+) units')


def analyse(lines):
    """The numbers behind this week's measurements, and what each one decides.

    Every entry says what the number means, because a count on its own has never settled anything here.
    """
    hips = [tuple(float(v) for v in m.groups()) for m in (HIPS_LOCAL.search(l) for l in lines) if m]
    hands = [tuple(float(v) for v in m.groups()) for m in (HANDS.search(l) for l in lines) if m]
    sinks = [m.groups() for m in (SINK.search(l) for l in lines) if m]
    ctrl = [tuple(int(v) for v in m.groups()) for m in (CONTROLLER.search(l) for l in lines) if m]
    misses = [m.groups() for m in (EPOCH_MISS.search(l) for l in lines) if m]
    archery = [int(m.group(1)) for m in (ARCHERY.search(l) for l in lines) if m]
    silences = [(int(m.group(1)), m.group(2)) for m in (SILENCE.search(l) for l in lines) if m]
    objects = [(m.group(1), float(m.group(2))) for m in (OBJECT_MOVE.search(l) for l in lines) if m]
    grabs = [(m.group(1), int(m.group(2)), float(m.group(3))) for m in (BODY_GRAB.search(l) for l in lines) if m]

    out = []

    if hips:
        zs = [h[2] for h in hips]
        spread = max(zs) - min(zs)
        verdict = ('the hips moved, so the tracker does reach the pelvis and this protocol is the right place to carry it'
                   if spread > 10 else
                   'the hips barely moved: either nobody crouched, or the hip tracker never reaches the pelvis and the fix is upstream of us')
        out.append(('Hips, sender', '%d samples, height %.1f to %.1f (spread %.1f). %s'
                    % (len(hips), min(zs), max(zs), spread, verdict)))

    if hands:
        dl = [abs(h[2] - h[8]) for h in hands]
        dr = [abs(h[5] - h[11]) for h in hands]
        worst = max(max(dl), max(dr))
        mean = (sum(dl) + sum(dr)) / (len(dl) + len(dr))
        verdict = ('the copy holds its hands where the owner holds theirs, so the offset is not in this protocol'
                   if worst < 8 else
                   'the copy holds its hands somewhere else; that difference is the hand offset, measured against the right person at last')
        out.append(('Hands, owner against copy', '%d comparisons, mean %.1f units apart, worst %.1f. %s'
                    % (len(hands), mean, worst, verdict)))

    if sinks:
        worst = max(float(s[1]) for s in sinks)
        worst_actor = max(sinks, key=lambda s: float(s[1]))[2]
        out.append(('Actors below the floor',
                    '%d reports, worst %.0f units below where it was last placed, on character %s. '
                    'Anything here at all means copies sink locally after being placed.'
                    % (len(sinks), worst, worst_actor)))

    if ctrl:
        skipped = sum(c[0] for c in ctrl)
        total = sum(c[1] for c in ctrl)
        share = (100.0 * skipped / total) if total else 0.0
        verdict = ('a controller waiting a frame or two for its first step, which is what the code expects'
                   if share < 5 else
                   'the assumption that interpolation catches the controller up is false, and that is the likely floor bug')
        out.append(('Character controller', '%d of %d placements (%.1f%%) could not move it. %s'
                    % (skipped, total, share, verdict)))

    if misses:
        by_kind = {}
        for what, _who, _msg, _mine, since in misses:
            by_kind[what] = by_kind.get(what, 0) + int(since) + 1
        detail = ', '.join('%s x%d' % (k, v) for k, v in sorted(by_kind.items(), key=lambda kv: -kv[1]))
        out.append(('Messages refused on arrival',
                    '%s. A steady count, above all on death, means the epoch test should become "not older" instead of "equal".'
                    % detail))

    if archery:
        states = sorted(set(archery))
        # Absence of the bow states only means something if a bow was actually drawn. The player's own launches
        # are "shooter 14"; a bow shot carries ammo, a spell does not. Without one of those in the window, states
        # of 0 and 2 say nothing at all -- and on 2026-09-26 this analyser cheerfully concluded the opposite from
        # a session in which Seen cast spells and never touched a bow.
        drew_bow = any(BOW_SHOT.search(l) for l in lines)
        if any(s >= 9 for s in states):
            verdict = 'the attack state moves, so a nocked arrow can be replicated by syncing it'
        elif drew_bow:
            verdict = ('a bow was fired and the attack state still never left the melee values, so VR archery does not set it '
                       'and the arrow has to be attached by hand on the other side')
        else:
            verdict = 'INCONCLUSIVE: no bow was fired while this was logging, so the missing bow states prove nothing. Draw a bow and look again.'
        out.append(('VR archery', 'states seen: %s. %s' % (', '.join(str(s) for s in states), verdict)))

    if silences:
        worst = max(s[0] for s in silences)
        inmenu = sum(1 for s in silences if s[1] and s[1] != 'none')
        out.append(('Client went quiet',
                    '%d times, longest %d ms, %d of them with a menu open (so a menu rather than a stall).'
                    % (len(silences), worst, inmenu)))

    if objects:
        out.append(('Objects moved by hand',
                    '%d moves over 8 units across %d objects, largest %.0f units. This is the traffic an object-position sync would have to carry.'
                    % (len(objects), len(set(o[0] for o in objects)), max(o[1] for o in objects))))

    backlog = [tuple(int(v) for v in m.groups()) for m in (BACKLOG.search(l) for l in lines) if m]
    if backlog:
        worst_behind = min(b[0] for b in backlog)
        starved = sum(b[2] for b in backlog)
        if worst_behind < -1000:
            out.append(('Interpolation backlog',
                        'the buffer was %d ms BEHIND playback at worst, and %d updates arrived with nothing after them. '
                        'Positions interpolated across a gap like that are nonsense -- this is what made SinkDiag report '
                        '875,458 units on 2026-09-23 -- and to a player it looks like everything lagging and then teleporting.'
                        % (worst_behind, starved)))
        else:
            out.append(('Interpolation backlog', 'worst %d ms behind playback, %d updates with nothing after them: healthy.' % (worst_behind, starved)))

    jumps = [(float(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4)), int(m.group(5))) for m in (JUMP.search(l) for l in lines) if m]
    if jumps:
        total = sum(j[4] for j in jumps)
        worst = max(j[0] for j in jumps)
        out.append(('Impossible jumps caught',
                    '%d of them, worst %.0f units. Each one is a pair of buffered points no actor could have travelled between, and before 2026-09-26 the actor was walked across that gap. '
                    'Cross-check the times against somebody changing worldspace: if they line up, the cause is confirmed; if they do not, the guard is still right but the reason is not.'
                    % (total, worst)))

    if grabs:
        longest = max(g[1] for g in grabs)
        out.append(('A body handled here',
                    '%d reports, longest continuous movement %d ms, across %d bodies. A claim rule has to tell this apart from a settling ragdoll.'
                    % (len(grabs), longest, len(set(g[0] for g in grabs)))))

    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', default=DEFAULT_LOG)
    ap.add_argument('--since', default=None, help='only lines at or after this "YYYY-MM-DD HH:MM:SS"')
    args = ap.parse_args()

    if not os.path.exists(args.log):
        print('No log at %s' % args.log)
        return 1

    lines = list(read(args.log, args.since))
    print('Session report: %s' % args.log)
    print('%d lines%s\n' % (len(lines), ' since ' + args.since if args.since else ''))

    text = ''.join(lines)

    print('=== did each fix fire? ===')
    for name, pattern, meaning in CHECKS:
        hits = len(re.findall(pattern, text))
        mark = 'yes' if hits else ' no'
        print('  [%s] %-28s %5d   %s' % (mark, name, hits, meaning if hits else ''))

    print("")
    print("=== this week's measurements: did they fire? ===")
    for name, pattern in MEASURED:
        hits = len(re.findall(pattern, text))
        print('  [%s] %-24s %5d' % ('yes' if hits else ' no', name, hits))

    findings = analyse(lines)
    if findings:
        print("")
        print("=== and what they say ===")
        for name, verdict in findings:
            print('  %s:' % name)
            print('    %s' % verdict)

    print('\n=== problems ===')
    for name, pattern in PROBLEMS:
        hits = len(re.findall(pattern, text))
        if hits:
            print('  %-32s %d' % (name, hits))
    if not any(re.search(p, text) for _, p in PROBLEMS):
        print('  none')

    # The invisible-body question. Each CopyDiag line carries the words past the end of the copy's NiNode and the
    # same words for the player, whose body is always drawn. A word that differs is the candidate for the fade.
    print('\n=== invisible body: copy against player ===')
    differing = Counter()
    samples = 0
    for line in lines:
        m = COPY_WORDS.search(line)
        if not m:
            continue
        samples += 1
        copy_words = dict(w.split('=') for w in m.group(1).split() if '=' in w)
        player_words = dict(w.split('=') for w in m.group(2).split() if '=' in w)
        for offset, value in copy_words.items():
            if offset in player_words and player_words[offset] != value:
                differing[offset] += 1

    if not samples:
        print('  no CopyDiag lines with render words; is the other player nearby and this build recent enough?')
    else:
        print('  %d samples' % samples)
        if not differing:
            print('  every word matched the player every time: nothing here explains an invisible body,')
            print('  or the body never went invisible during these lines')
        else:
            for offset, count in differing.most_common():
                print('  +0x%-4s differed in %d of %d samples' % (offset, count, samples))
            print('  the offset differing only while a body was invisible is the fade; force it to the')
            print('  player\'s value to bring the body back')

    print('\n=== what to send back ===')
    print('  the log itself, plus the time of anything odd you noticed;')
    print('  a crash is named by: python Tools\\VR\\explain-crash.py')
    return 0


if __name__ == '__main__':
    sys.exit(main())

