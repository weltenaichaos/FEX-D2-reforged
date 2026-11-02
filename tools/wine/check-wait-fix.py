#!/usr/bin/env python3
# Tries FEX_WINE_WAIT_FIX's ntdll checks (Source/Windows/ARM64EC/WineWaitFix.cpp) on an ntdll.dll file, the way FEX
# sees it in an ARM64EC process: python3 check-wait-fix.py <aarch64-windows/ntdll.dll> --ec --rfe
# Prints the three functions' native code, the woken-mark clears, the code hash, Wine's lists and the verdict.
# Stock Proton 11.0-1: hash 25abfe54721af2aa, 'in use'. Our Proton d2r1: 'already has the fix, left alone'.
import struct, sys
f = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', f, 0x3c)[0]
nsec = struct.unpack_from('<H', f, pe + 6)[0]
optsz = struct.unpack_from('<H', f, pe + 20)[0]
opt = pe + 24
imagebase = struct.unpack_from('<Q', f, opt + 24)[0]
sizeofimage = struct.unpack_from('<I', f, opt + 56)[0]
dirs = opt + 112
def dd(i): return struct.unpack_from('<II', f, dirs + 8 * i)
img = bytearray(sizeofimage)
secs = []
for i in range(nsec):
    h = pe + 24 + optsz + 40 * i
    name = f[h:h+8].rstrip(b'\0').decode(errors='replace')
    vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', f, h + 8)
    ch = struct.unpack_from('<I', f, h + 36)[0]
    n = min(rawsize, vsize) if vsize else rawsize
    img[va:va+n] = f[rawptr:rawptr+n]
    size = vsize if vsize else rawsize
    secs.append((va, va + size, ch, name))
hdrsize = struct.unpack_from('<I', f, opt + 60)[0]
img[0:hdrsize] = f[0:hdrsize]
def u32(a): return struct.unpack_from('<I', img, a)[0]
def apply_arm64x():
    lrva, _ = dd(10)
    off = struct.unpack_from('<I', img, lrva + 0xE0)[0]   # DynamicValueRelocTableOffset
    sec = struct.unpack_from('<H', img, lrva + 0xE4)[0]   # DynamicValueRelocTableSection (1-based)
    base = secs[sec - 1][0] + off
    ver, size = struct.unpack_from('<II', img, base)
    p = base + 8; end = p + size; n = 0
    while p < end:
        sym, brsize = struct.unpack_from('<QI', img, p); p += 12
        q = p; p += brsize
        if sym != 6: continue
        while q < p:
            va, bsz = struct.unpack_from('<II', img, q); r = q + 8; q += bsz
            while r < q:
                e = struct.unpack_from('<H', img, r)[0]; r += 2
                if e == 0 and r >= q: break
                o, t, m = e & 0xfff, (e >> 12) & 3, e >> 14
                a = va + o
                if t == 0:
                    img[a:a + (1 << m)] = bytes(1 << m)
                elif t == 1:
                    sz = 1 << m; img[a:a+sz] = img[r:r+sz]; r += sz
                elif t == 2:
                    v = struct.unpack_from('<H', img, r)[0]; r += 2
                    v *= 8 if m & 2 else 4
                    if m & 1: v = -v
                    old = struct.unpack_from('<Q', img, a)[0]
                    struct.pack_into('<Q', img, a, (old + v) & 0xffffffffffffffff)
                n += 1
    return n

def u64(a): return struct.unpack_from('<Q', img, a)[0]
def sec_at(a):
    for s in secs:
        if s[0] <= a < s[1]: return s
EXEC, WRITE = 0x20000000, 0x80000000
if '--ec' in sys.argv:
    print("ARM64X relocations applied:", apply_arm64x())
    def dd(i): return struct.unpack_from('<II', img, dirs + 8 * i)
# exports
erva, esz = dd(0)
nfun, nnames, afun, aname, aord = struct.unpack_from('<IIIII', img, erva + 20)
exports = {}
for i in range(nnames):
    st = u32(aname + 4*i); nm = bytes(img[st:img.find(b'\0', st)]).decode()
    o = struct.unpack_from('<H', img, aord + 2*i)[0]
    exports[nm] = u32(afun + 4*o)
# CHPE redirection
lrva, lsz = dd(10)
chpe = u64(lrva + 0xC8) - imagebase  # CHPEMetadataPointer offset in IMAGE_LOAD_CONFIG_DIRECTORY64
md = struct.unpack_from('<18I', img, chpe)
redir_rva, redir_cnt = md[4], md[13]
lut = {}
for i in range(redir_cnt):
    s, d = struct.unpack_from('<II', img, redir_rva + 8*i)
    lut[s] = d
print(f"CHPE v{md[0]} codemap {md[2]} entries, redirection {redir_cnt} entries")
cm = []
for i in range(md[2]):
    st, ln = struct.unpack_from('<II', img, md[1] + 8*i)
    cm.append((st & ~3, ln, ['ARM64','ARM64EC','AMD64','?'][st & 3]))
print("code map:", [(hex(a), hex(l), t) for a, l, t in cm])
def native(name):
    e = exports.get(name)
    if e is None: return 0
    return lut.get(e, e)
# .pdata
prva, psz = dd(3)
print(f"exception directory {prva:#x} size {psz:#x}; CHPE ExtraRFETable {md[16]:#x} size {md[17]:#x}")
if '--rfe' in sys.argv and md[16]:
    prva, psz = md[16], md[17]
def function_at(a):
    for i in range(psz // 8):
        start, unw = struct.unpack_from('<II', img, prva + 8*i)
        if unw & 3:
            ln = ((unw >> 2) & 0x7ff) * 4
        else:
            s = sec_at(unw)
            if not s or unw + 4 > s[1]: continue
            ln = (u32(unw) & 0x3ffff) * 4
        if start <= a < start + ln: return start, ln
    return None
def sx(v, bits):
    sign = 1 << (bits - 1); return (v ^ sign) - sign
def formed(pc, end, window=8):
    I = u32(pc)
    imm21 = (((I >> 5) & 0x7ffff) << 2) | ((I >> 29) & 3)
    if (I & 0x9f000000) == 0x10000000: return (pc + sx(imm21, 21), True)
    if (I & 0x9f000000) != 0x90000000: return (0, True)
    page = (pc & ~0xfff) + (sx(imm21, 21) << 12)
    rd = I & 31
    n = pc + 4
    while n < end and n <= pc + 4 * window:
        J = u32(n)
        if (J & 0xff800000) == 0x91000000 and ((J >> 5) & 31) == rd:
            return (page + (((J >> 10) & 0xfff) << (12 if (J >> 22) & 1 else 0)), True)
        if (J & 0x3b000000) == 0x39000000 and ((J >> 5) & 31) == rd:
            return (page + (((J >> 10) & 0xfff) << (J >> 30)), True)
        n += 4
    return (page, False)
QSZ, QN = 24, 256
def queues_empty(base):
    if base & 7: return False
    s = sec_at(base)
    if not s or base + QSZ*QN > s[1] or (s[2] & EXEC) or not (s[2] & WRITE): return False
    for i in range(QN):
        q = base + i*QSZ
        nx, pv, lk = u64(q), u64(q+8), u32(q+16)
        if lk or not ((nx == 0 and pv == 0) or (nx == q and pv == q)): return False
    return True
def ordered(b, l):
    c = 0
    for a in range(b, b + l, 4):
        I = u32(a)
        if (I & 0x3ffffc00) in (0x08dffc00, 0x089ffc00, 0x38bfc000) or (I & 0x3f200c00) == 0x19000000: c += 1
    return c
for nm in ('wine_get_version', 'NtWaitForAlertByThreadId', 'NtAlertThreadByThreadId'):
    print(nm, 'export', hex(exports.get(nm, 0)))
T = []
for nm in ('RtlWaitOnAddress', 'RtlWakeAddressAll', 'RtlWakeAddressSingle'):
    code = native(nm)
    fa = function_at(code)
    print(f"{nm}: export {exports[nm]:#x} native {code:#x} function {fa and (hex(fa[0]), hex(fa[1]))} ordered {fa and ordered(*fa)}")
    if not fa or fa[0] != code or fa[1] < 16: print("  -> NOT a whole function: left alone"); sys.exit(1)
    T.append([nm, code, fa[1], 0])
def marks(b, l):
    # str xzr, [Xn, #0x10]: the woken mark (futex_entry.addr) cleared; fenced = a DMB right before it
    fenced = plain = 0
    for a in range(b, b + l, 4):
        I = u32(a)
        if (I & 0xfffffc1f) == 0xf900081f:
            if (u32(a - 4) & 0xfffff0ff) == 0xd50330bf: fenced += 1
            else: plain += 1
    return fenced, plain
for t in T: print(f"  {t[0]} mark clears (fenced, plain): {marks(t[1], t[2])}")
fa, pa = marks(T[1][1], T[1][2]); fs, ps = marks(T[2][1], T[2][2])
if fa or fs or sum(ordered(b, l) for _, b, l, _ in T):
    print("RESULT: already has the fix, left alone"); sys.exit(0)
if not pa or not ps:
    print("RESULT: mark not cleared as 11.0 does, left alone"); sys.exit(0)
h = 0xcbf29ce484222325
for nm, b, l, _ in T:
    for x in img[b:b+l]: h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
print(f"code hash {h:016x}, ordered total {sum(ordered(b, l) for _, b, l, _ in T)}")
def formed_set(b, l):
    out = set()
    for pc in range(b, b + l, 4):
        fm, ok = formed(pc, b + l)
        if fm and ok: out.add(fm)
    return out
common = formed_set(T[0][1], T[0][2]) & formed_set(T[1][1], T[1][2]) & formed_set(T[2][1], T[2][2])
print("formed in all three:", sorted(hex(x) for x in common))
found = [x for x in sorted(common) if queues_empty(x)]
print("list candidates:", [hex(x) for x in found])
if len(found) != 1: sys.exit(1)
lists = found[0]
outside = []
for s in secs:
    if not (s[2] & EXEC): continue
    for pc in range(s[0], s[1] & ~3, 4):
        fm, ok = formed(pc, s[1] & ~3)
        if fm and not ok and fm + 0xfff >= lists and fm < lists + QSZ*QN:
            fm2, ok2 = formed(pc, s[1] & ~3, 256)
            print(f"  page only at {pc:#x} ({fm:#x}); with a longer look {fm2:#x}{'' if ok2 else ' (still page only)'}")
            fm = fm2 if ok2 else max(fm2, lists)
        if not (lists <= fm < lists + QSZ*QN): continue
        inside = False
        for t in T:
            if t[1] <= pc < t[1] + t[2]: t[3] += 1; inside = True
        if not inside: outside.append((pc, s[3]))
print("uses:", [(t[0], t[3]) for t in T], "outside:", [(hex(a), n) for a, n in outside[:10]], len(outside))
print("RESULT:", "in use" if not outside and all(t[3] for t in T) else "left alone")
