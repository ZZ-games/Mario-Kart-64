// Runtime interface for recompiled Mario Kart 64 code.
#pragma once
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;
typedef unsigned long size_t;

typedef struct Ctx {
    int64_t r[32];
    int64_t hi, lo;
    uint32_t f[32];   // FPU registers, FR=0 mode: doubles use even/odd pairs
    uint32_t fcr31;
    int fcc;
    int slot;         // scheduler slot of the N64 thread using this context
} Ctx;

typedef void (*RecompFn)(Ctx*);
typedef struct { uint32_t addr; RecompFn fn; } FnEntry;
extern const FnEntry rt_fn_table[];
extern const int rt_fn_count;
RecompFn rt_hle_lookup(uint32_t a);

// ---- memory: 8 MB RDRAM, word-swapped -------------------------------------
#define RDRAM_SIZE 0x800000u
extern uint8_t rdram[RDRAM_SIZE];

uint32_t rt_io_read32(uint32_t phys);
void rt_io_write32(uint32_t phys, uint32_t v);
void rt_bad_access(uint32_t a, int write);

static inline uint32_t PHYS(uint32_t a) { return a & 0x1FFFFFFFu; }
static inline int32_t MEM_W(uint32_t a) {
    uint32_t p = PHYS(a);
    if (p < RDRAM_SIZE) return *(int32_t*)(rdram + (p & ~3u));
    return (int32_t)rt_io_read32(p);
}
static inline void MEM_SW(uint32_t a, int64_t v) {
    uint32_t p = PHYS(a);
    if (p < RDRAM_SIZE) *(int32_t*)(rdram + (p & ~3u)) = (int32_t)v;
    else rt_io_write32(p, (uint32_t)v);
}
static inline int8_t MEM_B(uint32_t a) {
    uint32_t p = PHYS(a);
    if (p < RDRAM_SIZE) return (int8_t)rdram[p ^ 3];
    return (int8_t)(rt_io_read32(p & ~3u) >> (24 - 8 * (p & 3)));
}
static inline uint8_t MEM_BU(uint32_t a) { return (uint8_t)MEM_B(a); }
static inline void MEM_SB(uint32_t a, int64_t v) {
    uint32_t p = PHYS(a);
    if (p < RDRAM_SIZE) rdram[p ^ 3] = (uint8_t)v; else rt_bad_access(a, 1);
}
static inline int16_t MEM_H(uint32_t a) {
    uint32_t p = PHYS(a);
    if (p < RDRAM_SIZE) return *(int16_t*)(rdram + ((p ^ 2) & ~1u));
    return (int16_t)(rt_io_read32(p & ~3u) >> (16 - 8 * (p & 2)));
}
static inline uint16_t MEM_HU(uint32_t a) { return (uint16_t)MEM_H(a); }
static inline void MEM_SH(uint32_t a, int64_t v) {
    uint32_t p = PHYS(a);
    if (p < RDRAM_SIZE) *(int16_t*)(rdram + ((p ^ 2) & ~1u)) = (int16_t)v; else rt_bad_access(a, 1);
}
static inline int64_t MEM_D(uint32_t a) { return ((int64_t)MEM_W(a) << 32) | (uint32_t)MEM_W(a + 4); }
static inline void MEM_SD(uint32_t a, int64_t v) { MEM_SW(a, v >> 32); MEM_SW(a + 4, v); }

int64_t rt_lwl(int64_t old, uint32_t a);
int64_t rt_lwr(int64_t old, uint32_t a);
void rt_swl(uint32_t a, uint32_t v);
void rt_swr(uint32_t a, uint32_t v);

// ---- FPU --------------------------------------------------------------------
static inline float u2f(uint32_t u) { union { uint32_t u; float f; } x; x.u = u; return x.f; }
static inline uint32_t f2u(float f) { union { uint32_t u; float f; } x; x.f = f; return x.u; }
static inline double u2d(uint64_t u) { union { uint64_t u; double d; } x; x.u = u; return x.d; }
static inline uint64_t d2u(double d) { union { uint64_t u; double d; } x; x.d = d; return x.u; }
#define FS(r) u2f(c->f[r])
#define FS_SET(r, v) (c->f[r] = f2u((float)(v)))
#define FD_RAW(r) (((uint64_t)c->f[(r) + 1] << 32) | c->f[r])
#define FD(r) u2d(FD_RAW(r))
#define FD_SETRAW(r, v) do { uint64_t _v = (uint64_t)(v); c->f[r] = (uint32_t)_v; c->f[(r) + 1] = (uint32_t)(_v >> 32); } while (0)
#define FD_SET(r, v) FD_SETRAW(r, d2u((double)(v)))
#define FL(r) ((int64_t)FD_RAW(r))
#define FL_SET(r, v) FD_SETRAW(r, (int64_t)(v))
#define isnan(x) __builtin_isnan(x)
static inline float rt_sqrts(float x) { return __builtin_sqrtf(x); }
static inline double rt_sqrtd(double x) { return __builtin_sqrt(x); }
static inline float rt_fabss(float x) { return __builtin_fabsf(x); }
static inline double rt_fabsd(double x) { return __builtin_fabs(x); }
int32_t rt_cvt_w(Ctx* c, double v, int mode);
int64_t rt_cvt_l(Ctx* c, double v, int mode);
uint32_t rt_cfc1(Ctx* c, int r);
void rt_ctc1(Ctx* c, int r, uint32_t v);

// ---- integer helpers / system ---------------------------------------------
void rt_div(Ctx* c, int32_t a, int32_t b);
void rt_divu(Ctx* c, uint32_t a, uint32_t b);
void rt_dmult(Ctx* c, int64_t a, int64_t b);
void rt_dmultu(Ctx* c, uint64_t a, uint64_t b);
void rt_ddiv(Ctx* c, int64_t a, int64_t b);
void rt_ddivu(Ctx* c, uint64_t a, uint64_t b);
uint32_t rt_mfc0(Ctx* c, int r);
void rt_mtc0(Ctx* c, int r, uint32_t v);
void rt_cop0op(Ctx* c, int fn, uint32_t pc);
void rt_call(Ctx* c, uint32_t addr);
void rt_bad_jump(Ctx* c, uint32_t addr);
void rt_idle_forever(Ctx* c);
void rt_syscall(Ctx* c, uint32_t pc);
void rt_break(Ctx* c, uint32_t pc);
void rt_trap(Ctx* c, uint32_t pc);
void rt_unhandled(Ctx* c, uint32_t pc, uint32_t w);
void rt_fatal(const char* msg, uint32_t a, uint32_t b);
void rt_log(const char* msg, uint32_t a, uint32_t b);

#ifdef DEBUG_TRACE
extern uint32_t rt_cur_fn[17]; extern int rt_depth[16]; extern uint32_t rt_stk[16][128];
extern int rt_tls_slot;
static inline int rt_push(Ctx* c, uint32_t a) { int s = c->slot & 15; int d = rt_depth[s]++; if (d < 128) rt_stk[s][d] = a; rt_cur_fn[s] = a; return s; }
typedef struct { Ctx* c; uint32_t fn; int64_t saved[10]; } RtFrame;
void rt_abi_violation(uint32_t fn, int reg, uint32_t before, uint32_t after);
static inline void rt_pop(RtFrame* f) {
    Ctx* c = f->c; rt_depth[c->slot & 15]--;
    static const int regs[10] = {16, 17, 18, 19, 20, 21, 22, 23, 29, 30};
    for (int i = 0; i < 10; i++) if (c->r[regs[i]] != f->saved[i]) rt_abi_violation(f->fn, regs[i], (uint32_t)f->saved[i], (uint32_t)c->r[regs[i]]);
}
static inline RtFrame rt_enter(Ctx* c, uint32_t a) {
    RtFrame f; f.c = c; f.fn = a; rt_push(c, a);
    static const int regs[10] = {16, 17, 18, 19, 20, 21, 22, 23, 29, 30};
    for (int i = 0; i < 10; i++) f.saved[i] = c->r[regs[i]];
    return f;
}
#define TRACE(a) __attribute__((cleanup(rt_pop))) RtFrame _tr = rt_enter(c, a)
#else
#define TRACE(a) ((void)0)
#endif
extern int rt_preempt;
void rt_preempt_point(Ctx* c);
static inline void PREEMPT_CHECK(Ctx* c) { if (__atomic_load_n(&rt_preempt, __ATOMIC_RELAXED)) rt_preempt_point(c); }
void* memcpy(void* d, const void* s, size_t n);
void* memset(void* d, int v, size_t n);
