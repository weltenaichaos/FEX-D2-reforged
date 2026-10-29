"""Reorder D2R's clutter compute shader (0e21a3b5088ba1af) so the on-screen test and the mask-texture test return
early, before the two density noise layers. Input: raw-id spirv-dis text. Output: spvasm text.
Usage: clutpatch.py IN.spvasm OUT.spvasm BLOCK F T"""
import re, sys
src, dst, block, F, T = sys.argv[1:6]
lines = open(src).read().split('\n')
start = next(i for i, l in enumerate(lines) if l.strip() == f'{block} = OpLabel')
end = next(i for i in range(start + 1, len(lines)) if lines[i].strip().startswith('OpSelectionMerge'))
body = lines[start + 1:end]
assert all('OpLabel' not in l and 'OpBranch' not in l and 'OpReturn' not in l for l in body)
SIDE = ('OpStore', 'OpAtomic', 'OpImageWrite', 'OpControlBarrier', 'OpMemoryBarrier', 'OpEmitVertex', 'OpKill',
        'OpCopyMemory', 'OpTerminate', 'OpDemote')
defs = {}
for k, l in enumerate(body):
    m = re.match(r'\s*(%\d+) = (Op\w+)(.*)', l)
    assert m, l
    assert not m[2].startswith(SIDE), l
    defs[m[1]] = (k, m[2], re.findall(r'%\d+', m[3]))
def cone(x):
    out, todo = set(), [x]
    while todo:
        y = todo.pop()
        if y in out or y not in defs: continue
        out.add(y); todo += defs[y][2]
    return out
# functions called in the block must be side-effect free: check their bodies
calls = {defs[d][2][1] for d in defs if defs[d][1] == 'OpFunctionCall'}
for fn in calls:
    fs = next(i for i, l in enumerate(lines) if l.strip().startswith(f'{fn} = OpFunction '))
    fe = next(i for i in range(fs, len(lines)) if lines[i].strip() == 'OpFunctionEnd')
    for l in lines[fs:fe]:
        assert not any(s in l for s in SIDE + ('OpFunctionCall',)), (fn, l)
cf = cone(F); ct = cone(T) - cf
first = [l for l in body if re.match(r'\s*(%\d+)', l)[1] in cf]
second = [l for l in body if re.match(r'\s*(%\d+)', l)[1] in ct]
rest = [l for l in body if re.match(r'\s*(%\d+)', l)[1] not in cf | ct]
boolty = re.search(r'= OpLogicalAnd (%\d+)', '\n'.join(body))[1]
def early(cond, tag):
    return [f'     %fex_not_{tag} = OpLogicalNot {boolty} {cond}',
            f'               OpSelectionMerge %fex_go_{tag} None',
            f'               OpBranchConditional %fex_not_{tag} %fex_out_{tag} %fex_go_{tag}',
            f'     %fex_out_{tag} = OpLabel', '               OpReturn', f'     %fex_go_{tag} = OpLabel']
new = lines[:start + 1] + first + early(F, 'f') + second + early(T, 't') + rest + lines[end:]
open(dst, 'w').write('\n'.join(new))
print(f'block {block}: {len(body)} instructions -> on-screen test {len(first)}, mask texture test {len(second)}, '
      f'rest {len(rest)}; functions called {sorted(calls)}')
