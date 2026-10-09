"""Does a player who leaves before getting a character take somebody else's character with them?

    python Tools\\VR\\leaver-check.py

No game needed: a fresh server and three bots.

  Host     is there first and stays. On a fresh server its character is entity 0.
  Leaver   connects where nobody is, so it never asks for a character, and goes away after a few seconds.
  Checker  joins next to Host afterwards and looks for Host's character.

Found on 2026-10-02 with the real game as Host: after a bot that never found the player had given up, the server
logged "Entity is invalid: 0" four times a second and no newcomer was ever sent Emma's character again. The clean-up
for a leaving player (GameServer.cpp, "Cleanup all entities that we own") removed "the leaver's own character",
which for a leaver without one was written as entity 0 -- a real entity, and the first one a server makes.

The server must be fresh, or entity 0 is long gone; this script stops a running server and starts its own. It
refuses to do that while the game is running.
"""
import io, os, re, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
RELEASE = os.path.join(ROOT, 'build', 'windows', 'x64', 'release')
SERVER_LOG = os.path.join(RELEASE, 'logs', 'STServerOut.log')
BOT = os.path.join(RELEASE, 'STBot.exe')


def running(name):
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq ' + name], capture_output=True, text=True).stdout
    return name.lower() in out.lower()


def server_lines(skip):
    with io.open(SERVER_LOG, encoding='utf-8', errors='replace') as f:
        lines = f.read().splitlines()
    return lines[skip:] if skip <= len(lines) else lines


def bot(script, name, log, *extra):
    out = io.open(os.path.join(RELEASE, 'logs', log), 'w', encoding='utf-8')
    cmd = [BOT, os.path.join('scripts', script), '--server', '127.0.0.1:10578', '--name', name, '--worldspace', '3C'] + list(extra)
    return subprocess.Popen(cmd, cwd=RELEASE, stdout=out, stderr=subprocess.STDOUT), out


def wait_for(what, seconds, test):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if test():
            return True
        time.sleep(0.5)
    print('   gave up waiting for ' + what, flush=True)
    return False


def read(log):
    with io.open(os.path.join(RELEASE, 'logs', log), encoding='utf-8', errors='replace') as f:
        return f.read()


def main():
    if running('urSovngarde.exe') or running('SkyrimTogetherVR.exe') or running('SkyrimVR.exe'):
        print('The game is running; this test restarts the server. Not started.')
        return 2

    for name in ('leaver-host', 'leaver-checker'):
        with io.open(os.path.join(ROOT, 'Code', 'bot', 'scripts', name + '.txt'), 'rb') as f:
            data = f.read()
        with io.open(os.path.join(RELEASE, 'scripts', name + '.txt'), 'wb') as f:
            f.write(data)

    subprocess.run(['taskkill', '/F', '/IM', 'SkyrimTogetherServer.exe'], capture_output=True)
    subprocess.run(['taskkill', '/F', '/IM', 'STBot.exe'], capture_output=True)
    time.sleep(2)
    skip = len(server_lines(0)) if os.path.exists(SERVER_LOG) else 0
    server = subprocess.Popen([os.path.join(RELEASE, 'SkyrimTogetherServer.exe')], cwd=RELEASE, creationflags=0x00000010,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(4)
    if os.path.exists(SERVER_LOG) and len(server_lines(0)) < skip:
        skip = 0   # the log rotated at start-up

    procs = []
    ok = False
    try:
        host, host_out = bot('leaver-host.txt', 'Host', 'leaver-host.log', '--standalone', '--host-timeout', '2', '--x', '0', '--y', '0', '--max-runtime', '150')
        procs.append(host)
        if not wait_for('Host to be in the world', 40, lambda: 'In the world: our character is' in read('leaver-host.log')):
            return 1
        m = re.search(r'In the world: our character is ([0-9A-Fa-f]+)', read('leaver-host.log'))
        print('Host is in the world as character %s%s' % (m.group(1), '' if m.group(1) == '0' else '  (not 0: the test proves nothing)'), flush=True)
        if m.group(1) != '0':
            return 1

        # Far from anybody, and not standalone: it looks for a host, finds none, and never asks for a character.
        leaver, _ = bot('leaver-host.txt', 'Leaver', 'leaver-leaver.log', '--x', '400000', '--y', '400000', '--max-runtime', '6')
        procs.append(leaver)
        leaver.wait(timeout=60)
        had_character = 'In the world: our character is' in read('leaver-leaver.log')
        print('Leaver has gone%s' % ('  (it did get a character: the test proves nothing)' if had_character else ', without ever having a character'), flush=True)
        if had_character:
            return 1
        wait_for('the server to notice Leaver has gone', 40, lambda: any("'Leaver' disconnected" in l for l in server_lines(skip)))
        time.sleep(3)

        checker, _ = bot('leaver-checker.txt', 'Checker', 'leaver-checker.log', '--x', '200', '--y', '0', '--max-runtime', '70')
        procs.append(checker)
        code = checker.wait(timeout=120)
        invalid = sum(1 for l in server_lines(skip) if 'Entity is invalid: 0' in l)
        saw_host = 'ok: known other' in read('leaver-checker.log')
        ok = code == 0 and saw_host and invalid == 0
        print('%s  Checker %s Host\'s character; the server said "Entity is invalid: 0" %d times' %
              ('PASS' if ok else 'FAIL', 'was sent' if saw_host else 'was never sent', invalid), flush=True)
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'SkyrimTogetherServer.exe'], capture_output=True)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
