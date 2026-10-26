import json, os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
import symmap as S
sym = json.load(open('build/symbols2.json')); a2n = {v: k for k, v in sym.items()}
st = sorted(sym.values())
for name in sys.argv[1:]:
    a = sym[name]; e = st[st.index(a) + 1]
    pc, js = a, []
    while pc < e:
        x = S.word(pc)
        if x >> 26 == 3:
            t = (pc & 0xF0000000) | ((x & 0x3FFFFFF) << 2); js.append(a2n.get(t, hex(t)))
        pc += 4
    print(name, hex(a), '->', js)
