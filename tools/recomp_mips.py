#!/usr/bin/env python3
"""Static recompiler: Mario Kart 64 (US) MIPS R4300i machine code -> C.

Each function in the ROM becomes one C function `void F_xxxxxxxx(Ctx* c)`.
Branches become gotos, calls become direct C calls, `jr ra` becomes return,
jump tables become switches over their known targets, and indirect calls go
through a lookup table. N64 OS functions that touch hardware or switch threads
are redirected to the runtime's replacements (HLE).

Memory is the N64's 8 MB RDRAM kept word-swapped, so 32-bit accesses are native
and byte/halfword accesses flip the low address bits (the same trick N64Recomp uses).
"""
import json, os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import symmap as S

ROOT = S.ROOT
OUT = os.path.join(ROOT, 'build/gen')
TEXT = {'main': (0x80000400, 0x800D8F70), 'racing': (0x8028DF00, 0x802B8790), 'ending': (0x80280000, 0x80284E40)}

HLE = json.load(open(os.path.join(ROOT, 'runtime/hle_list.json')))  # name -> addr (hex str)
HLE_BY_ADDR = {int(v, 16): k for k, v in HLE.items()}


def in_text(a):
    return any(s <= a < e for s, e in TEXT.values())


def s16(x): return x - 0x10000 if x & 0x8000 else x


# ---------------------------------------------------------------- function starts
def function_starts():
    starts = set()
    for seg, (s, e) in TEXT.items():
        starts.update(x for x in S.sweep(seg, e) if s <= x < e)
    # jal targets
    for seg, (s, e) in TEXT.items():
        for pc in range(s, e, 4):
            w = S.word(pc)
            if w >> 26 == 3:
                t = (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if in_text(t): starts.add(t)
    return sorted(starts)


def data_code_refs():
    """32-bit words in non-text parts of the segments that point into code (jump tables, fn ptrs)."""
    refs = set()
    for seg, (v, a, b) in S.SEGS.items():
        ts, te = TEXT[seg]
        for pc in range(v, v + (b - a), 4):
            if ts <= pc < te: continue
            x = S.word(pc)
            if in_text(x) and x % 4 == 0: refs.add(x)
    # address constants built in code (lui + addiu/ori) also count
    for seg, (s, e) in TEXT.items():
        hi = {}
        for pc in range(s, e, 4):
            w = S.word(pc); op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31
            if op == 0x0F: hi[rt] = (w & 0xFFFF) << 16
            elif op in (9, 0xD) and rs in hi:
                x = (hi[rs] + (s16(w & 0xFFFF) if op == 9 else (w & 0xFFFF))) & 0xFFFFFFFF
                if in_text(x): refs.add(x)
    return refs


# ---------------------------------------------------------------- C emission helpers
R = lambda n: '0' if n == 0 else f'c->r[{n}]'
RW = lambda n: '0' if n == 0 else f'(int32_t)c->r[{n}]'
RU = lambda n: '0' if n == 0 else f'(uint32_t)c->r[{n}]'


def setr(n, expr32):
    return '' if n == 0 else f'c->r[{n}] = (int64_t)(int32_t)({expr32});'


def setr64(n, expr):
    return '' if n == 0 else f'c->r[{n}] = (int64_t)({expr});'


def addr(rs, imm):
    return f'((uint32_t)c->r[{rs}] + {s16(imm)})' if rs else f'(uint32_t){s16(imm) & 0xFFFFFFFF}u'


class Fn:
    def __init__(self, start, end, starts_set, labels_from_data):
        self.start, self.end = start, end
        self.starts = starts_set
        self.data_labels = sorted(a for a in labels_from_data if start <= a < end)
        self.labels = set(self.data_labels)
        self.warn = []

    def emit(self):
        a, end = self.start, self.end
        # first pass: branch targets
        for pc in range(a, end, 4):
            t = self.branch_target(pc)
            if t is not None and a <= t < end and S.word(pc) >> 26 != 3: self.labels.add(t)
        body = []
        pc = a
        while pc < end:
            if pc in self.labels: body.append(f'L_{pc:08X}:;')
            w = S.word(pc)
            info = self.ctrl(pc, w)
            if info:
                ds = self.insn(pc + 4, S.word(pc + 4))
                body.append(info(ds))
                if pc + 4 in self.labels:  # something branches straight to the delay slot
                    body.append(f'if (0) {{ L_{pc+4:08X}:; {ds} }}')
                pc += 8
                continue
            body.append(self.insn(pc, w))
            pc += 4
        # falls off the end -> continue in next function
        if end in self.starts or in_text(end):
            body.append(f'F_{end:08X}(c); return;')
        name = f'F_{a:08X}'
        return f'void {name}(Ctx* c) {{\nTRACE(0x{a:08X}u);\n' + '\n'.join(x for x in body if x) + '\n}\n'

    def branch_target(self, pc):
        w = S.word(pc); op = w >> 26; rt = (w >> 16) & 31; rs = (w >> 21) & 31
        if op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and rt in (0, 1, 2, 3, 16, 17, 18, 19)) or (op == 17 and rs == 8):
            return (pc + 4 + (s16(w & 0xFFFF) << 2)) & 0xFFFFFFFF
        if op in (2, 3):
            return (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        return None

    def goto(self, t, pc=None):
        if self.start <= t < self.end:
            # loops: honor pending interrupts (thread preemption) on backward branches
            if pc is not None and t <= pc: return f'PREEMPT_CHECK(c); goto L_{t:08X};'
            return f'goto L_{t:08X};'
        if t in self.starts or t in HLE_BY_ADDR:
            return self.call(t) + ' return;'
        return f'rt_bad_jump(c, 0x{t:08X}u); return;'

    def call(self, t):
        if t in HLE_BY_ADDR: return f'H_{HLE_BY_ADDR[t]}(c);'
        if t in self.starts: return f'F_{t:08X}(c);'
        return f'rt_call(c, 0x{t:08X}u);'

    def ctrl(self, pc, w):
        """Return a function(delay_slot_code)->code for control-transfer instructions, else None."""
        op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; fn = w & 0x3F
        t = self.branch_target(pc)
        nxt = pc + 8
        if op == 0 and fn == 8:  # jr
            if rs == 31: return lambda ds: f'{ds} return;'
            if self.data_labels:
                cases = ' '.join(f'case 0x{x:08X}u: goto L_{x:08X};' for x in self.data_labels)
                return lambda ds: f'{{ uint32_t jt = {RU(rs)}; {ds} switch (jt) {{ {cases} default: rt_call(c, jt); return; }} }}'
            return lambda ds: f'{{ uint32_t jt = {RU(rs)}; {ds} rt_call(c, jt); return; }}'
        if op == 0 and fn == 9:  # jalr
            rd = (w >> 11) & 31
            return lambda ds: f'{{ uint32_t jt = {RU(rs)}; {setr(rd, nxt)} {ds} rt_call(c, jt); }}'
        if op == 2:  # j
            if t == pc and S.word(pc + 4) == 0:
                return lambda ds: 'rt_idle_forever(c); return;'
            return lambda ds: f'{ds} {self.goto(t, pc)}'
        if op == 3:  # jal
            return lambda ds: f'{setr(31, nxt)} {ds} {self.call(t)}'
        conds = {4: f'{R(rs)} == {R(rt)}', 5: f'{R(rs)} != {R(rt)}', 6: f'{R(rs)} <= 0', 7: f'{R(rs)} > 0',
                 20: f'{R(rs)} == {R(rt)}', 21: f'{R(rs)} != {R(rt)}', 22: f'{R(rs)} <= 0', 23: f'{R(rs)} > 0'}
        likely = op >= 20
        cond = conds.get(op)
        link = False
        if op == 1:
            cond = {0: f'{R(rs)} < 0', 1: f'{R(rs)} >= 0', 2: f'{R(rs)} < 0', 3: f'{R(rs)} >= 0',
                    16: f'{R(rs)} < 0', 17: f'{R(rs)} >= 0', 18: f'{R(rs)} < 0', 19: f'{R(rs)} >= 0'}.get(rt)
            if cond is None: return None
            likely = rt in (2, 3, 18, 19); link = rt >= 16
        if op == 17 and rs == 8:
            nd, tf = (w >> 17) & 1, (w >> 16) & 1
            cond = 'c->fcc' if tf else '!c->fcc'
            likely = bool(nd)
        if cond is None: return None
        if t == pc and not likely and S.word(pc + 4) == 0 and op == 4 and rs == 0 and rt == 0:
            return lambda ds: 'rt_idle_forever(c); return;'
        lk = setr(31, nxt) if link else ''
        g = self.goto(t, pc)
        if likely:
            return lambda ds: f'{lk} if ({cond}) {{ {ds} {g} }}'
        return lambda ds: f'{lk} {{ int bc = ({cond}); {ds} if (bc) {{ {g} }} }}'

    def insn(self, pc, w):
        op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
        sa = (w >> 6) & 31; fn = w & 0x3F; imm = w & 0xFFFF; si = s16(imm)
        if w == 0: return ''
        if op == 0:
            if fn == 0: return setr(rd, f'{RU(rt)} << {sa}')
            if fn == 2: return setr(rd, f'{RU(rt)} >> {sa}')
            if fn == 3: return setr(rd, f'{RW(rt)} >> {sa}')
            if fn == 4: return setr(rd, f'{RU(rt)} << ({RU(rs)} & 31)')
            if fn == 6: return setr(rd, f'{RU(rt)} >> ({RU(rs)} & 31)')
            if fn == 7: return setr(rd, f'{RW(rt)} >> ({RU(rs)} & 31)')
            if fn == 0x0C: return f'rt_syscall(c, 0x{pc:08X}u);'
            if fn == 0x0D: return f'rt_break(c, 0x{pc:08X}u);'
            if fn == 0x0F: return ''  # sync
            if fn == 0x10: return setr64(rd, 'c->hi')
            if fn == 0x11: return f'c->hi = {R(rs)};'
            if fn == 0x12: return setr64(rd, 'c->lo')
            if fn == 0x13: return f'c->lo = {R(rs)};'
            if fn == 0x14: return setr64(rd, f'(uint64_t){R(rt)} << ({RU(rs)} & 63)')
            if fn == 0x16: return setr64(rd, f'(uint64_t){R(rt)} >> ({RU(rs)} & 63)')
            if fn == 0x17: return setr64(rd, f'{R(rt)} >> ({RU(rs)} & 63)')
            if fn == 0x18: return f'{{ int64_t p = (int64_t){RW(rs)} * (int64_t){RW(rt)}; c->lo = (int32_t)p; c->hi = (int32_t)(p >> 32); }}'
            if fn == 0x19: return f'{{ uint64_t p = (uint64_t){RU(rs)} * (uint64_t){RU(rt)}; c->lo = (int32_t)p; c->hi = (int32_t)(p >> 32); }}'
            if fn == 0x1A: return f'rt_div(c, {RW(rs)}, {RW(rt)});'
            if fn == 0x1B: return f'rt_divu(c, {RU(rs)}, {RU(rt)});'
            if fn == 0x1C: return f'rt_dmult(c, {R(rs)}, {R(rt)});'
            if fn == 0x1D: return f'rt_dmultu(c, {R(rs)}, {R(rt)});'
            if fn == 0x1E: return f'rt_ddiv(c, {R(rs)}, {R(rt)});'
            if fn == 0x1F: return f'rt_ddivu(c, {R(rs)}, {R(rt)});'
            if fn in (0x20, 0x21): return setr(rd, f'{RU(rs)} + {RU(rt)}')
            if fn in (0x22, 0x23): return setr(rd, f'{RU(rs)} - {RU(rt)}')
            if fn == 0x24: return setr64(rd, f'{R(rs)} & {R(rt)}')
            if fn == 0x25: return setr64(rd, f'{R(rs)} | {R(rt)}')
            if fn == 0x26: return setr64(rd, f'{R(rs)} ^ {R(rt)}')
            if fn == 0x27: return setr64(rd, f'~({R(rs)} | {R(rt)})')
            if fn == 0x2A: return setr64(rd, f'{R(rs)} < {R(rt)}')
            if fn == 0x2B: return setr64(rd, f'(uint64_t){R(rs)} < (uint64_t){R(rt)}')
            if fn in (0x2C, 0x2D): return setr64(rd, f'{R(rs)} + {R(rt)}')
            if fn in (0x2E, 0x2F): return setr64(rd, f'{R(rs)} - {R(rt)}')
            if fn == 0x34: return f'if ({R(rs)} == {R(rt)}) rt_trap(c, 0x{pc:08X}u);'  # teq
            if fn == 0x36: return f'if ({R(rs)} != {R(rt)}) rt_trap(c, 0x{pc:08X}u);'  # tne
            if fn == 0x38: return setr64(rd, f'(uint64_t){R(rt)} << {sa}')
            if fn == 0x3A: return setr64(rd, f'(uint64_t){R(rt)} >> {sa}')
            if fn == 0x3B: return setr64(rd, f'{R(rt)} >> {sa}')
            if fn == 0x3C: return setr64(rd, f'(uint64_t){R(rt)} << {sa + 32}')
            if fn == 0x3E: return setr64(rd, f'(uint64_t){R(rt)} >> {sa + 32}')
            if fn == 0x3F: return setr64(rd, f'{R(rt)} >> {sa + 32}')
        if op in (8, 9): return setr(rt, f'{RU(rs)} + {si}')
        if op == 0x0A: return setr64(rt, f'{R(rs)} < {si}')
        if op == 0x0B: return setr64(rt, f'(uint64_t){R(rs)} < (uint64_t)(int64_t){si}')
        if op == 0x0C: return setr64(rt, f'{R(rs)} & {imm}')
        if op == 0x0D: return setr64(rt, f'{R(rs)} | {imm}')
        if op == 0x0E: return setr64(rt, f'{R(rs)} ^ {imm}')
        if op == 0x0F: return setr(rt, f'{imm << 16}u')
        if op in (0x18, 0x19): return setr64(rt, f'{R(rs)} + {si}')
        A = addr(rs, imm)
        if op == 0x20: return setr64(rt, f'MEM_B({A})')
        if op == 0x24: return setr64(rt, f'MEM_BU({A})')
        if op == 0x21: return setr64(rt, f'MEM_H({A})')
        if op == 0x25: return setr64(rt, f'MEM_HU({A})')
        if op == 0x23: return setr64(rt, f'MEM_W({A})')
        if op == 0x27: return setr64(rt, f'(uint32_t)MEM_W({A})')
        if op == 0x37: return setr64(rt, f'MEM_D({A})')
        if op == 0x22: return '' if rt == 0 else f'c->r[{rt}] = rt_lwl(c->r[{rt}], {A});'
        if op == 0x26: return '' if rt == 0 else f'c->r[{rt}] = rt_lwr(c->r[{rt}], {A});'
        if op == 0x28: return f'MEM_SB({A}, {R(rt)});'
        if op == 0x29: return f'MEM_SH({A}, {R(rt)});'
        if op == 0x2B: return f'MEM_SW({A}, {R(rt)});'
        if op == 0x3F: return f'MEM_SD({A}, {R(rt)});'
        if op == 0x2A: return f'rt_swl({A}, {RU(rt)});'
        if op == 0x2E: return f'rt_swr({A}, {RU(rt)});'
        if op == 0x2F: return ''  # cache
        if op == 0x31: return f'c->f[{rt}] = MEM_W({A});'  # lwc1
        if op == 0x39: return f'MEM_SW({A}, c->f[{rt}]);'  # swc1
        if op == 0x35: return f'{{ uint32_t ea = {A}; c->f[{rt}] = MEM_W(ea + 4); c->f[{rt + 1}] = MEM_W(ea); }}'  # ldc1
        if op == 0x3D: return f'{{ uint32_t ea = {A}; MEM_SW(ea + 4, c->f[{rt}]); MEM_SW(ea, c->f[{rt + 1}]); }}'  # sdc1
        if op == 0x10:  # cop0
            if rs == 0: return setr(rt, f'rt_mfc0(c, {rd})')
            if rs == 4: return f'rt_mtc0(c, {rd}, {RU(rt)});'
            if rs == 0x10: return f'rt_cop0op(c, {fn}, 0x{pc:08X}u);'
        if op == 0x11:
            return self.cop1(pc, w)
        self.warn.append(f'unhandled {w:08X} at {pc:08X}')
        return f'rt_unhandled(c, 0x{pc:08X}u, 0x{w:08X}u);'

    def cop1(self, pc, w):
        rs = (w >> 21) & 31; rt = (w >> 16) & 31; fs = (w >> 11) & 31; fd = (w >> 6) & 31; ft = rt; fn = w & 0x3F
        if rs == 0: return setr(rt, f'c->f[{fs}]')  # mfc1
        if rs == 4: return f'c->f[{fs}] = {RU(rt)};'  # mtc1
        if rs == 2: return setr(rt, f'rt_cfc1(c, {fs})')
        if rs == 6: return f'rt_ctc1(c, {fs}, {RU(rt)});'
        if rs == 1: return setr64(rt, f'FD_RAW({fs})')  # dmfc1
        if rs == 5: return f'FD_SETRAW({fs}, {R(rt)});'
        fmt = {16: 'S', 17: 'D', 20: 'W', 21: 'L'}.get(rs)
        if fmt is None: return f'rt_unhandled(c, 0x{pc:08X}u, 0x{w:08X}u);'
        G = {'S': lambda r: f'FS({r})', 'D': lambda r: f'FD({r})', 'W': lambda r: f'(int32_t)c->f[{r}]', 'L': lambda r: f'FL({r})'}[fmt]
        P = {'S': lambda r, v: f'FS_SET({r}, {v});', 'D': lambda r, v: f'FD_SET({r}, {v});'}
        if fmt in 'SD':
            put = P[fmt]
            if fn == 0: return put(fd, f'{G(fs)} + {G(ft)}')
            if fn == 1: return put(fd, f'{G(fs)} - {G(ft)}')
            if fn == 2: return put(fd, f'{G(fs)} * {G(ft)}')
            if fn == 3: return put(fd, f'{G(fs)} / {G(ft)}')
            if fn == 4: return put(fd, f'rt_sqrt{fmt.lower()}({G(fs)})')
            if fn == 5: return put(fd, f'rt_fabs{fmt.lower()}({G(fs)})')
            if fn == 6: return put(fd, G(fs))
            if fn == 7: return put(fd, f'-{G(fs)}')
            if fn in (8, 9, 10, 11):  # round/trunc/ceil/floor .l
                return f'FL_SET({fd}, rt_cvt_l(c, (double){G(fs)}, {fn - 8}));'
            if fn in (12, 13, 14, 15):  # round/trunc/ceil/floor .w
                return f'c->f[{fd}] = (uint32_t)rt_cvt_w(c, (double){G(fs)}, {fn - 12});'
            if fn == 0x20: return P['S'](fd, f'(float){G(fs)}')
            if fn == 0x21: return P['D'](fd, f'(double){G(fs)}')
            if fn == 0x24: return f'c->f[{fd}] = (uint32_t)rt_cvt_w(c, (double){G(fs)}, -1);'
            if fn == 0x25: return f'FL_SET({fd}, rt_cvt_l(c, (double){G(fs)}, -1));'
            if fn >= 0x30:
                cmp = fn & 0xF
                a, b = G(fs), G(ft)
                expr = {0: '0', 1: f'isnan({a})||isnan({b})', 2: f'{a} == {b}', 3: f'!({a} < {b} || {a} > {b})',
                        4: f'{a} < {b}', 5: f'!({a} >= {b})', 6: f'{a} <= {b}', 7: f'!({a} > {b})'}[cmp & 7]
                return f'c->fcc = ({expr});'
        else:  # W / L conversions
            src = G(fs)
            if fn == 0x20: return P['S'](fd, f'(float){src}')
            if fn == 0x21: return P['D'](fd, f'(double){src}')
        self.warn.append(f'unhandled cop1 {w:08X} at {pc:08X}')
        return f'rt_unhandled(c, 0x{pc:08X}u, 0x{w:08X}u);'


def main():
    os.makedirs(OUT, exist_ok=True)
    starts = function_starts()
    refs = data_code_refs()
    starts_set = set(starts)
    sym = {}
    for f in ('build/symbols.json', 'build/os_symbols.json'):
        p = os.path.join(ROOT, f)
        if os.path.exists(p):
            for k, v in json.load(open(p)).items(): sym.setdefault(v, k)
    # function ends: next start within same text range
    fns = []
    for i, s in enumerate(starts):
        seg_end = next(e for (ts, e) in TEXT.values() if ts <= s < e)
        e = starts[i + 1] if i + 1 < len(starts) and starts[i + 1] < seg_end else seg_end
        fns.append((s, e))
    warns, files, cur, size, nfile = [], [], [], 0, 0
    protos = []
    for s, e in fns:
        f = Fn(s, e, starts_set, refs)
        code = f.emit()
        nm = sym.get(s)
        if nm: code = f'/* {nm} */\n' + code
        warns += f.warn
        protos.append(f'void F_{s:08X}(Ctx* c);')
        cur.append(code); size += len(code)
        if size > 1_500_000:
            files.append(cur); cur, size = [], 0
    files.append(cur)
    hdr = '#include "rt.h"\n#include "funcs.h"\n'
    for i, chunk in enumerate(files):
        open(os.path.join(OUT, f'rec_{i:02d}.c'), 'w').write(hdr + '\n'.join(chunk))
    open(os.path.join(OUT, 'funcs.h'), 'w').write('#pragma once\n#include "rt.h"\n' + '\n'.join(protos) +
                                                  '\n' + '\n'.join(f'void H_{n}(Ctx* c);' for n in HLE) + '\n')
    # lookup table for indirect calls
    tab = ',\n'.join(f'{{0x{s:08X}u, F_{s:08X}}}' for s, e in fns)
    hle_over = '\n'.join(f'    if (a == 0x{int(v, 16):08X}u) return H_{n};' for n, v in HLE.items())
    open(os.path.join(OUT, 'table.c'), 'w').write(
        '#include "rt.h"\n#include "funcs.h"\n'
        f'const FnEntry rt_fn_table[] = {{\n{tab}\n}};\nconst int rt_fn_count = {len(fns)};\n'
        f'RecompFn rt_hle_lookup(uint32_t a) {{\n{hle_over}\n    return 0;\n}}\n')
    print(f'{len(fns)} functions, {len(files)} files, {len(refs)} code refs from data, {len(warns)} warnings')
    for w_ in warns[:30]: print('  ', w_)
    json.dump({f'{s:08X}': sym.get(s, '') for s, e in fns}, open(os.path.join(ROOT, 'build/fnlist.json'), 'w'))


if __name__ == '__main__':
    main()
