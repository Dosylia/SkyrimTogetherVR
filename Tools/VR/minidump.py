"""Read a Windows minidump far enough to answer one question: whose code is on the faulting stack.

There is no debugger installed on the machines this mod is developed and played on, and the client's own
crash handler only labels the stack addresses it happens to recognise. A minidump carries the module list
and the thread stacks, which is enough to attribute every address properly.

One wrinkle is specific to this project: the launcher *replaces* the game's executable, loading the game
image inside its own module, so a dump shows a single ~90 MB module named SkyrimTogetherVR.exe holding both
the game's code and ours. Module names therefore cannot tell the two apart -- see mapsym.py, which can.

Usage:
    python minidump.py <path to .dmp>

Also importable: `from minidump import Dump`.
"""
import struct
import sys
import collections

STREAM = {3: 'ThreadList', 4: 'ModuleList', 5: 'MemoryList', 6: 'Exception', 7: 'SystemInfo', 9: 'Memory64List'}


class Dump:
    def __init__(self, path):
        self.f = open(path, 'rb')
        sig, ver, nstreams, dir_rva = struct.unpack('<4sIII', self.rd(0, 16))
        assert sig == b'MDMP', 'not a minidump'
        self.dirs = {}
        for i in range(nstreams):
            t, size, rva = struct.unpack('<III', self.rd(dir_rva + i * 12, 12))
            self.dirs[t] = (size, rva)

    def rd(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def string(self, rva):
        n = struct.unpack('<I', self.rd(rva, 4))[0]
        return self.rd(rva + 4, n).decode('utf-16-le', 'replace')

    def modules(self):
        size, rva = self.dirs[4]
        n = struct.unpack('<I', self.rd(rva, 4))[0]
        out = []
        for i in range(n):
            b = self.rd(rva + 4 + i * 108, 108)
            base, imgsize, _ck, _ts, name_rva = struct.unpack('<QIIII', b[:24])
            out.append((base, imgsize, self.string(name_rva).split('\\')[-1]))
        return sorted(out)

    def module_timestamp(self, name):
        """The PE TimeDateStamp a module was built with, for checking a linker map against this dump."""
        size, rva = self.dirs[4]
        n = struct.unpack('<I', self.rd(rva, 4))[0]
        for i in range(n):
            b = self.rd(rva + 4 + i * 108, 108)
            _base, _imgsize, _ck, ts, name_rva = struct.unpack('<QIIII', b[:24])
            if self.string(name_rva).split('\\')[-1].lower() == name.lower():
                return ts
        return None

    def exception(self):
        # MINIDUMP_EXCEPTION_STREAM: thread 0, pad 4, then MINIDUMP_EXCEPTION at 8.
        # That record is code 0, flags 4, nested record 8, address 16, nparams 24 -- 28 bytes, not 32, and
        # getting it wrong silently shifts every field after it rather than failing.
        size, rva = self.dirs[6]
        tid, _pad = struct.unpack('<II', self.rd(rva, 8))
        code, flags, _rec, addr, nparams = struct.unpack('<IIQQI', self.rd(rva + 8, 28))
        params = struct.unpack('<15Q', self.rd(rva + 8 + 32, 120))
        ctx_size, ctx_rva = struct.unpack('<II', self.rd(rva + 8 + 152, 8))
        return tid, code, addr, params[:nparams], ctx_rva

    def context(self, rva):
        # CONTEXT_AMD64. Only the integer registers are wanted here; their offsets are fixed by the ABI.
        b = self.rd(rva, 1232)

        def g(o):
            return struct.unpack('<Q', b[o:o + 8])[0]

        return collections.OrderedDict(
            Rax=g(120), Rcx=g(128), Rdx=g(136), Rbx=g(144), Rsp=g(152), Rbp=g(160), Rsi=g(168), Rdi=g(176),
            R8=g(184), R9=g(192), R10=g(200), R11=g(208), R12=g(216), R13=g(224), R14=g(232), R15=g(240),
            Rip=g(248))

    def threads(self):
        # MINIDUMP_THREAD is 48 bytes: id, suspend, priclass, pri, teb, stack{start, size, rva}, ctx{size, rva}
        size, rva = self.dirs[3]
        n = struct.unpack('<I', self.rd(rva, 4))[0]
        out = []
        for i in range(n):
            b = self.rd(rva + 4 + i * 48, 48)
            tid, _s, _pc, _p, _teb, st_start, st_size, st_rva, _cs, c_rva = struct.unpack('<IIIIQQIIII', b)
            out.append(dict(tid=tid, stack_start=st_start, stack_size=st_size, stack_rva=st_rva, ctx_rva=c_rva))
        return out

    def memlist(self):
        # What a non-full dump actually stores. The stacks live here and in the thread list, not in
        # Memory64List, which is why the first version of this reader found nothing to walk.
        if 5 not in self.dirs:
            return []
        size, rva = self.dirs[5]
        n = struct.unpack('<I', self.rd(rva, 4))[0]
        out = []
        for i in range(n):
            start, dsize, drva = struct.unpack('<QII', self.rd(rva + 4 + i * 16, 16))
            out.append((start, dsize, drva))
        return out

    def mem64(self):
        if 9 not in self.dirs:
            return []
        size, rva = self.dirs[9]
        nranges, base_rva = struct.unpack('<QQ', self.rd(rva, 16))
        out, off = [], base_rva
        for i in range(nranges):
            start, dsize = struct.unpack('<QQ', self.rd(rva + 16 + i * 16, 16))
            out.append((start, dsize, off))
            off += dsize
        return out

    def ranges(self, tid=None):
        """Every readable span of the dumped process's memory, as (start, size, file offset)."""
        out = self.mem64() + self.memlist()
        for th in self.threads():
            if th['stack_size'] and (tid is None or th['tid'] == tid):
                out.append((th['stack_start'], th['stack_size'], th['stack_rva']))
        return out

    def read(self, addr, count, ranges=None):
        """Read memory from the dumped process, or None if the dump does not contain that address."""
        for start, size, off in (ranges if ranges is not None else self.ranges()):
            if start <= addr < start + size:
                return self.rd(off + (addr - start), min(count, start + size - addr))
        return None


def main(path, depth=900):
    d = Dump(path)
    mods = d.modules()
    tid, code, addr, params, ctx_rva = d.exception()
    ctx = d.context(ctx_rva)

    def owner(a):
        for base, size, name in mods:
            if base <= a < base + size:
                return name, a - base
        return None, 0

    print("exception {:#010x} at {:#x}".format(code, addr))
    if code == 0xC0000005 and len(params) >= 2:
        kind = {0: 'read', 1: 'write', 8: 'execute'}.get(params[0], params[0])
        print("  access violation: {} at {:#x}".format(kind, params[1]))
    n, o = owner(addr)
    print("  faulting instruction: {}{}".format(n or 'unmapped', "+{:#x}".format(o) if n else ""))
    print("  faulting thread: {}".format(tid))
    print("  registers: " + ", ".join("{}={:#x}".format(k, v) for k, v in ctx.items()))
    for reg, val in ctx.items():
        n2, o2 = owner(val)
        if n2 and reg not in ('Rip', 'Rsp'):
            print("    {} points into {}+{:#x}".format(reg, n2, o2))

    ranges = d.ranges(tid)
    for th in d.threads():
        if th['tid'] == tid and th['stack_size']:
            print("  stack region: {:#x} .. {:#x}".format(th['stack_start'], th['stack_start'] + th['stack_size']))

    raw = d.read(ctx['Rsp'], depth * 8, ranges) or b''
    print("\n  stack walk from Rsp ({} slots readable), code addresses only:".format(len(raw) // 8))
    seen = collections.OrderedDict()
    for i in range(0, len(raw) - 7, 8):
        val = struct.unpack('<Q', raw[i:i + 8])[0]
        n2, o2 = owner(val)
        if not n2:
            continue
        key = (n2, o2)
        if key not in seen:
            seen[key] = ctx['Rsp'] + i
    for (n2, o2), where in seen.items():
        print("    [rsp+{:#06x}]  {}+{:#x}".format(where - ctx['Rsp'], n2, o2))

    tally = collections.Counter(n2 for (n2, _o) in seen)
    print("\n  modules appearing on this stack:")
    for name, c in tally.most_common():
        print("    {:4d}  {}".format(c, name))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1])
