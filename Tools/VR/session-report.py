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
