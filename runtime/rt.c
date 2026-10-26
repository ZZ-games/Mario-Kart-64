// CPU-level runtime: memory helpers, indirect calls, integer/FPU helpers, COP0.
#include "rt.h"

__attribute__((aligned(16))) uint8_t rdram[RDRAM_SIZE];
uint32_t rt_cur_fn[17]; int rt_depth[16]; uint32_t rt_stk[16][128];
static uint32_t abi_seen[64]; static int abi_n;
void rt_abi_violation(uint32_t fn, int reg, uint32_t before, uint32_t after) {
    for (int i = 0; i < abi_n; i++) if (abi_seen[i] == fn) return;
    if (abi_n < 64) abi_seen[abi_n++] = fn;
    rt_log("ABI violation: function, reg<<24|low bits", fn, ((uint32_t)reg << 24) | (after & 0xFFFFFF));
}

// imports from the host (JavaScript)
__attribute__((import_name("js_log"))) void js_log(const char* msg, uint32_t a, uint32_t b);
__attribute__((import_name("js_fatal"))) void js_fatal(const char* msg, uint32_t a, uint32_t b);

void rt_log(const char* msg, uint32_t a, uint32_t b) { js_log(msg, a, b); }
void rt_fatal(const char* msg, uint32_t a, uint32_t b) { js_fatal(msg, a, b); __builtin_trap(); }

void* memcpy(void* d, const void* s, size_t n) { uint8_t* dd = d; const uint8_t* ss = s; while (n--) *dd++ = *ss++; return d; }
void* memset(void* d, int v, size_t n) { uint8_t* dd = d; while (n--) *dd++ = (uint8_t)v; return d; }

// ---- I/O space (SP DMEM/IMEM, registers): only reached by code we don't replace
uint32_t sp_mem[0x2000 / 4];
uint32_t rt_io_read32(uint32_t p) {
    if (p >= 0x04000000 && p < 0x04002000) return sp_mem[(p - 0x04000000) >> 2];
    if (p == 0x04040010) return 1;              // SP_STATUS: halted
    if (p == 0x04300004) return 0x02020102;     // MI_VERSION
    rt_log("io read (addr, function)", p, rt_cur_fn[0]);
    return 0;
}
void rt_io_write32(uint32_t p, uint32_t v) {
    if (p >= 0x04000000 && p < 0x04002000) { sp_mem[(p - 0x04000000) >> 2] = v; return; }
    rt_log("io write (addr, function)", p, rt_cur_fn[0]);
}
void rt_bad_access(uint32_t a, int write) { rt_log(write ? "bad byte write" : "bad access", a, 0); }

// ---- unaligned word access (big-endian semantics)
int64_t rt_lwl(int64_t old, uint32_t a) {
    uint32_t o = a & 3, w = (uint32_t)MEM_W(a & ~3u);
    uint32_t r = (uint32_t)old;
    r = (r & ((1u << (8 * o)) - 1)) | (w << (8 * o));
    return (int32_t)r;
}
int64_t rt_lwr(int64_t old, uint32_t a) {
    uint32_t o = a & 3, w = (uint32_t)MEM_W(a & ~3u);
    uint32_t r = (uint32_t)old;
    uint32_t sh = 8 * (3 - o);
    uint32_t mask = o == 3 ? 0 : (0xFFFFFFFFu << (8 * (o + 1)));
    r = (r & mask) | (w >> sh);
    return (int32_t)r;
}
void rt_swl(uint32_t a, uint32_t v) {
    uint32_t o = a & 3, w = (uint32_t)MEM_W(a & ~3u);
    uint32_t mask = o == 0 ? 0 : (0xFFFFFFFFu << (8 * (4 - o)));
    w = (w & mask) | (v >> (8 * o));
    MEM_SW(a & ~3u, (int32_t)w);
}
void rt_swr(uint32_t a, uint32_t v) {
    uint32_t o = a & 3, w = (uint32_t)MEM_W(a & ~3u);
    uint32_t sh = 8 * (3 - o);
    uint32_t mask = o == 3 ? 0 : ((1u << sh) - 1);
    w = (w & mask) | (v << sh);
    MEM_SW(a & ~3u, (int32_t)w);
}

// ---- integer helpers
void rt_div(Ctx* c, int32_t a, int32_t b) {
    if (b == 0) { c->lo = a >= 0 ? -1 : 1; c->hi = a; return; }
    if (a == (int32_t)0x80000000 && b == -1) { c->lo = a; c->hi = 0; return; }
    c->lo = a / b; c->hi = a % b;
}
void rt_divu(Ctx* c, uint32_t a, uint32_t b) {
    if (b == 0) { c->lo = -1; c->hi = (int32_t)a; return; }
    c->lo = (int32_t)(a / b); c->hi = (int32_t)(a % b);
}
static void mul128(uint64_t a, uint64_t b, uint64_t* lo, uint64_t* hi) {
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    *lo = (mid << 32) | (uint32_t)p00;
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}
void rt_dmultu(Ctx* c, uint64_t a, uint64_t b) { uint64_t lo, hi; mul128(a, b, &lo, &hi); c->lo = (int64_t)lo; c->hi = (int64_t)hi; }
void rt_dmult(Ctx* c, int64_t a, int64_t b) {
    uint64_t lo, hi; mul128((uint64_t)a, (uint64_t)b, &lo, &hi);
    if (a < 0) hi -= (uint64_t)b;
    if (b < 0) hi -= (uint64_t)a;
    c->lo = (int64_t)lo; c->hi = (int64_t)hi;
}
void rt_ddiv(Ctx* c, int64_t a, int64_t b) {
    if (b == 0) { c->lo = a >= 0 ? -1 : 1; c->hi = a; return; }
    if (a == (int64_t)0x8000000000000000ull && b == -1) { c->lo = a; c->hi = 0; return; }
    c->lo = a / b; c->hi = a % b;
}
void rt_ddivu(Ctx* c, uint64_t a, uint64_t b) {
    if (b == 0) { c->lo = -1; c->hi = (int64_t)a; return; }
    c->lo = (int64_t)(a / b); c->hi = (int64_t)(a % b);
}

// ---- FPU control
uint32_t rt_cfc1(Ctx* c, int r) { return r == 31 ? c->fcr31 : (r == 0 ? 0x0B00 : 0); }
void rt_ctc1(Ctx* c, int r, uint32_t v) { if (r == 31) c->fcr31 = v; }
static double rnd(double v, int mode) {
    switch (mode) {
        case 0: { double f = __builtin_floor(v), d = v - f;  // round to nearest even
                  if (d > 0.5 || (d == 0.5 && (f - 2.0 * __builtin_floor(f / 2.0)) != 0)) f += 1; return f; }
        case 1: return __builtin_trunc(v);
        case 2: return __builtin_ceil(v);
        default: return __builtin_floor(v);
    }
}
int32_t rt_cvt_w(Ctx* c, double v, int mode) {
    if (mode < 0) mode = c->fcr31 & 3;
    double r = rnd(v, mode);
    if (!(r == r) || r >= 2147483648.0 || r < -2147483648.0) return (int32_t)0x7FFFFFFF;
    return (int32_t)r;
}
int64_t rt_cvt_l(Ctx* c, double v, int mode) {
    if (mode < 0) mode = c->fcr31 & 3;
    double r = rnd(v, mode);
    if (!(r == r) || r >= 9.2233720368547758e18 || r < -9.2233720368547758e18) return 0x7FFFFFFFFFFFFFFFll;
    return (int64_t)r;
}

// ---- COP0
extern uint64_t rt_count(void);
static uint32_t cop0_status = 0x34000000, cop0_compare = 0;
uint32_t rt_mfc0(Ctx* c, int r) {
    switch (r) {
        case 9: return (uint32_t)rt_count();
        case 11: return cop0_compare;
        case 12: return cop0_status;
        case 13: return 0;
        case 15: return 0x00000B22;
    }
    return 0;
}
void rt_mtc0(Ctx* c, int r, uint32_t v) {
    if (r == 12) cop0_status = v;
    else if (r == 11) cop0_compare = v;
}
void rt_cop0op(Ctx* c, int fn, uint32_t pc) {
    if (fn == 0x18) rt_fatal("eret reached (libultra exception code not replaced)", pc, 0);
    // tlb ops: ignore
}

// ---- calls
void rt_call(Ctx* c, uint32_t addr) {
    RecompFn h = rt_hle_lookup(addr);
    if (h) { h(c); return; }
    int lo = 0, hi = rt_fn_count - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint32_t a = rt_fn_table[m].addr;
        if (a == addr) { rt_fn_table[m].fn(c); return; }
        if (a < addr) lo = m + 1; else hi = m - 1;
    }
    rt_fatal("call to address with no recompiled function", addr, (uint32_t)c->r[31]);
}
void rt_bad_jump(Ctx* c, uint32_t addr) { rt_fatal("jump outside known code", addr, 0); }
void rt_syscall(Ctx* c, uint32_t pc) { rt_fatal("syscall", pc, 0); }
void rt_break(Ctx* c, uint32_t pc) { rt_log("break instruction (divide-by-zero check)", pc, 0); }
void rt_trap(Ctx* c, uint32_t pc) { rt_fatal("trap", pc, 0); }
void rt_unhandled(Ctx* c, uint32_t pc, uint32_t w) { rt_fatal("unhandled instruction", pc, w); }
