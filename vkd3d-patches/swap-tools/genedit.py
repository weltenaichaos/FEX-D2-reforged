"""Edit script from a game's SPIR-V (vkd3d's translation) to FEX's reordered version of it, so the game's code itself
is never stored: the script only says which runs of the original words to copy, in which order, plus FEX's own new
words. Usage: genedit.py ORIG.spv NEW.spv ARRAY OUT.h 'comment'. Prints the counts and both FNV-1a 64 hashes."""
import struct, sys
orig_p, new_p, arr, out, comment = sys.argv[1:6]
def words(p):
    b = open(p, 'rb').read(); return list(struct.unpack('<%dI' % (len(b) // 4), b))
def insns(w):
    r, i = [], 5
    while i < len(w):
        n = w[i] >> 16; assert n; r.append((i, n)); i += n
    assert i == len(w); return r
def fnv(w):
    h = 0xcbf29ce484222325
    for x in struct.pack('<%dI' % len(w), *w): h = ((h * 0x100000001b3) & 0xffffffffffffffff) ^ x
    return h
o, n = words(orig_p), words(new_p)
n[2] = o[2]  # keep the original generator word
where = {}
for i, c in insns(o): where.setdefault(tuple(o[i:i + c]), []).append(i)
ops, copied, inserted, nxt = [], 0, 0, None
ops.append(['ins', n[0:5]]); inserted += 5
for i, c in insns(n):
    key = tuple(n[i:i + c]); cand = where.get(key)
    if cand:
        src = nxt if nxt in cand else cand[0]
        if ops[-1][0] == 'copy' and ops[-1][1] + ops[-1][2] == src: ops[-1][2] += c
        else: ops.append(['copy', src, c])
        nxt = src + c; copied += c
    else:
        if ops[-1][0] == 'ins': ops[-1][1] += key
        else: ops.append(['ins', list(key)])
        inserted += c; nxt = None
# replay to check
res = []
for op in ops:
    if op[0] == 'copy': res += o[op[1]:op[1] + op[2]]
    else: res += op[1]
assert res == n
script = []
for op in ops:
    if op[0] == 'copy': script += [0xC0000000 | op[2], op[1]]
    else: script += [0x80000000 | len(op[1])] + list(op[1])
lines = ['    ' + ', '.join('0x%08x' % x for x in script[i:i + 8]) + ',' for i in range(0, len(script), 8)]
open(out, 'a').write('\n/* %s\n * Edit script: 0xC0000000 | n, start = copy n words of vkd3d\'s translation from word start;\n'
    ' * 0x80000000 | n, words = n new words. %d words copied, %d new (header and the new early tests).\n'
    ' * Translation %016x (%d words) -> result %016x (%d words). */\nstatic const uint32_t %s[] =\n{\n%s\n};\n' % (
    comment, copied, inserted, fnv(o), len(o), fnv(n), len(n), arr, '\n'.join(lines)))
print('ops', len(ops), 'copied', copied, 'inserted', inserted, 'script words', len(script),
      'orig %016x new %016x' % (fnv(o), fnv(n)))
for op in ops:
    if op[0] == 'ins': print('  insert', ' '.join('%08x' % x for x in op[1]))
