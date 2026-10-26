"""Align the libultra region using verified checkpoints; writes build/os_symbols.json."""
import json, os, sys, re
sys.path.insert(0, os.path.dirname(__file__))
import symmap as S
w = json.load(open('build/symwork.json'))
names = [n for o, n in w['main']['names'] if not o.startswith('data/')]
KNOWN = {'osCreateThread': 0x800cbf70, 'osInitialize': 0x800cc0c0, 'osStartThread': 0x800cc360, 'osCreateViManager': 0x800cc4b0,
         'osViSetMode': 0x800cc850, 'osViBlack': 0x800cc8c0, 'osViSetSpecialFeatures': 0x800cc930, 'osCreatePiManager': 0x800ccaf0,
         'osSetThreadPri': 0x800ccc80, 'osCreateMesgQueue': 0x800ccd60, 'osViSetEvent': 0x800ccd90, 'osSetEventMesg': 0x800cce00,
         'osSpTaskLoad': 0x800ccf8c, 'osSpTaskStartGo': 0x800cd0ec, 'osContInit': 0x800cd130, 'osContStartReadData': 0x800cd4f0,
         'osContGetReadData': 0x800cd5b4, 'osRecvMesg': 0x800cd750, 'osWritebackDCacheAll': 0x800cd890, 'osSendMesg': 0x800cd8c0,
         'osViSwapBuffer': 0x800cda10, 'osInvalICache': 0x800cdb00, 'osInvalDCache': 0x800cdb80, 'osPiStartDma': 0x800cdc30,
         'osAiSetFrequency': 0x800d0f80, 'osAiGetLength': 0x800d10e0, 'osAiSetNextBuffer': 0x800d10f0, 'osGetCount': 0x800d11a0,
         'osEepromLongWrite': 0x800d0050, 'osEepromLongRead': 0x800d0190, 'osSetTime': 0x800ce310, 'osPfsInit': 0x800ce720}
starts = [s for s in S.sweep('main', 0x800d8f70) if s >= 0x800cbf70]
i0 = names.index('osCreateThread')
region = names[i0:]
anch = sorted((region.index(n), a) for n, a in KNOWN.items())
out = {}
for (i1, a1), (i2, a2) in zip(anch, anch[1:] + [(len(region), 0x800d8f70)]):
    inner = [s for s in starts if a1 <= s < a2]
    want = region[i1:i2]
    tag = 'OK ' if len(inner) == len(want) else 'MISMATCH'
    if len(inner) == len(want):
        out.update(zip(want, inner))
    else:
        out[want[0]] = a1
    print(tag, hex(a1), len(inner), len(want), want[:8], [hex(x) for x in inner[:8]])
json.dump(out, open('build/os_symbols.json', 'w'), indent=0)
