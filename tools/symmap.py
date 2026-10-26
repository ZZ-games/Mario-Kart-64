#!/usr/bin/env python3
"""Recover a function symbol map for Mario Kart 64 (US) without building the decomp.

1. Recursive-descent disassembly of the ROM's code segments finds every function
   start reachable by calls (jal) or by address constants built in code (lui+addiu).
2. The decomp's linker script gives the order of source files; parsing those files
   gives the order of function definitions.
3. The two ordered lists are aligned per segment. Functions named func_XXXXXXXX
   carry their own address and serve as checkpoints.

Output: build/symbols.json {name: addr}, build/functions.json [addr...]
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEC = os.path.join(ROOT, 'decomp')
rom = open(os.path.join(ROOT, 'rom/baserom.us.z64'), 'rb').read()

# segment: (vram, rom_start, rom_end)
SEGS = {'main': (0x80000400, 0x1000, 0xF7510), 'racing': (0x8028DF00, 0xF7510, 0x123640),
        'ending': (0x80280000, 0x123640, 0x12AAE0)}


def seg_of(addr):
    for n, (v, a, b) in SEGS.items():
        if v <= addr < v + (b - a):
            return n
    return None


def word(addr):
    v, a, b = SEGS[seg_of(addr)]
    o = a + addr - v
    return int.from_bytes(rom[o:o + 4], 'big')


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


# ---------- recursive descent ----------
def descend(entries):
    code, funcs, todo = set(), set(entries), list(entries)
    lui_consts = set()
    while todo:
        pc = todo.pop()
        regs = {}
        while seg_of(pc) and pc not in code:
            code.add(pc)
            w = word(pc)
            op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            imm = w & 0xFFFF
            ends = False
            if op == 2 or op == 3:  # j / jal
                tgt = (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if seg_of(tgt):
                    if op == 3 and tgt not in funcs:
                        funcs.add(tgt); todo.append(tgt)
                    elif op == 2:
                        todo.append(tgt)  # tail jump
                if op == 2:
                    ends = True
            elif op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and rt in (0, 1, 2, 3)) or (op == 17 and rs == 8):
                tgt = (pc + 4 + (s16(imm) << 2)) & 0xFFFFFFFF
                todo.append(tgt)
                if op == 4 and rs == 0 and rt == 0:  # b (beq zero,zero)
                    ends = True
            elif op == 0 and (w & 0x3F) == 8:  # jr
                ends = True
            elif op == 0x0F:  # lui
                regs[rt] = imm << 16
            elif op in (0x09, 0x0D) and rs in regs:  # addiu / ori
                v = (regs[rs] + (s16(imm) if op == 9 else imm)) & 0xFFFFFFFF
                lui_consts.add(v)
            if ends:
                code.add(pc + 4)  # delay slot
                break
            pc += 4
    return code, funcs, lui_consts


def discover():
    entries = [0x80000400]
    code, funcs, consts = descend(entries)
    for _ in range(10):
        new = {c for c in consts if seg_of(c) and c % 4 == 0 and c not in funcs and is_func_start(c, code)}
        # also data words pointing at code (function pointer tables)
        if not new:
            break
        code, funcs, consts = descend(sorted(funcs | new))
    return code, funcs


def is_func_start(addr, code):
    # plausible function start: previous instruction is a delay slot after jr ra / or a nop pad, and
    # first instruction is typical prologue or anything not already inside known code
    if addr in code:
        return False
    w = word(addr)
    return w != 0 and (w >> 16) == 0x27BD or True


# ---------- source order ----------
def text_objects():
    lines = open(os.path.join(DEC, 'mk64.ld')).read().splitlines()
    order = {'main': [], 'racing': [], 'ending': []}
    cur = None
    for ln in lines:
        m = re.search(r'BEGIN_SEG\((main|racing|ending),', ln)
        if m:
            cur = m.group(1); continue
        if re.search(r'END_SEG\(', ln):
            cur = None; continue
        m = re.search(r'BUILD_DIR/(\S+?)\.o\(\.text\*?\)', ln)
        if m and cur:
            order[cur].append(m.group(1))
    return order


C_DEF = re.compile(r'^(?!static\s+inline)(?:[A-Za-z_][\w\s\*]*?[\s\*])([A-Za-z_]\w*)\s*\(([^;]*?)\)\s*\{?\s*$')


def funcs_in_source(obj):
    base = obj.replace('.jp', '')
    for ext in ('.c', '.s'):
        p = os.path.join(DEC, base + ext)
        if os.path.exists(p):
            break
    else:
        return None
    txt = open(p, encoding='utf-8', errors='replace').read()
    def inl(m):
        inc = m.group(1)
        for base_dir in (os.path.dirname(p), DEC, os.path.join(DEC, 'src')):
            q = os.path.join(base_dir, inc)
            if os.path.exists(q):
                return open(q, encoding='utf-8', errors='replace').read()
        return ''
    if p.endswith('.c'):
        for _ in range(3):
            txt = re.sub(r'^#include\s+"([^"]+\.c)"', inl, txt, flags=re.M)
    out = []
    if p.endswith('.s'):
        for ln in txt.splitlines():
            m = re.match(r'\s*(?:glabel|.global\s+)\s*(\w+)', ln)
            if m and m.group(1) not in out:
                out.append(m.group(1))
        return out
    # C: track preprocessor for VERSION_US
    stack, depth = [], 0
    lines = txt.splitlines()
    i = 0
    while i < len(lines):
        ln = lines[i]
        s = ln.strip()
        if s.startswith('#'):
            d = s[1:].strip()
            if d.startswith('ifdef') or d.startswith('ifndef') or d.startswith('if '):
                cond = d
                act = evalcond(cond)
                stack.append([act, act])
            elif d.startswith('elif'):
                top = stack[-1]
                act = (not top[1]) and evalcond('if ' + d[4:])
                top[0] = act; top[1] = top[1] or act
            elif d.startswith('else') and stack:
                stack[-1][0] = not stack[-1][1]
            elif d.startswith('endif') and stack:
                stack.pop()
            i += 1; continue
        if stack and not all(a for a, _ in stack):
            i += 1; continue
        if depth == 0 and ln and not ln[0].isspace() and '(' in ln and not s.startswith('//') and not s.endswith(';'):
            # gather signature possibly over multiple lines
            sig = ln; j = i
            while ')' not in sig.split('(', 1)[1] if '(' in sig else False:
                j += 1; sig += ' ' + lines[j].strip()
            rest = ''.join(lines[j + 1:j + 2]).strip()
            m = re.match(r'^[\w\s\*]*?\b([A-Za-z_]\w*)\s*\(', sig)
            if m and ('{' in sig or rest.startswith('{')) and not re.match(r'^\s*(if|while|for|switch|return)\b', sig) \
                    and '=' not in sig.split('(')[0] and not sig.lstrip().startswith('static inline'):
                name = m.group(1)
                if name not in ('GLOBAL_ASM',):
                    out.append(name)
        if s.startswith('GLOBAL_ASM') or s.startswith('#pragma GLOBAL_ASM'):
            pass
        depth += ln.count('{') - ln.count('}')
        i += 1
    return out


def evalcond(c):
    c = c.strip()
    if c.startswith('ifdef'):
        return c.split()[1] in DEFS
    if c.startswith('ifndef'):
        return c.split()[1] not in DEFS
    e = c[2:].strip()
    e = re.sub(r'defined\s*\(?\s*(\w+)\s*\)?', lambda m: '1' if m.group(1) in DEFS else '0', e)
    e = re.sub(r'\b([A-Za-z_]\w*)\b', lambda m: str(DEFS.get(m.group(1), 0)), e)
    e = e.replace('&&', ' and ').replace('||', ' or ').replace('!', ' not ').replace(' not =', '!=')
    try:
        return bool(eval(e))
    except Exception:
        return False


DEFS = {'VERSION_US': 1, 'F3DEX_GBI': 1, 'F3D_OLD': 1, '_LANGUAGE_C': 1, 'NON_MATCHING': 0}


def main():
    code, funcs = discover()
    order = text_objects()
    result, report = {}, []
    for seg, objs in order.items():
        v, a, b = SEGS[seg]
        found = sorted(f for f in funcs if seg_of(f) == seg)
        names = []
        for o in objs:
            fs = funcs_in_source(o)
            if fs is None:
                report.append(f'missing source {o}')
                fs = []
            names += [(o, n) for n in fs]
        report.append(f'{seg}: discovered {len(found)} function starts, source lists {len(names)} functions')
        result[seg] = (found, names)
    json.dump({s: {'found': f, 'names': n} for s, (f, n) in result.items()}, open(os.path.join(ROOT, 'build/symwork.json'), 'w'))
    print('\n'.join(report))
    print('code instructions', len(code))


if __name__ == '__main__':
    os.makedirs(os.path.join(ROOT, 'build'), exist_ok=True)
    main()


def sweep(seg, end_vram):
    """Linear sweep for function starts (IDO layout: functions back to back)."""
    v, a, b = SEGS[seg]
    starts, pc = [], v
    while pc < end_vram:
        while pc < end_vram and word(pc) == 0:
            pc += 4
        if pc >= end_vram:
            break
        starts.append(pc)
        maxb = pc
        while pc < end_vram:
            w = word(pc); op = w >> 26; rt = (w >> 16) & 31; rs = (w >> 21) & 31
            if op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and rt in (0, 1, 2, 3, 16, 17, 18, 19)) or (op == 17 and rs == 8):
                t = (pc + 4 + (s16(w & 0xFFFF) << 2)) & 0xFFFFFFFF
                maxb = max(maxb, t)
            if op == 2:  # j within function (switch or loop)
                t = (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if t > pc and t < pc + 0x4000: maxb = max(maxb, t)
            if op == 0 and (w & 0x3F) == 8 and rs == 31 and pc + 8 > maxb:
                pc += 8
                break
            uncond_back = (op == 4 and rs == 0 and rt == 0 and (pc + 4 + (s16(w & 0xFFFF) << 2)) <= pc) or \
                (op == 2 and not (pc < ((pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)) < pc + 0x4000))
            if uncond_back and pc + 8 > maxb:
                pc += 8
                break
            pc += 4
    return starts
