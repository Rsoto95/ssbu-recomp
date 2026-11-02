"""Memory growth finder for the Test_*_mem.bat runs (Windows only, read-only).

Waits for ssbu_native.exe, then every INTERVAL seconds walks its address space from outside (VirtualQueryEx),
sums the committed private memory per allocation (AllocationBase) and writes to the output file:
  - the total and the 10 largest allocations,
  - every allocation that grew or shrank by 16 MB or more since the previous walk, with what kind of memory it is
    (protection) and the module that owns the address, when one does.
Usage: python scripts/memdiff.py <output.txt> [interval seconds, default 20]
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

OUT = sys.argv[1]
INTERVAL = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
MB = 1 << 20
BIG = 16 * MB

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_uint64), ('AllocationBase', ctypes.c_uint64), ('AllocationProtect', wt.DWORD),
                ('PartitionId', wt.WORD), ('RegionSize', ctypes.c_uint64), ('State', wt.DWORD), ('Protect', wt.DWORD),
                ('Type', wt.DWORD)]


k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.OpenProcess.restype = wt.HANDLE
MEM_COMMIT, MEM_PRIVATE = 0x1000, 0x20000
PROT = {0x01: 'noaccess', 0x02: 'r', 0x04: 'rw', 0x08: 'wc', 0x10: 'x', 0x20: 'rx', 0x40: 'rwx', 0x80: 'wcx'}


def find_pid():
    import subprocess
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq ssbu_native.exe', '/FO', 'CSV', '/NH'],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == 'ssbu_native.exe':
            return int(parts[1])
    return None


def walk(h):
    allocs = {}
    m = MBI()
    a = 0
    while k32.VirtualQueryEx(h, a, ctypes.byref(m), ctypes.sizeof(m)):
        if m.State == MEM_COMMIT and m.Type == MEM_PRIVATE:
            e = allocs.setdefault(m.AllocationBase, [0, {}])
            e[0] += m.RegionSize
            p = PROT.get(m.Protect & 0xff, hex(m.Protect))
            e[1][p] = e[1].get(p, 0) + m.RegionSize
        nxt = m.BaseAddress + m.RegionSize
        if nxt <= a:
            break
        a = nxt
    return allocs


def alive(h):
    code = wt.DWORD()
    return k32.GetExitCodeProcess(h, ctypes.byref(code)) and code.value == 259


def kinds(d):
    return ' '.join(f'{k}:{v / MB:.0f}' for k, v in sorted(d.items(), key=lambda kv: -kv[1]))


def main():
    pid = None
    for _ in range(1200):
        pid = find_pid()
        if pid:
            break
        time.sleep(0.5)
    with open(OUT, 'w') as f:
        if not pid:
            f.write('ssbu_native.exe never started\n')
            return
        h = k32.OpenProcess(0x0400 | 0x0010, False, pid)      # QUERY_INFORMATION | VM_READ
        f.write(f'pid {pid}, a walk every {INTERVAL:.0f} s\n')
        t0 = time.time()
        prev = None
        while alive(h):
            cur = walk(h)
            tot = sum(v[0] for v in cur.values())
            top = sorted(cur.items(), key=lambda kv: -kv[1][0])[:10]
            f.write(f'\n[{time.time() - t0:5.0f} s] committed private {tot / MB:.0f} MB in {len(cur)} allocations; largest: '
                    + ' '.join(f'{b:x}:{v[0] / MB:.0f}' for b, v in top) + '\n')
            if prev is not None:
                grown = 0
                for b in sorted(set(cur) | set(prev)):
                    now = cur.get(b, [0, {}])[0]
                    was = prev.get(b, [0, {}])[0]
                    d = now - was
                    grown += d
                    if abs(d) >= BIG:
                        f.write(f'   {b:x}: {was / MB:.0f} -> {now / MB:.0f} MB ({d / MB:+.0f})'
                                f'{" NEW" if not was else ""}{" GONE" if not now else ""} [{kinds(cur.get(b, prev.get(b))[1])}]\n')
                small = grown - sum(cur.get(b, [0])[0] - prev.get(b, [0])[0] for b in set(cur) | set(prev)
                                    if abs(cur.get(b, [0])[0] - prev.get(b, [0])[0]) >= BIG)
                f.write(f'   change in all {grown / MB:+.0f} MB (of it in allocations that moved less than 16 MB: {small / MB:+.0f} MB)\n')
                new = [b for b in cur if b not in prev]
                gone = [b for b in prev if b not in cur]
                hist = {}
                for b in new:
                    key = (cur[b][0] >> 10, kinds(cur[b][1]))
                    hist[key] = hist.get(key, 0) + 1
                f.write(f'   new allocations {len(new)} ({sum(cur[b][0] for b in new) / MB:.0f} MB), gone {len(gone)} '
                        f'({sum(prev[b][0] for b in gone) / MB:.0f} MB); new by size: '
                        + ', '.join(f'{n} x {k} KB [{kd}]' for (k, kd), n in sorted(hist.items(), key=lambda kv: -kv[1])[:8])
                        + ('; first: ' + ' '.join(f'{b:x}' for b in sorted(new)[:4]) if new else '') + '\n')
                small_grown = sorted(((cur[b][0] - prev[b][0], b) for b in cur if b in prev and 0 < cur[b][0] - prev[b][0] < BIG), reverse=True)[:5]
                if small_grown:
                    f.write('   existing allocations that grew most (under 16 MB): '
                            + ' '.join(f'{b:x}:+{d / MB:.1f}' for d, b in small_grown) + '\n')
            f.flush()
            prev = cur
            time.sleep(INTERVAL)
        f.write('\nthe game exited\n')


if __name__ == '__main__':
    main()
