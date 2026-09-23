"""Name the functions in the newest crash from tp_client.log.

The client logs a crash as a register dump plus a scan of the stack for anything that looks like code.
Every address is printed as `SkyrimVR.exe+0xRVA`, because the launcher loads the game image inside its
own module, so the log cannot tell our code from the game's. This does.

The exe's `.game` section is the buffer the game image is loaded into, so an address inside it is game
code and there is nothing to look up (the game exe on disk is Steam-encrypted). Anything after it is
ours, and the PDB next to the exe gives the function name and source line.

Usage:
    python explain-crash.py                     # newest crash in the deployed client's log
    python explain-crash.py --log <path>        # a log from elsewhere, e.g. the friend's bundle
    python explain-crash.py --exe <path>        # match a log to the exe that produced it
    python explain-crash.py 0x145634621 ...     # just name these addresses

The exe and the PDB must be the pair that produced the crash. When a build has been deployed since,
use the `.old-*` copies the deploy keeps, or rebuild that commit.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import sys

DEFAULT_CLIENT = r'E:\FUS\tools\Skyrim Together VR\SkyrimTogetherVR.exe'
DEFAULT_LOG = r'E:\FUS\tools\Skyrim Together VR\logs\tp_client.log'
BASE = 0x140000000

MAX_SYM_NAME = 2000


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [
        ('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_ulonglong * 2),
        ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_ulonglong),
        ('Flags', wt.ULONG), ('Value', ctypes.c_ulonglong), ('Address', ctypes.c_ulonglong),
        ('Register', wt.ULONG), ('Scope', wt.ULONG), ('Tag', wt.ULONG),
        ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG), ('Name', ctypes.c_char * MAX_SYM_NAME),
    ]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', ctypes.c_void_p),
                ('LineNumber', wt.DWORD), ('FileName', ctypes.c_char_p), ('Address', ctypes.c_ulonglong)]


def pair_for(exe):
    """dbghelp finds a PDB by the name recorded in the exe, so a deploy's `.old-<stamp>` copies are
    invisible to it. Stage such a pair under its real names in a temp folder and load that instead."""
    import shutil
    import tempfile
    base = os.path.basename(exe)
    if '.old-' not in base:
        return exe
    real, stamp = base.split('.old-', 1)
    pdb = os.path.join(os.path.dirname(exe), '%s.old-%s' % (os.path.splitext(real)[0] + '.pdb', stamp))
    if not os.path.exists(pdb):
        print('note: no matching .pdb for %s, names will be wrong' % base)
        return exe
    staged = os.path.join(tempfile.gettempdir(), 'st-crash-%s' % stamp)
    os.makedirs(staged, exist_ok=True)
    exe_out = os.path.join(staged, real)
    pdb_out = os.path.join(staged, os.path.splitext(real)[0] + '.pdb')
    for src, dst in ((exe, exe_out), (pdb, pdb_out)):
        if not os.path.exists(dst) or os.path.getmtime(src) > os.path.getmtime(dst):
            shutil.copy2(src, dst)
    return exe_out


class Symbols:
    def __init__(self, exe):
        exe = pair_for(exe)
        self.exe = exe
        self.game_end = 0
        self.dbghelp = ctypes.WinDLL('dbghelp.dll')
        handle = ctypes.WinDLL('kernel32.dll').GetCurrentProcess()
        self.handle = wt.HANDLE(handle)

        size = self._read_sections(exe)
        self.dbghelp.SymSetOptions(0x2 | 0x4 | 0x10 | 0x80000)  # UNDNAME | DEFERRED | LOAD_LINES | NO_PROMPTS
        if not self.dbghelp.SymInitialize(self.handle, None, False):
            raise SystemExit('SymInitialize failed')
        self.dbghelp.SymLoadModuleExW.restype = ctypes.c_ulonglong
        self.dbghelp.SymLoadModuleExW.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_wchar_p, ctypes.c_wchar_p,
                                                  ctypes.c_ulonglong, wt.DWORD, ctypes.c_void_p, wt.DWORD]
        self.dbghelp.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_ulonglong,
                                             ctypes.POINTER(ctypes.c_ulonglong), ctypes.POINTER(SYMBOL_INFO)]
        self.dbghelp.SymGetLineFromAddr64.argtypes = [wt.HANDLE, ctypes.c_ulonglong,
                                                      ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINE64)]
        if not self.dbghelp.SymLoadModuleExW(self.handle, None, exe, None, BASE, size, None, 0):
            raise SystemExit('SymLoadModuleEx failed for %s' % exe)

    def _read_sections(self, exe):
        """Section table straight from the PE headers, so pefile is not required."""
        with open(exe, 'rb') as fh:
            data = fh.read(0x1000)
        pe_off = int.from_bytes(data[0x3C:0x40], 'little')
        n_sections = int.from_bytes(data[pe_off + 6:pe_off + 8], 'little')
        opt_size = int.from_bytes(data[pe_off + 20:pe_off + 22], 'little')
        size_of_image = int.from_bytes(data[pe_off + 24 + 56:pe_off + 24 + 60], 'little')
        table = pe_off + 24 + opt_size
        for i in range(n_sections):
            entry = table + i * 40
            name = data[entry:entry + 8].rstrip(b'\x00').decode(errors='replace')
            rva = int.from_bytes(data[entry + 12:entry + 16], 'little')
            vsize = int.from_bytes(data[entry + 8:entry + 12], 'little')
            if name == '.game':
                self.game_end = rva + vsize
        return size_of_image

    def name(self, va):
        if va < BASE:
            va += BASE
        rva = va - BASE
        if self.game_end and rva < self.game_end:
            return 'the game (SkyrimVR.exe+0x%x)' % rva

        buf = SYMBOL_INFO()
        buf.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - MAX_SYM_NAME
        buf.MaxNameLen = MAX_SYM_NAME
        disp = ctypes.c_ulonglong(0)
        if not self.dbghelp.SymFromAddr(self.handle, ctypes.c_ulonglong(va), ctypes.byref(disp), ctypes.byref(buf)):
            return 'ours, no symbol (RVA 0x%x)' % rva
        text = '%s + 0x%x' % (buf.Name.decode(errors='replace'), disp.value)

        line = IMAGEHLP_LINE64()
        line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        col = wt.DWORD(0)
        if self.dbghelp.SymGetLineFromAddr64(self.handle, ctypes.c_ulonglong(va), ctypes.byref(col), ctypes.byref(line)):
            text += '   [%s:%d]' % (os.path.basename(line.FileName.decode(errors='replace')), line.LineNumber)
        return text


CRASH_START = re.compile(r'^\[([\d\-: .]+)\].*crash occurred!')
ADDR = re.compile(r'0x([0-9a-f]{6,16})\s+SkyrimVR\.exe\+0x[0-9a-f]+', re.I)
OTHER_MODULE = re.compile(r'0x([0-9a-f]{6,16})\s+(\S+\.(?:dll|exe|DLL|EXE))\+0x[0-9a-f]+')
RIP = re.compile(r'\brip 0x([0-9a-f]+)', re.I)
DETAIL = re.compile(r'(exception code is .*|faulting access: .*)')


def newest_crash(log_path):
    with open(log_path, 'r', encoding='utf-8', errors='replace') as fh:
        lines = fh.readlines()
    starts = [i for i, l in enumerate(lines) if CRASH_START.search(l)]
    if not starts:
        return None, None
    begin = starts[-1]
    end = begin
    while end + 1 < len(lines) and ('[error]' in lines[end + 1] or 'crash' in lines[end + 1].lower()):
        end += 1
    return lines[begin:end + 1], lines[begin].split(']')[0].lstrip('[')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('addresses', nargs='*')
    ap.add_argument('--log', default=DEFAULT_LOG)
    ap.add_argument('--exe', default=DEFAULT_CLIENT)
    args = ap.parse_args()

    syms = Symbols(args.exe)

    if args.addresses:
        for a in args.addresses:
            va = int(a, 0)
            print('%016x  %s' % (va if va >= BASE else va + BASE, syms.name(va)))
        return

    block, when = newest_crash(args.log)
    if not block:
        print('No crash found in %s' % args.log)
        return

    print('Newest crash in %s' % args.log)
    print('  at %s' % when)
    for line in block:
        m = DETAIL.search(line)
        if m:
            print('  %s' % m.group(1).strip())

    joined = ''.join(block)
    m = RIP.search(joined)
    if m:
        print('\nFaulting instruction:')
        print('  %s' % syms.name(int(m.group(1), 16)))

    print('\nStack, top first (addresses the scan found; some are stale):')
    seen = set()
    for line in block:
        if RIP.search(line):
            continue
        other = OTHER_MODULE.search(line)
        if other and not other.group(2).lstrip('(').lower().startswith('skyrimvr'):
            text = '%s+...' % other.group(2).lstrip('(')
        else:
            m = ADDR.search(line)
            if not m:
                continue
            text = syms.name(int(m.group(1), 16))
        if text in seen:
            continue
        seen.add(text)
        print('  %s' % text)


if __name__ == '__main__':
    main()
