#!/usr/bin/env python3
"""Resolve callee addresses by matching source call order to jal order in known functions."""
import json, os, re, sys, collections
sys.path.insert(0, os.path.dirname(__file__))
import symmap as S
ROOT = S.ROOT
sym = json.load(open(os.path.join(ROOT, 'build/symbols.json')))
addr2name = {v: k for k, v in sym.items()}
w = json.load(open(os.path.join(ROOT, 'build/symwork.json')))
starts_sorted = sorted(sym.values())
KW = {'if', 'while', 'for', 'switch', 'return', 'sizeof', 'case', 'else', 'do'}

# source bodies
bodies = {}
for seg in ('main', 'racing', 'ending'):
    for obj in dict.fromkeys(o for o, n in w[seg]['names']):
        base = obj.replace('.jp', '')
        p = os.path.join(S.DEC, base + '.c')
        if not os.path.exists(p): continue
        txt = open(p, encoding='utf-8', errors='replace').read()
        txt = re.sub(r'//[^\n]*|/\*.*?\*/', '', txt, flags=re.S)
        for m in re.finditer(r'^[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;{]*?\)\s*\{', txt, re.M):
            name, i, d = m.group(1), m.end(), 1
            while d and i < len(txt):
                d += {'{': 1, '}': -1}.get(txt[i], 0); i += 1
            bodies.setdefault(name, txt[m.end():i])

def jals(start, end):
    out, pc = [], start
    while pc < end:
        x = S.word(pc)
        if x >> 26 == 3: out.append((pc & 0xF0000000) | ((x & 0x3FFFFFF) << 2))
        pc += 4
    return out

votes = collections.defaultdict(collections.Counter)
for name, a in sym.items():
    if name not in bodies: continue
    i = starts_sorted.index(a)
    end = starts_sorted[i + 1] if i + 1 < len(starts_sorted) else a + 0x400
    if not S.seg_of(end - 4) == S.seg_of(a): continue
    calls = [c for c in re.findall(r'\b([A-Za-z_]\w*)\s*\(', bodies[name]) if c not in KW and not c.isupper() and not c.startswith('g') or c.startswith('gu') or c.startswith('gsp')]
    calls = [c for c in calls if c not in KW]
    js = jals(a, end)
    if len(js) == len(calls):
        for c, t in zip(calls, js): votes[c][t] += 1
resolved = {}
for c, cnt in votes.items():
    t, n = cnt.most_common(1)[0]
    if n >= 1 and n >= 0.6 * sum(cnt.values()): resolved[c] = t
conflicts = [(c, hex(sym[c]), hex(t)) for c, t in resolved.items() if c in sym and sym[c] != t]
new = {c: t for c, t in resolved.items() if c not in sym}
print('resolved', len(resolved), 'new', len(new), 'conflicts with alignment', len(conflicts))
for x in conflicts[:15]: print('  conflict', x)
for k in ['osCreateThread','osStartThread','osRecvMesg','osSendMesg','osCreateMesgQueue','osPiStartDma','osViSwapBuffer','osSpTaskStartGo','osSpTaskLoad','osContStartReadData','osContGetReadData','osContInit','osSetEventMesg','osViSetEvent','osWritebackDCache','osInvalDCache','osGetTime','osAiSetNextBuffer','osAiSetFrequency','osAiGetLength','osEepromProbe','osEepromLongRead','osEepromLongWrite','osViSetMode','osViBlack','osCreateViManager','osCreatePiManager','osInitialize','osSetThreadPri','osGetCount','osJamMesg','osViSetSpecialFeatures','osWritebackDCacheAll','osInvalICache','osSetIntMask','osDpSetStatus','osViGetCurrentFramebuffer','osViGetNextFramebuffer','osEPiStartDma','osCartRomInit','osPfsInit','osContSetCh','osSetTime']:
    print(f'  {k:28s}', hex(resolved[k]) if k in resolved else '-', hex(sym[k]) if k in sym else '')
strong = {c: t for c, t in resolved.items() if sum(votes[c].values()) >= 2 and votes[c][t] == sum(votes[c].values())}
json.dump(strong, open(os.path.join(ROOT, 'build/voted.json'), 'w'))
merged = dict(sym); merged.update(new)
json.dump(dict(sorted(merged.items(), key=lambda x: x[1])), open(os.path.join(ROOT, 'build/symbols2.json'), 'w'), indent=0)
