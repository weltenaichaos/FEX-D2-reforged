import struct, sys
# Prints the 32-bit value of a data export of a PE file (ARM64X too: the data is shared).
#   pe-export-u32.py <dll> <export name>     (run with python3 -I)
d = open(sys.argv[1], 'rb').read()
want = sys.argv[2].encode()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec, = struct.unpack_from('<H', d, pe + 6)
optsz, = struct.unpack_from('<H', d, pe + 20)
opt = pe + 24
magic, = struct.unpack_from('<H', d, opt)
dd = opt + (96 if magic == 0x10b else 112)
exp_rva, exp_size = struct.unpack_from('<II', d, dd)
secs = []
for i in range(nsec):
    s = opt + optsz + 40 * i
    vsz, va, rsz, raw = struct.unpack_from('<IIII', d, s + 8)
    secs.append((va, max(vsz, rsz), raw, rsz))
def off(rva):
    for va, sz, raw, rsz in secs:
        if va <= rva < va + sz:
            if rva - va >= rsz: return None  # in .bss part
            return raw + rva - va
    return None
e = off(exp_rva)
nfun, nnames, afun, anames, aords = struct.unpack_from('<IIIII', d, e + 20)
for i in range(nnames):
    nrva, = struct.unpack_from('<I', d, off(anames) + 4 * i)
    o = off(nrva)
    name = d[o:d.index(b'\0', o)]
    if name == want:
        ordi, = struct.unpack_from('<H', d, off(aords) + 2 * i)
        frva, = struct.unpack_from('<I', d, off(afun) + 4 * ordi)
        fo = off(frva)
        if fo is None: sys.exit('in bss')
        print(struct.unpack_from('<I', d, fo)[0]); sys.exit(0)
sys.exit('not exported')
