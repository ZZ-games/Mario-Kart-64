#!/usr/bin/env python3
"""Align swept function starts with source-order names using func_XXXXXXXX anchors."""
import json, os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
import symmap as S
ROOT = S.ROOT
w = json.load(open(os.path.join(ROOT, 'build/symwork.json')))
TEXT_END = {'main': int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x800D8F70, 'racing': 0x802BA370, 'ending': 0x80287500}
sym, bad, total_ok = {}, [], 0
for seg in ('main', 'racing', 'ending'):
    v, a, b = S.SEGS[seg]
    end = min(TEXT_END[seg], v + (b - a))
    starts = S.sweep(seg, end)
    names = [n for o, n in w[seg]['names'] if not o.startswith('data/')]
    # anchors as (name index, address); add virtual anchors at both ends
    voted = json.load(open(os.path.join(ROOT, 'build/voted.json'))) if os.path.exists(os.path.join(ROOT, 'build/voted.json')) else {}
    anchors = [(-1, v - 4)] + [(i, int(n[5:], 16)) if re.fullmatch(r'func_8[0-9A-Fa-f]{7}', n) else (i, voted[n])
               for i, n in enumerate(names) if re.fullmatch(r'func_8[0-9A-Fa-f]{7}', n) or (n in voted and S.seg_of(voted[n]) == seg)]
    # keep only anchors increasing in both index and address
    keep = [anchors[0]]
    for an in anchors[1:]:
        if an[1] > keep[-1][1] and an[0] > keep[-1][0]: keep.append(an)
    anchors = keep
    anchors.append((len(names), end))
    for (i1, a1), (i2, a2) in zip(anchors, anchors[1:]):
        if i1 >= 0: sym[names[i1]] = a1
        inner = [s for s in starts if a1 < s < a2]
        want = names[i1 + 1:i2]
        if len(inner) == len(want):
            for n, s in zip(want, inner): sym[n] = s
            total_ok += len(want)
        else:
            bad.append((seg, hex(a1), hex(a2), len(inner), len(want), want[:6]))
    print(seg, 'sweep starts', len(starts), 'names', len(names))
print('assigned', len(sym), 'mismatched gaps', len(bad))
for b in bad[:40]: print(' ', b)
json.dump({k: v for k, v in sorted(sym.items(), key=lambda x: x[1])}, open(os.path.join(ROOT, 'build/symbols.json'), 'w'), indent=0)
