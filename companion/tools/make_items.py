#!/usr/bin/env python3
"""Builds app/src/main/assets/items.tsv and words.txt from blizzhackers/d2data (MIT).

Usage: make_items.py <d2data json dir> <out.tsv> [<words.txt>]
Lines: kind<TAB>display name<TAB>base name   (kind = base, unique, set)
"""
import json, sys, os

src, out = sys.argv[1], sys.argv[2]
def load(name):
    d = json.load(open(os.path.join(src, name + '.json')))
    return list(d.values()) if isinstance(d, dict) else d
s = json.load(open(os.path.join(src, 'allstrings-eng.json')))
def text(key, fallback=None):
    v = s.get(key, fallback if fallback is not None else key)
    return v.split('\n')[-1].strip() if v else v

rows = set()
code_name = {}
for f in ('weapons', 'armor', 'misc'):
    for it in load(f):
        if not it.get('code') or not it.get('name'):
            continue
        name = text(it.get('namestr') or it['code'], it['name'])
        if not name or name.startswith('Not used') or name.lower() == 'unused':
            continue
        code_name[it['code']] = name
        rows.add(('base', name, name))
for kind, f, base_key in (('unique', 'uniqueitems', 'code'), ('set', 'setitems', 'item')):
    for it in load(f):
        idx = it.get('index')
        if not idx or not it.get(base_key):
            continue
        base = code_name.get(it[base_key], it.get('*ItemName', ''))
        rows.add((kind, text(idx), base))
with open(out, 'w') as o:
    o.write('# from github.com/blizzhackers/d2data (MIT license), made by companion/tools/make_items.py\n')
    for r in sorted(rows):
        o.write('\t'.join(r) + '\n')
print(len(rows), 'rows')

# Words that appear in item text (properties, names, skills), most frequent first.
if len(sys.argv) > 3:
    import re, collections
    cnt = collections.Counter()
    prefixes = ('ModStr', 'ItemStats', 'StrSkill', 'skillname', 'Skillname', 'strItemMod', 'Item')
    texts = [v for k, v in s.items() if isinstance(v, str) and k.startswith(prefixes)]
    texts += [r[1] for r in rows]
    for t in texts:
        for w in re.findall(r"[A-Za-z][A-Za-z']*", t.replace('%d', ' ').replace('%s', ' ')):
            cnt[w.lower()] += 1
    with open(sys.argv[3], 'w') as o:
        o.write('# from github.com/blizzhackers/d2data (MIT license), made by companion/tools/make_items.py\n')
        for w, n in cnt.most_common():
            if len(w) < 2:
                continue
            o.write(w + '\n')
    print(len(cnt), 'words')
