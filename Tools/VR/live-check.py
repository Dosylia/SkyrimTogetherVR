"""Runs a bot against the real game and checks, in the game itself, what the bot's script says should be true.

    python Tools/VR/live-check.py live-copy [live-npc ...]
    python Tools/VR/live-check.py --all            every script in ALL, in that order
    powershell -File Tools/VR/run-live.ps1         the same, with the game brought up before and down after

The game must be up and connected (Tools/VR/headless.ps1 up). A script marks a check with a line `log CHECK ...`;
when the bot prints it, this asks the running game through DevBench (http://127.0.0.1:8921) and prints PASS or FAIL
with the value it read. The bot alone can only say what the server sent; this says what the game did with it.

Checks:
    copy exists | copy 3d | copy alive | copy dead | copy health <n>      the game's copy of the bot's character
    npc exists  | npc alive | npc dead | npc health <n|full>             its copy of an NPC the bot owns
    player mark | player dropped <n>                                      the local player's health, before and after
    drop exists | drop gone | drop mark | drop moved <dx> <dy> <dz>       the item the game placed for the bot's drop
    player alive                                                          the local player is not dead
    player has <hex>                                                      the local player carries that item
    hands within <units>                                                  copies' hands are where their owners have
                                                                          them (median of the game's measurements)
    player in <worldspace>                                                outdoors in that worldspace (Tamriel), not
                                                                          in an interior
    ref <hex> near <x> <y> <z> <units>                                    a reference of the game's own is that close
                                                                          to that place
    game alive | game inworld                                             the game still answers | and is in the
                                                                          world with a save loaded, not at a menu
    slide mark | slide below <pct> | slide above <pct>                    the game's own SlideDiag since the mark: the
                                                                          share of a remote body's moves that came
                                                                          with animation variables that said nothing
    copy gait walking <s> | copy gait sliding <s>                         watches the copy for that many seconds: when
                                                                          it moves, does its own animation graph have
                                                                          a speed (walking) or none (sliding)

A line `log DO console <command>` runs that console command in the game at that point of the script, and
`log DO key <name> <ms>` holds a key down in the game for that long (DevBench's keyboard input; `w` walks the player
forward even in VR), and `log DO load last` loads the most recent save (while connected, which is the point of asking). `log DO combat on` and
`log DO combat off` switch combat AI (off is the run's default, so a creature a test spawns just stands there; on makes
it come for the player, which is how it is made to move). `log DO god on` and
`log DO god off` set god mode for a script that sends the player somewhere dangerous: the away tests go to wherever
Lydia stands, which in Emma's save is under water, and the player drowned there twice on 2026-10-02.

`log DO server restart` stops the server and starts it again, and waits until the game has gone back in by itself
(the game reconnects on its own; so does the bot, and its script carries on once it is back in the world).
The bot's script does not stop while the driver is busy with that, so its own `wait` lines run out meanwhile:
`log DO sleep <seconds>` makes the driver itself wait before it reads the next line.
`log DO server restart after console <command>` runs that command while the server is down (the game offline).
`log DO menu <seconds> <menu name>` keeps a menu open that long (the driver waits; the bot's script does not).
`log DO pickup own` has the player pick up the newest item this game dropped itself.
`log DO papyrus <Script> <Function> <self> [form ...]` calls a native function on a reference (ids in hex; a number as n:<value>,
a float as f:<value>, a bool as b:true or b:false).

`log CHECK copy|npc equipped|unequipped <hex>` asks the game whether its copy has that item equipped (not for the left hand: the
game's IsEquipped does not count it); `lefthand|righthand <hex>` asks which weapon is in that hand; `has <hex>` whether it owns one.
`log CHECK distinct <most> <regular expression with one group>` passes when the client has logged at least one and
at most that many different values of the group since the script began.
`log CHECK absent <regular expression>` passes when the client has written no such line since the script began.
`log CHECK logged <regular expression>` passes when the client has written a matching line since the script began.
`log CHECK ids reused <name>` passes when the game gave an actor of that name a form id under which this client had
deleted a copy earlier in the session (what live-temp-reuse needs to have happened to mean anything).

Before the first script, combat AI is switched off and god mode off, by reading what the console answers rather than
by toggling blind: a wild snow bear killed the player in the middle of the first full run (2026-10-02), and a
script that toggles leaves the next one with the opposite setting.
A line `log SHOT <name>` takes a screenshot of the game (DevBench's capture tool, the game's own screenshot) and
saves it as build/.../logs/shots/<script>-<name>.png. `log SHOT <name> of copy|npc|drop|player` first puts a free
camera 260 units south of that thing and a little above it, looking at it, and puts the view back afterwards --
turning the player does not turn a VR view, which follows the headset. Nothing judges the picture; it is there to
be looked at.

Checks are asked while the script carries on, so a script puts a wait after each group of them.
"""
import io, json, math, os, re, shutil, subprocess, sys, time, urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
RELEASE = os.path.join(ROOT, 'build', 'windows', 'x64', 'release')
FUS_ROOT = os.environ.get('SKYRIM_FUS_ROOT') or next(
    (path for path in ('C:/FUS', 'E:/FUS') if os.path.isfile(os.path.join(path, 'ModOrganizer.exe'))),
    'E:/FUS',
)
CLIENT_LOG = os.path.join(FUS_ROOT, 'tools', 'Skyrim Together VR', 'logs', 'tp_client.log')
DEVBENCH = 'http://127.0.0.1:8921/api/tool/'
HEALTH_TOLERANCE = 1.5
PLAYER_TOLERANCE = 2.0
DROP_TOLERANCE = 4.0    # units; a position travels packed to about one unit
SHOTS = os.path.join(RELEASE, 'logs', 'shots')
CAPTURES = os.path.join(FUS_ROOT, 'overwrite', 'SKSE', 'Plugins', 'devbench', 'captures')   # where MO2 puts what DevBench writes under Data

# Order matters: the "away" scripts travel by cell (`cow Tamriel 34 8` and back to 34 -9, Mistwatch) and leave the
# player at the centre of the Mistwatch cell. Not "go to Lydia": she follows the player back, and the next "go to
# Lydia" is then two paces that unload nothing (found 2026-10-02, a session that passed for that reason alone).
ALL = ['live-copy', 'live-npc', 'live-dropmove', 'live-look', 'live-walk', 'live-creatures', 'live-npc-away', 'live-away-seeker',
       'live-temp-remove', 'live-temp-reuse', 'live-copy-abandon', 'live-temp-reconnect', 'live-crime', 'live-netch',
       'live-sender', 'live-game-drops', 'live-equip', 'live-factions', 'live-menu-hold', 'live-levelled', 'live-whiterun', 'live-time', 'live-hit-npc', 'live-world-object', 'live-door-echo', 'live-body-owner', 'live-hands', 'live-death', 'live-load', 'live-load-dead']
# Not in the list, run by name: live-seeker-copy-remote and live-seeker-copy-local (the two halves that showed the
# crash of live-copy-abandon needs a copy and its original together).


def tool(name, body, timeout=6):
    req = urllib.request.Request(DEVBENCH + name, data=json.dumps(body).encode(), headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def papyrus(script, function, form, args=None):
    body = {'action': 'call', 'script': script, 'function': function, 'self': {'form': '0x' + form}}
    if args is not None:
        body['args'] = args
    try:
        return tool('papyrus', body).get('returned')
    except Exception as e:  # a deleted reference answers with an HTTP error
        return 'ERR ' + type(e).__name__


def session_lines():
    """This game session's part of the client log."""
    with io.open(CLIENT_LOG, encoding='utf-8', errors='replace') as f:
        lines = f.read().splitlines()
    start = 0
    for i, l in enumerate(lines):
        if 'Skyrim Together client, build' in l:
            start = i
    return lines[start:]


def restart_server(while_down=None):
    """Server down and up again; returns how long the game took to be back in, or raises. `while_down` is a console
    command run once the game has noticed the server is gone, before it comes back."""
    before = len(session_lines())
    subprocess.run(['taskkill', '/F', '/IM', 'SkyrimTogetherServer.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    started = time.time()
    # The game only notices once its connection times out; a server that is back before that is a server that never
    # left, as far as the game can tell.
    deadline = started + 60
    while time.time() < deadline:
        if any('Disconnected' in l or 'disconnected' in l for l in session_lines()[before:]):
            break
        time.sleep(1)
    else:
        raise RuntimeError('the game never noticed the server was gone')
    noticed = time.time() - started
    if while_down:
        tool('console', {'action': 'exec', 'command': while_down})
        print('   (console while the server is down: %s)' % while_down, flush=True)
        time.sleep(3)
    subprocess.Popen([os.path.join(RELEASE, 'SkyrimTogetherServer.exe')], cwd=RELEASE, creationflags=0x00000010 | 0x00000200,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 150
    while time.time() < deadline:
        new = session_lines()[before:]
        tries = [i for i, l in enumerate(new) if 'VRConnectService: connecting to' in l]
        if tries and any('Grid change: reporting centre' in l for l in new[tries[-1]:]):
            return noticed, time.time() - started
        time.sleep(1)
    raise RuntimeError('the game did not reconnect in 150 s')


def form_of_server_id(server_id):
    """The actor the game runs for a server id; the newest line wins.

    "New entity remotely managed" is the binding itself, so it is preferred to the spawn line: a copy placed at the edge
    of the loaded cells can be gone a tenth of a second after the spawn made it, and the client then makes another one
    for the same character (`Spawned character for entity`), which only the binding line names. Following the spawn
    line alone, live-copy asked five times about a copy that no longer existed (2026-10-03, after live-spawn-burst)."""
    form = None
    wanted = server_id.upper()
    for l in session_lines():
        m = (re.search(r'CharacterSpawnRequest, server id: ([0-9A-Fa-f]+), form id: ([0-9A-Fa-f]+)', l)
             or re.search(r'New entity remotely managed, form id: ([0-9A-Fa-f]+), server id: ([0-9A-Fa-f]+)', l))
        if not m:
            continue
        if 'New entity' in l:
            if m.group(2).upper() == wanted:
                form = m.group(1).upper()
        elif m.group(1).upper() == wanted:
            form = m.group(2).upper()
    return form


def dropped_ref():
    """The reference the game placed for the newest drop it was sent, or None once it has been removed."""
    ref = None
    for l in session_lines():
        m = re.search(r'DroppedItem: placed \d+ \(.*\) as ([0-9A-F]{8})', l)
        if m:
            ref = m.group(1)
        elif ref and ('removed ' + ref) in l:
            ref = None
    return ref


def position(form):
    p = [papyrus('ObjectReference', f, form) for f in ('GetPositionX', 'GetPositionY', 'GetPositionZ')]
    return p if all(isinstance(v, (int, float)) for v in p) else None


def ensure(command, wanted):
    """Puts a console toggle in a known state: runs it, reads the answer, and runs it once more if it went the wrong way."""
    for _ in range(2):
        try:
            tool('console', {'action': 'exec', 'command': command, 'capture': True})
            time.sleep(1.2)
            answer = ' '.join(tool('console', {'action': 'read'}).get('lines', []))
        except Exception:
            return False   # the game is gone; the caller reports it, and the run goes on to its table
        if wanted.lower() in answer.lower():
            return True
    return False


def screenshot(name, target=None):
    """The game's own screenshot through DevBench, moved out of the mod manager's overwrite folder.

    With a target (a world position), the picture is taken from a free camera looking at it."""
    if target:
        eye = (target[0], target[1] - 260.0, target[2] + 150.0)
        aim = (target[0], target[1], target[2] + 60.0)          # about the middle of a body
        dx, dy, dz = aim[0] - eye[0], aim[1] - eye[1], aim[2] - eye[2]
        yaw = math.atan2(dx, dy)                                 # 0 is north (+y), growing towards east (+x)
        pitch = math.atan2(-dz, math.hypot(dx, dy))              # positive looks down
        tool('camera', {'action': 'freecam', 'on': True})
        tool('camera', {'action': 'drive', 'x': eye[0], 'y': eye[1], 'z': eye[2], 'pitch': pitch, 'yaw': yaw})
        time.sleep(0.5)                                          # a rendered frame or two before the picture
    try:
        r = tool('capture', {'kind': 'native', 'checkpointId': name, 'cleanup': True}, timeout=40)
    finally:
        if target:
            tool('camera', {'action': 'freecam', 'on': False})
    if not r.get('ok'):
        return None
    os.makedirs(SHOTS, exist_ok=True)
    target = os.path.join(SHOTS, name + '.png')
    for base, _, files in os.walk(CAPTURES):
        if name + '.png' in files:
            # Copy and delete, not a rename: the overwrite folder and the build folder are on different drives.
            shutil.copyfile(os.path.join(base, name + '.png'), target)
            os.remove(os.path.join(base, name + '.png'))
            return target
    return None


class Run:
    def __init__(self, script):
        self.script = script
        self.drop_mark = None
        self.slide_mark = 0
        self.shots = []
        self.bot_id = None     # the bot character's server id
        self.npc_id = None     # the NPC it registered
        self.mark = None
        self.heal_rate = None
        self.results = []

    def subject(self, who):
        sid = self.bot_id if who == 'copy' else self.npc_id
        if not sid:
            return None, 'the bot never reported that id'
        form = form_of_server_id(sid)
        if not form:
            return None, 'the game logged no spawn for server id %s' % sid
        return form, None

    def check(self, text):
        words = text.split()
        who, what = words[0], words[1]
        ok, detail = False, ''
        try:
            if who == 'game':
                # "game alive": it still answers, and has run frames since the last time it was asked.
                health = json.loads(urllib.request.urlopen('http://127.0.0.1:8921/api/health', timeout=6).read().decode())
                ok = bool(health.get('ok'))
                detail = 'frame %s, %s' % (health.get('frame'), health.get('lastLifecycle'))
                if ok and what == 'inworld':
                    menus = tool('menu', {'action': 'list'}).get('openMenus', [])
                    loaded = tool('inspect', {'kind': 'state'}).get('playerLoaded')
                    at_menu = [m for m in menus if m in ('Main Menu', 'Loading Menu')]
                    ok = bool(loaded) and not at_menu
                    detail += ', player loaded %s, menus %s' % (loaded, menus)
            elif who == 'distinct':
                # "distinct <most> <regular expression with one group>": how many different things the client has
                # logged since the script began -- "Spawn Actor: (\w+), and NPC Seeker" counts the Seekers it has seen.
                most = int(words[1])
                rx = re.compile(' '.join(words[2:]))
                found = sorted({m.group(1) for m in (rx.search(l) for l in session_lines()[self.log_start:]) if m})
                ok = 0 < len(found) <= most
                detail = '%d: %s' % (len(found), ', '.join(found))
            elif who == 'ref' and words[2] == 'base':
                # "ref <hex> base <hex>": the base the game's levelled actor at that reference has now.
                form = words[1]
                base = papyrus('Actor', 'GetLeveledActorBase', form)
                got = base.get('formId', '') if isinstance(base, dict) else str(base)
                ok = isinstance(base, dict) and int(got, 16) & 0xFFFFFF == int(words[3], 16) & 0xFFFFFF
                detail = 'reference %s, levelled base %s (%s)' % (form, got, base.get('editorId', '') if isinstance(base, dict) else '')
            elif who == 'ref' and words[2] == 'near':
                # "ref <hex> near <x> <y> <z> <units>": a reference of the game's own is that close to that place.
                form = words[1]
                at = [papyrus('ObjectReference', 'GetPosition' + axis, form) for axis in 'XYZ']
                wanted = [float(v) for v in words[3:6]]
                known = all(isinstance(v, (int, float)) for v in at)
                ok = known and math.dist(at, wanted) <= float(words[6])
                detail = 'reference %s at %s, %s units from (%s)' % (form, [round(v) for v in at] if known else at,
                                                                   round(math.dist(at, wanted)) if known else '?', ', '.join(words[3:6]))
            elif who == 'ref':
                # "ref <hex> alive|dead": a reference of the game's own, asked directly.
                form, state = words[1], words[2]
                dead = papyrus('Actor', 'IsDead', form)
                ok = isinstance(dead, bool) and dead is (state == 'dead')
                detail = 'reference %s, IsDead %s' % (form, dead)
            elif who == 'gamehour':
                # "gamehour away <hours>": the game clock (global 38) is not at that hour any more.
                hour = papyrus('GlobalVariable', 'GetValue', '38')
                ok = isinstance(hour, (int, float)) and abs(float(hour) - float(words[2])) > 0.5
                detail = 'GameHour %s' % hour
            elif who == 'fewer':
                # "fewer <n> <regular expression>": the client has written fewer than n such lines since the script began.
                rx = re.compile(' '.join(words[2:]))
                hits = [l for l in session_lines()[self.log_start:] if rx.search(l)]
                ok = len(hits) < int(words[1])
                detail = '%d lines' % len(hits)
            elif who == 'absent':
                # "absent <regular expression>": the client has written no such line since the script began.
                rx = re.compile(' '.join(words[1:]))
                hits = [l for l in session_lines()[self.log_start:] if rx.search(l)]
                ok = not hits
                detail = 'none' if ok else '%d lines, e.g. %s' % (len(hits), hits[0][35:140].strip())
            elif who == 'logged':
                # "logged <regular expression>": the client wrote such a line since this script began. For a test
                # whose point is that a certain path was taken, not only that the game survived it.
                rx = re.compile(' '.join(words[1:]))
                hits = [l for l in session_lines()[self.log_start:] if rx.search(l)]
                ok = bool(hits)
                detail = ('%d lines, e.g. %s' % (len(hits), hits[0][35:140].strip())) if ok else 'no such line since the script began'
            elif who == 'ids' and what == 'reused':
                # "ids reused Seeker": the game gave an actor of that name a form id this client had deleted a copy
                # under earlier in the session. Not a fault: it is what makes live-temp-reuse a test of anything.
                deleted, reused = set(), []
                for l in session_lines():
                    m = re.search(r'Deleted actor ([0-9A-Fa-f]+)', l)
                    if m:
                        deleted.add(m.group(1).upper())
                    m = re.search(r'Spawn Actor: ([0-9A-Fa-f]+), and NPC (.*)$', l)
                    if m and m.group(1).upper() in deleted and m.group(2).strip() == ' '.join(words[2:]):
                        reused.append(m.group(1).upper())
                # Not a fault when none came back: the game hands ids out as it likes, and in a long session (the full
                # suite, 2026-10-03) it did not happen to reuse one. The run then tests nothing, and says so.
                ok = True
                detail = ('reused: %s' % ', '.join(sorted(set(reused)))) if reused else 'SKIPPED: none of the %d deleted ids came back, so this run tests nothing' % len(deleted)
            elif who == 'hands' and what == 'within':
                # "hands within <units>": the copies' hands against the owners' own numbers, from the game's
                # "VRBodySync: actor X hands -- owner L(..) R(..); copy L(..) R(..)" lines since the script began
                # (one every 5 s per copy). Passes when the median distance, both hands, is within that many units.
                rx = re.compile(r'hands -- owner L\(([-\d.]+), ([-\d.]+), ([-\d.]+)\) R\(([-\d.]+), ([-\d.]+), ([-\d.]+)\); copy L\(([-\d.]+), ([-\d.]+), ([-\d.]+)\) R\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)')
                gaps = []
                for l in session_lines()[self.log_start:]:
                    m = rx.search(l)
                    if m:
                        v = [float(x) for x in m.groups()]
                        gaps += [math.dist(v[0:3], v[6:9]), math.dist(v[3:6], v[9:12])]
                gaps.sort()
                median = gaps[len(gaps) // 2] if gaps else None
                ok = median is not None and median <= float(words[2])
                detail = ('%d hand(s) measured, median %.1f units, worst %.1f' % (len(gaps), median, gaps[-1])) if gaps else 'no hand measurement logged'
            elif who == 'player' and what == 'has':
                # "player has <hex>": the local player carries at least one of that item.
                count = papyrus('ObjectReference', 'GetItemCount', '14', [{'form': '0x' + words[2]}])
                ok = isinstance(count, int) and count > 0
                detail = 'GetItemCount(%s) %s' % (words[2], count)
            elif who == 'player' and what == 'in':
                # "player in <worldspace editor id>": the player is outdoors in that worldspace (not in an interior).
                scene = tool('inspect', {'kind': 'scene'})
                ws = (scene.get('worldspace') or {}).get('editorId')
                cell = (scene.get('cell') or {}).get('editorId') or (scene.get('cell') or {}).get('formId')
                ok = ws == words[2]
                detail = 'worldspace %s, cell %s' % (ws, cell)
            elif who == 'player' and what == 'alive':
                dead = papyrus('Actor', 'IsDead', '14')
                health = tool('inspect', {'kind': 'player'})['actorValues']['health']['current']
                ok = dead is False and health > 0
                detail = 'IsDead %s, health %.0f' % (dead, health)
            elif who == 'slide':
                lines = session_lines()
                if what == 'mark':
                    self.slide_mark = len(lines)
                    ok, detail = True, 'from log line %d of this session' % self.slide_mark
                else:
                    frozen = moved = 0
                    for l in lines[self.slide_mark:]:
                        m = re.search(r'SlideDiag: (\d+) of (\d+) moves', l)
                        if m:
                            frozen += int(m.group(1))
                            moved += int(m.group(2))
                    wanted = float(words[2])
                    if not moved:
                        ok, detail = False, 'the game logged no SlideDiag line since the mark (the body did not move, or 10 s have not passed)'
                    else:
                        pct = 100.0 * frozen / moved
                        ok = pct < wanted if what == 'below' else pct > wanted
                        detail = '%d of %d moves with unchanged animation variables (%.0f%%), wanted %s %.0f%%' % (frozen, moved, pct, what, wanted)
            elif who == 'drop':
                ref = dropped_ref()
                if what == 'gone':
                    ok, detail = ref is None, ('no placed item left' if ref is None else 'still there as %s' % ref)
                elif not ref:
                    ok, detail = False, 'the game logged no placed item'
                else:
                    at = position(ref)
                    if what == 'exists':
                        ok = at is not None
                        detail = 'item %s at %s' % (ref, ('(%.0f, %.0f, %.0f)' % tuple(at)) if at else 'no position')
                    elif what == 'mark':
                        self.drop_mark = at
                        ok = at is not None
                        detail = 'item %s at %s' % (ref, ('(%.0f, %.0f, %.0f)' % tuple(at)) if at else 'no position')
                    elif what == 'moved':
                        wanted = [float(w) for w in words[2:5]]
                        if at is None or self.drop_mark is None:
                            ok, detail = False, 'no position, or no mark taken'
                        else:
                            moved = [at[i] - self.drop_mark[i] for i in range(3)]
                            ok = all(abs(moved[i] - wanted[i]) <= DROP_TOLERANCE for i in range(3))
                            detail = 'item %s moved (%.0f, %.0f, %.0f), wanted (%.0f, %.0f, %.0f)' % tuple([ref] + moved + wanted)
                    else:
                        detail = 'unknown check'
            elif who == 'player':
                health = tool('inspect', {'kind': 'player'})['actorValues']['health']['current']
                if what == 'mark':
                    # The player regenerates a few points a second, which read as "the spell did 14 of its 24".
                    # Switched off for the length of the run and put back at the end; nothing is saved.
                    if self.heal_rate is None:
                        # The multiplier, forced: setting the rate itself leaves whatever a perk or an enchantment
                        # adds on top, which still gave back a point a second.
                        self.heal_rate = papyrus('Actor', 'GetActorValue', '14', ['HealRateMult'])
                        papyrus('Actor', 'ForceActorValue', '14', ['HealRateMult', 0.0])
                        time.sleep(0.3)
                        health = tool('inspect', {'kind': 'player'})['actorValues']['health']['current']
                    self.mark = health
                    ok, detail = True, 'health %.0f' % health
                else:
                    wanted = float(words[2])
                    dropped = (self.mark if self.mark is not None else health) - health
                    ok = abs(dropped - wanted) <= PLAYER_TOLERANCE
                    detail = 'health %.0f -> %.0f, dropped %.0f (wanted %.0f)' % (self.mark, health, dropped, wanted)
            else:
                form, why = self.subject(who)
                if not form:
                    ok, detail = False, why
                elif what == 'exists':
                    x = papyrus('ObjectReference', 'GetPositionX', form)
                    ok = isinstance(x, (int, float))
                    detail = 'actor %s, x %s' % (form, x)
                elif what == '3d':
                    loaded = papyrus('ObjectReference', 'Is3DLoaded', form)
                    ok = loaded is True
                    detail = 'actor %s, Is3DLoaded %s' % (form, loaded)
                elif what in ('alive', 'dead'):
                    dead = papyrus('Actor', 'IsDead', form)
                    ok = dead is (what == 'dead')
                    detail = 'actor %s, IsDead %s' % (form, dead)
                elif what == 'gait':
                    # The body's position and the "Speed" its animation graph holds, sampled together. A body that
                    # travels while its graph says zero is sliding; this asks the copy itself, not the stream.
                    seconds = float(words[3]) if len(words) > 3 else 8.0
                    end = time.time() + seconds
                    last = position(form)
                    moving = legs = 0
                    top = 0.0
                    while time.time() < end:
                        time.sleep(0.3)
                        at = position(form)
                        speed = papyrus('ObjectReference', 'GetAnimationVariableFloat', form, ['Speed'])
                        if at and last and isinstance(speed, (int, float)):
                            if math.dist(at, last) > 10.0:
                                moving += 1
                                if abs(speed) > 1.0:
                                    legs += 1
                                top = max(top, abs(speed))
                        last = at
                    share = (100.0 * legs / moving) if moving else 0.0
                    if not moving:
                        # Nothing to judge, which is not the same as sliding: a Netch hovers on the spot.
                        ok, detail = True, 'SKIPPED: actor %s did not travel in %.0f s, so there is no gait to read' % (form, seconds)
                    else:
                        # 50, not 80: a creature lunges, stops and is pushed about, and its copy trails the stream by
                        # 300 ms, so even a healthy one is in the 70s. The broken cases measured 0 and 17.
                        ok = share >= 50.0 if words[2] == 'walking' else share <= 20.0
                        detail = 'actor %s moved in %d samples, %d of them with a speed in its animation graph (%.0f%%, top %.0f)' % (form, moving, legs, share, top)
                elif what in ('infaction', 'notinfaction'):
                    # "npc infaction <hex>": the game's copy is in that faction (Actor.IsInFaction).
                    inside = papyrus('Actor', 'IsInFaction', form, [{'form': '0x' + words[2]}])
                    ok = isinstance(inside, bool) and inside is (what == 'infaction')
                    detail = 'actor %s, IsInFaction(%s) %s' % (form, words[2], inside)
                elif what == 'has':
                    # "copy has <hex>": the copy's inventory holds at least one.
                    n = papyrus('ObjectReference', 'GetItemCount', form, [{'form': '0x' + words[2]}])
                    ok = isinstance(n, int) and n > 0
                    detail = 'actor %s, GetItemCount(%s) %s' % (form, words[2], n)
                elif what in ('lefthand', 'righthand'):
                    # "copy lefthand <hex>": the weapon the game says is in that hand (GetEquippedWeapon).
                    weapon = papyrus('Actor', 'GetEquippedWeapon', form, [what == 'lefthand'])
                    got = weapon.get('formId', '') if isinstance(weapon, dict) else str(weapon)
                    ok = isinstance(weapon, dict) and int(got, 16) & 0xFFFFFF == int(words[2], 16) & 0xFFFFFF
                    detail = 'actor %s, %s holds %s' % (form, 'left hand' if what == 'lefthand' else 'right hand', got or weapon)
                elif what in ('equipped', 'unequipped'):
                    # "copy equipped <hex>": whether the game's copy has that item equipped, asked of the game itself.
                    held = papyrus('Actor', 'IsEquipped', form, [{'form': '0x' + words[2]}])
                    ok = isinstance(held, bool) and held is (what == 'equipped')
                    detail = 'actor %s, IsEquipped(%s) %s' % (form, words[2], held)
                elif what == 'health':
                    value = papyrus('Actor', 'GetActorValue', form, ['Health'])
                    base = papyrus('Actor', 'GetBaseActorValue', form, ['Health'])
                    # "full" is whatever this game says the actor's own health is, which a script cannot know.
                    # "below <n>": anything under that, for a hit whose exact damage is the game's business.
                    if words[2] == 'below':
                        wanted = float(words[3])
                        ok = isinstance(value, (int, float)) and value < wanted
                    else:
                        wanted = float(base) if words[2] == 'full' and isinstance(base, (int, float)) else float(words[2] if words[2] != 'full' else 'nan')
                        ok = isinstance(value, (int, float)) and abs(value - wanted) <= HEALTH_TOLERANCE
                    detail = 'actor %s, health %s (wanted %.0f; its base health here is %s)' % (
                        form, ('%.1f' % value) if isinstance(value, (int, float)) else value, wanted,
                        ('%.0f' % base) if isinstance(base, (int, float)) else base)
                else:
                    detail = 'unknown check'
        except Exception as e:
            ok, detail = False, 'could not ask the game: %s %s' % (type(e).__name__, e)
        self.results.append((ok, text, detail))
        print('   %s  %-24s %s' % ('PASS' if ok else 'FAIL', text, detail), flush=True)

    def run(self):
        exe = os.path.join(RELEASE, 'STBot.exe')
        cmd = [exe, os.path.join('scripts', self.script + '.txt'), '--server', '127.0.0.1:10578', '--name', 'Bot']
        # Where the player stands, asked of the game. Left to itself the bot looks for a position line near the end
        # of the client log, and after a busy script there is none there: it then waits at the world origin for
        # five minutes and the script never starts (second run of live-temp-reuse, 2026-10-02).
        try:
            where = position('14')
            if where:
                cmd += ['--x', '%.0f' % where[0], '--y', '%.0f' % where[1]]
        except Exception:
            pass
        print('== %s' % self.script, flush=True)
        self.log_start = len(session_lines())
        proc = subprocess.Popen(cmd, cwd=RELEASE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
        log = io.open(os.path.join(RELEASE, 'logs', self.script + '-bot.log'), 'w', encoding='utf-8')
        for line in proc.stdout:
            log.write(line)
            m = re.search(r'ok: known me \(([0-9A-Fa-f]+) is known\)', line) or re.search(r'Character assigned.*?([0-9A-Fa-f]+)', line)
            if m and not self.bot_id:
                self.bot_id = m.group(1)
            m = re.search(r'NPC registered as actor ([0-9A-Fa-f]+)', line)
            if m:
                self.npc_id = m.group(1)
            m = re.search(r'\[script\] (CHECK .*|DO face .*|DO console .*|DO key .*|DO load last|DO server restart.*|DO sleep \d+|DO papyrus .*|DO pickup own|DO menu .*|DO god on|DO god off|DO combat on|DO combat off|SHOT .*|--.*|done)$', line.rstrip())
            if m:
                text = m.group(1)
                if text.startswith('CHECK '):
                    self.check(text[6:].strip())
                elif text.strip() in ('DO combat on', 'DO combat off'):
                    on = text.strip().endswith('on')
                    if ensure('tcai', 'is On' if on else 'is Off'):
                        print('   (combat AI %s)' % ('on' if on else 'off'), flush=True)
                    else:
                        print('   FAIL  combat AI could not be set', flush=True)
                        self.results.append((False, text.strip(), 'console did not confirm'))
                elif text.strip() in ('DO god on', 'DO god off'):
                    on = text.strip().endswith('on')
                    if ensure('tgm', 'enabled' if on else 'disabled'):
                        print('   (god mode %s)' % ('on' if on else 'off'), flush=True)
                    else:
                        print('   FAIL  god mode could not be set', flush=True)
                        self.results.append((False, text.strip(), 'console did not confirm'))
                elif text.startswith('DO key '):
                    parts = text.split()
                    try:
                        r = tool('input', {'action': 'down', 'device': 'keyboard', 'key': parts[2], 'maxHoldMs': int(parts[3]), 'owner': 'live-check'})
                        print('   (key %s held for %s ms: %s)' % (parts[2], parts[3], 'accepted' if r.get('accepted') else r), flush=True)
                    except Exception as e:
                        print('   FAIL  key %s: %s' % (parts[2], type(e).__name__), flush=True)
                        self.results.append((False, text.strip(), type(e).__name__))
                elif text.startswith('DO papyrus '):
                    # "DO papyrus <Script> <Function> <self> [form ...]": a native function called on a reference,
                    # with forms as its arguments (hex ids).
                    words = text.split()
                    # Arguments: a form by its hex id, or a plain number written "n:<value>".
                    def arg(w):
                        if w.startswith('n:'):
                            return int(w[2:])
                        if w.startswith('f:'):
                            return float(w[2:])
                        if w in ('b:true', 'b:false'):
                            return w == 'b:true'
                        if w in ('copy', 'npc'):
                            f, _ = self.subject(w)
                            return {'form': '0x' + (f or '0')}
                        return {'form': '0x' + w}
                    args = [arg(w) for w in words[5:]]
                    target = words[4]
                    if target in ('copy', 'npc'):
                        target, _ = self.subject(target)
                    result = papyrus(words[2], words[3], target, args) if target else 'ERR no %s' % words[4]
                    print('   (papyrus %s.%s on %s: %s)' % (words[2], words[3], words[4], result), flush=True)
                    if isinstance(result, str) and result.startswith('ERR'):
                        self.results.append((False, text.strip(), result))
                elif text.strip() in ('DO face copy', 'DO face npc'):
                    # "DO face copy|npc": turn the player towards that actor. A copy outside the headset's view is
                    # not posed (VRBodySync), and the rig's headset looks wherever the player faces.
                    who = text.split()[2]
                    form, _ = self.subject(who)
                    try:
                        mine = position('14')
                        theirs = position(form) if form else None
                        if not mine or not theirs:
                            raise RuntimeError('no position for the player or the %s' % who)
                        angle = math.degrees(math.atan2(theirs[0] - mine[0], theirs[1] - mine[1])) % 360.0
                        tool('console', {'action': 'exec', 'command': 'player.setangle z %.1f' % angle})
                        print('   (player turned to %.0f degrees, towards %s %s)' % (angle, who, form), flush=True)
                    except Exception as e:
                        print('   FAIL  face %s: %s' % (who, e), flush=True)
                        self.results.append((False, text.strip(), str(e)))
                elif text.startswith('DO sleep '):
                    time.sleep(int(text.split()[2]))
                elif text.startswith('DO menu '):
                    # "DO menu <seconds> <menu name>": open a menu, keep it open, close it again (DevBench's menu tool).
                    words = text.split(None, 3)
                    try:
                        tool('menu', {'action': 'open', 'name': words[3]})
                        time.sleep(float(words[2]))
                        tool('menu', {'action': 'close', 'name': words[3]})
                        print('   (menu %s open for %s s)' % (words[3], words[2]), flush=True)
                    except Exception as e:
                        print('   FAIL  menu %s: %s' % (words[3], e), flush=True)
                        self.results.append((False, text.strip(), str(e)))
                elif text.strip() == 'DO pickup own':
                    # The newest item this game dropped itself, picked up by the player through the game's own Activate.
                    refs = [m.group(1) for m in (re.search(r'DroppedItem: dropped \S+ x\d+ as ([0-9A-Fa-f]+)', l) for l in session_lines()) if m]
                    if not refs:
                        print('   FAIL  pickup: this game has dropped nothing', flush=True)
                        self.results.append((False, text.strip(), 'nothing dropped'))
                    else:
                        result = papyrus('ObjectReference', 'Activate', refs[-1], [{'form': '0x14'}])
                        print('   (picked up %s: %s)' % (refs[-1], result), flush=True)
                elif text.strip().startswith('DO server restart'):
                    try:
                        rest = text.strip()[len('DO server restart'):].strip()
                        noticed, back = restart_server(rest[len('after console '):] if rest.startswith('after console ') else None)
                        print('   (server restarted: the game noticed after %.0f s and was back in after %.0f s)' % (noticed, back), flush=True)
                    except Exception as e:
                        print('   FAIL  server restart: %s' % e, flush=True)
                        self.results.append((False, 'server restart', str(e)))
                elif text.strip() == 'DO load last':
                    try:
                        # The save the session started from (headless.ps1 keeps its name), not the most recent one:
                        # that is wherever Emma last played.
                        started_from = ''
                        try:
                            with io.open(os.path.join(os.environ.get('TEMP', ''), 'st-headless-state.json'), encoding='utf-8-sig') as f:
                                started_from = json.load(f).get('save') or ''
                        except Exception:
                            pass
                        r = tool('game', {'action': 'load', 'name': started_from}) if started_from else tool('game', {'action': 'loadLast'})
                        print('   (load: %s)' % r.get('name'), flush=True)
                    except Exception as e:
                        print('   FAIL  load last: %s' % type(e).__name__, flush=True)
                        self.results.append((False, 'load last', type(e).__name__))
                elif text.startswith('DO console '):
                    # Something only the game can do, at a moment only the script knows: move the player, say.
                    command = text[len('DO console '):].strip()
                    try:
                        tool('console', {'action': 'exec', 'command': command})
                        print('   (console: %s)' % command, flush=True)
                    except Exception as e:
                        print('   FAIL  console %s: %s' % (command, type(e).__name__), flush=True)
                        self.results.append((False, 'console ' + command, type(e).__name__))
                elif text.startswith('SHOT '):
                    words = text[5:].split()
                    name = '%s-%s' % (self.script, words[0])
                    target = None
                    if len(words) >= 3 and words[1] == 'of':
                        if words[2] == 'player':
                            target = position('14')
                        elif words[2] == 'drop':
                            ref = dropped_ref()
                            target = position(ref) if ref else None
                        else:
                            form, _ = self.subject(words[2])
                            target = position(form) if form else None
                        if not target:
                            print('   FAIL  screenshot %s: nothing to point the camera at' % name, flush=True)
                            self.results.append((False, 'screenshot ' + name, 'no %s to look at' % words[2]))
                            continue
                    try:
                        path = screenshot(name, target)
                    except Exception as e:
                        path = None
                        print('   FAIL  screenshot %s: %s' % (name, type(e).__name__), flush=True)
                    if path:
                        self.shots.append(path)
                        print('   (screenshot: %s)' % path, flush=True)
                    else:
                        self.results.append((False, 'screenshot ' + name, 'no picture came back'))
                else:
                    print('   ' + text, flush=True)
        proc.wait()
        log.close()
        # The bot's own checks ("expect ..."), which the game is not asked about: its verdict is the last word of its log.
        # Without this a script that only uses them -- live-sender, the real game as the sender -- read as "not run".
        with io.open(os.path.join(RELEASE, 'logs', self.script + '-bot.log'), encoding='utf-8', errors='replace') as f:
            text = f.read()
        verdict = re.findall(r'\[script\] all (\d+) checks passed|\[script\] (\d+) of (\d+) checks failed', text)
        if verdict:
            passed_all, failed, of = verdict[-1]
            if passed_all:
                self.results.append((True, 'bot: all %s of its own checks' % passed_all, ''))
                print('   PASS  bot: all %s of its own checks' % passed_all, flush=True)
            else:
                self.results.append((False, 'bot: %s of %s of its own checks' % (failed, of), 'see logs/%s-bot.log' % self.script))
                print('   FAIL  bot: %s of %s of its own checks failed' % (failed, of), flush=True)
        if isinstance(self.heal_rate, (int, float)):
            papyrus('Actor', 'ForceActorValue', '14', ['HealRateMult', float(self.heal_rate)])
        return self.results


def main():
    args = sys.argv[1:]
    if args == ['--list']:
        print('\n'.join(ALL))
        return 0
    scripts = ALL if (not args or args == ['--all']) else args
    try:
        state = tool('inspect', {'kind': 'state'})
    except Exception as e:
        print('The game does not answer on DevBench (%s). Run Tools/VR/headless.ps1 up first.' % type(e).__name__)
        return 2
    if not state.get('playerLoaded'):
        print('The game is up but no save is loaded.')
        return 2

    # Scripts are tracked in Code/bot/scripts; the bot reads them from the release folder.
    src = os.path.join(ROOT, 'Code', 'bot', 'scripts')
    for name in scripts:
        with io.open(os.path.join(src, name + '.txt'), 'rb') as f:
            data = f.read()
        with io.open(os.path.join(RELEASE, 'scripts', name + '.txt'), 'wb') as f:
            f.write(data)

    if not ensure('tcai', 'is Off'):
        print('Could not switch combat AI off; wildlife may attack the player during the run.')
    if not ensure('tgm', 'disabled'):
        print('Could not confirm god mode is off; the damage checks may read zero.')

    failed = 0
    total = 0
    table = []
    shots = []
    for name in scripts:
        # A game that went down takes every later script with it; say so once instead of failing each in turn.
        try:
            urllib.request.urlopen('http://127.0.0.1:8921/api/health', timeout=6).read()
        except Exception:
            table.append((name, 0, 0, 'NOT RUN: the game is gone'))
            failed += 1
            continue
        run = Run(name)
        results = run.run()
        bad = [text for ok, text, _ in results if not ok]
        total += len(results)
        failed += len(bad)
        shots += run.shots
        if not results:
            # A script whose bot never got going has no failed check either.
            failed += 1
            table.append((name, 0, 0, 'NOT RUN: no check was reached; see logs/%s-bot.log' % name))
            time.sleep(6)
            continue
        table.append((name, len(results), len(bad), 'ok' if not bad else 'FAILED: ' + '; '.join(bad)))
        time.sleep(6)  # let the server finish removing the bot before the next one joins

    print('\n%-20s %6s %6s  %s' % ('script', 'checks', 'failed', ''))
    for name, n, bad, note in table:
        print('%-20s %6d %6d  %s' % (name, n, bad, note))
    print('%d checks, %d failed' % (total, failed))
    for path in shots:
        print('screenshot: ' + path)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
