// Replacements for the N64 OS (libultra): threads, messages, events, DMA, RSP tasks,
// video, controllers, EEPROM and audio output.
//
// Every N64 thread runs in its own Web Worker over shared memory. A scheduler
// passes a baton so exactly one N64 thread executes at a time, in priority
// order, the way the console's single CPU behaved. Hardware "interrupts"
// (vertical blank, RSP done, ...) are posted by the host as events.
#include "rt.h"

#define A0 ((uint32_t)c->r[4])
#define A1 ((uint32_t)c->r[5])
#define A2 ((uint32_t)c->r[6])
#define A3 ((uint32_t)c->r[7])
#define SARG(n) ((uint32_t)MEM_W((uint32_t)c->r[29] + 16 + 4 * (n)))
#define RET(v) (c->r[2] = (int64_t)(int32_t)(v))

__attribute__((import_name("js_spawn"))) void js_spawn(int slot);
__attribute__((import_name("js_gfx_task"))) void js_gfx_task(uint32_t task);
__attribute__((import_name("js_audio_task"))) void js_audio_task(uint32_t task);

// ROM image (big-endian bytes) loaded by the host
__attribute__((aligned(16))) uint8_t rom[0x1000000];
uint32_t rom_size;
uint8_t* rt_rom_ptr(void) { return rom; }
uint8_t* rt_rdram_ptr(void) { return rdram; }

// ---------------------------------------------------------------- scheduler
#define MAXT 16
enum { T_FREE, T_READY, T_RUNNING, T_BLOCKED, T_PARKED };
typedef struct {
    uint32_t thread, entry, arg, sp;
    int pri, state, seq, spawned;
    uint32_t waitq; int waitfull;
} Slot;
Slot T[MAXT];
int nslots;
int go[MAXT];            // futex words: 1 = this slot holds the baton
volatile int running = -1;
static int seqctr, lockw;
int rt_preempt;
#define preempt rt_preempt
uint32_t sched_log[64];

static void lock(void) { while (__atomic_exchange_n(&lockw, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&lockw, 0, __ATOMIC_RELEASE); }

static int pick(void) {
    int best = -1;
    for (int i = 0; i < nslots; i++)
        if (T[i].state == T_READY && (best < 0 || T[i].pri > T[best].pri || (T[i].pri == T[best].pri && T[i].seq < T[best].seq)))
            best = i;
    return best;
}
static void give(int s) {  // lock held
    running = s;
    if (s >= 0) {
        T[s].state = T_RUNNING;
        if (!T[s].spawned) { T[s].spawned = 1; js_spawn(s); }
        __atomic_store_n(&go[s], 1, __ATOMIC_SEQ_CST);
        __builtin_wasm_memory_atomic_notify(&go[s], 1);
    }
}
static void wait_baton(int s) {
    while (__atomic_load_n(&go[s], __ATOMIC_SEQ_CST) == 0)
        __builtin_wasm_memory_atomic_wait32(&go[s], 0, -1);
    __atomic_store_n(&go[s], 0, __ATOMIC_SEQ_CST);
}
// Current thread gives up the CPU (its state already set); lock held on entry, released here.
static void reschedule(int cur) {
    int n = pick();
    preempt = 0;
    if (n == cur) { T[cur].state = T_RUNNING; running = cur; unlock(); return; }
    give(n);
    unlock();
    wait_baton(cur);
}
static void ready(int s) { T[s].state = T_READY; T[s].seq = ++seqctr; }
// called by the running thread at OS calls: honor a pending preemption from an event
void rt_preempt_point(Ctx* c);
static void check_preempt(Ctx* c) {
    if (!__atomic_load_n(&preempt, __ATOMIC_SEQ_CST)) return;
    lock();
    int n = pick();
    if (n >= 0 && T[n].pri > T[c->slot].pri) { ready(c->slot); reschedule(c->slot); }
    else { preempt = 0; unlock(); }
}

void rt_preempt_point(Ctx* c) { check_preempt(c); }

static int slot_of(uint32_t thread) {
    for (int i = 0; i < nslots; i++) if (T[i].thread == thread) return i;
    return -1;
}

// ---------------------------------------------------------------- messages
#define Q_VALID(q) ((q) + 8)
#define Q_FIRST(q) ((q) + 12)
#define Q_COUNT(q) ((q) + 16)
#define Q_MSG(q) ((q) + 20)

static int wake_waiter(uint32_t q, int full) {  // lock held; returns slot woken or -1
    int best = -1;
    for (int i = 0; i < nslots; i++)
        if (T[i].state == T_BLOCKED && T[i].waitq == q && T[i].waitfull == full && (best < 0 || T[i].pri > T[best].pri)) best = i;
    if (best >= 0) ready(best);
    return best;
}
// insert a message; lock held. returns 0, or -1 if the queue is full
static int q_put(uint32_t q, uint32_t msg, int jam) {
    int32_t valid = MEM_W(Q_VALID(q)), count = MEM_W(Q_COUNT(q)), first = MEM_W(Q_FIRST(q));
    if (valid >= count) return -1;
    if (jam) { first = (first + count - 1) % count; MEM_SW(Q_FIRST(q), first); MEM_SW(MEM_W(Q_MSG(q)) + 4 * first, msg); }
    else MEM_SW((uint32_t)MEM_W(Q_MSG(q)) + 4 * ((first + valid) % count), msg);
    MEM_SW(Q_VALID(q), valid + 1);
    return 0;
}

static int send(Ctx* c, uint32_t q, uint32_t msg, int block, int jam) {
    int cur = c ? c->slot : -1;
    lock();
    while (q_put(q, msg, jam) < 0) {
        if (!block || cur < 0) { unlock(); return -1; }
        T[cur].state = T_BLOCKED; T[cur].waitq = q; T[cur].waitfull = 1;
        reschedule(cur);
        lock();
    }
    int w = wake_waiter(q, 0);
    if (cur >= 0) {
        if (w >= 0 && T[w].pri > T[cur].pri) { ready(cur); reschedule(cur); return 0; }
        unlock(); return 0;
    }
    // from the host (an interrupt): run something if the CPU is idle, else flag preemption
    if (running < 0) give(pick());
    else if (w >= 0 && T[w].pri > T[running].pri) preempt = 1;
    unlock();
    return 0;
}

void H_osSendMesg(Ctx* c) { RET(send(c, A0, A1, A2 == 1, 0)); }
void H_osJamMesg(Ctx* c) { RET(send(c, A0, A1, A2 == 1, 1)); }
void H_osRecvMesg(Ctx* c) {
    uint32_t q = A0, out = A1; int block = A2 == 1, cur = c->slot;
    check_preempt(c);
    lock();
    while (MEM_W(Q_VALID(q)) == 0) {
        if (!block) { unlock(); RET(-1); return; }
        T[cur].state = T_BLOCKED; T[cur].waitq = q; T[cur].waitfull = 0;
        reschedule(cur);
        lock();
    }
    int32_t first = MEM_W(Q_FIRST(q)), count = MEM_W(Q_COUNT(q));
    uint32_t msg = MEM_W((uint32_t)MEM_W(Q_MSG(q)) + 4 * first);
    if (out) MEM_SW(out, msg);
    MEM_SW(Q_FIRST(q), (first + 1) % count);
    MEM_SW(Q_VALID(q), MEM_W(Q_VALID(q)) - 1);
    int w = wake_waiter(q, 1);
    if (w >= 0 && T[w].pri > T[cur].pri) { ready(cur); reschedule(cur); }
    else unlock();
    RET(0);
}

// ---------------------------------------------------------------- events
enum { EV_SW1, EV_SW2, EV_CART, EV_COUNTER, EV_SP, EV_SI, EV_AI, EV_VI, EV_PI, EV_DP, EV_NUM = 15 };
static uint32_t ev_q[EV_NUM], ev_msg[EV_NUM];
static uint32_t vi_q, vi_msg, vi_retrace = 1, vi_counter;
uint32_t vi_next_fb, vi_cur_fb, vi_frames;
void H_osSetEventMesg(Ctx* c) { if (A0 < EV_NUM) { ev_q[A0] = A1; ev_msg[A0] = A2; } }
void H_osViSetEvent(Ctx* c) { vi_q = A0; vi_msg = A1; vi_retrace = A2 ? A2 : 1; }
static void post(Ctx* c, int ev) { if (ev_q[ev]) send(c, ev_q[ev], ev_msg[ev], 0, 0); }

// host: one vertical blank
static uint64_t count_base;
void rt_vblank(void) {
    vi_frames++;
    vi_cur_fb = vi_next_fb;
    count_base += 781250;  // 46.875 MHz / 60
    if (vi_q && ++vi_counter >= vi_retrace) { vi_counter = 0; send(0, vi_q, vi_msg, 0, 0); }
}
// host: finished an RSP task it was handed
void rt_task_done(int gfx) { post(0, EV_SP); if (gfx) post(0, EV_DP); }

static uint64_t count_tick;
uint64_t rt_count(void) { return count_base + (count_tick += 2000); }

// ---------------------------------------------------------------- threads
void H_osInitialize(Ctx* c) {}
void H_osCreateThread(Ctx* c) {
    uint32_t t = A0;
    lock();
    int s = slot_of(t);
    if (s < 0) s = nslots++;
    Slot* S = &T[s];
    S->thread = t; S->entry = A2; S->arg = A3; S->sp = SARG(0); S->pri = (int32_t)SARG(1);
    S->state = T_PARKED; S->spawned = 0;
    MEM_SW(t + 4, S->pri); MEM_SW(t + 0x14, A1);
    unlock();
}
void H_osStartThread(Ctx* c) {
    lock();
    int s = slot_of(A0);
    if (s < 0) { unlock(); rt_fatal("osStartThread on unknown thread", A0, 0); }
    ready(s);
    int cur = c->slot;
    if (T[cur].pri < 0) {  // the boot context: hand over and never come back
        T[cur].state = T_PARKED; reschedule(cur);
        for (;;) wait_baton(cur);
    }
    if (T[s].pri > T[cur].pri) { ready(cur); reschedule(cur); } else unlock();
}
void H_osSetThreadPri(Ctx* c) {
    lock();
    int s = A0 ? slot_of(A0) : c->slot;
    T[s].pri = (int32_t)A1; MEM_SW(T[s].thread + 4, T[s].pri);
    int cur = c->slot, n = pick();
    if (n >= 0 && T[n].pri > T[cur].pri) { ready(cur); reschedule(cur); } else unlock();
}
void H_osGetThreadPri(Ctx* c) { int s = A0 ? slot_of(A0) : c->slot; RET(T[s].pri); }
void H_osYieldThread(Ctx* c) { lock(); ready(c->slot); reschedule(c->slot); }
void H_osStopThread(Ctx* c) {
    lock();
    int s = A0 ? slot_of(A0) : c->slot;
    T[s].state = T_PARKED;
    if (s == c->slot) reschedule(s); else unlock();
}
void H_osDestroyThread(Ctx* c) { H_osStopThread(c); }
// infinite idle loop: this thread never needs the CPU again
void rt_idle_forever(Ctx* c) {
    lock();
    T[c->slot].state = T_PARKED;
    reschedule(c->slot);
    for (;;) wait_baton(c->slot);
}

// worker entry: run N64 thread `s` once it is given the baton
static Ctx ctxs[MAXT];
__attribute__((aligned(16))) uint8_t thread_stacks[MAXT][0x100000];
uint32_t rt_stack_top(int s) { return (uint32_t)(thread_stacks[s] + sizeof(thread_stacks[s]) - 16); }
void rt_thread_main(int s) {
    wait_baton(s);
    Ctx* c = &ctxs[s];
    c->slot = s;
    c->r[4] = (int32_t)T[s].arg;
    c->r[29] = (int32_t)T[s].sp;
    c->r[31] = 0;
    c->fcr31 = 0x01000800;
    rt_call(c, T[s].entry);
    rt_idle_forever(c);  // thread returned: retire it
}

// boot: copy the first 1 MB of code like the N64 boot loader, then run entry_point
extern void dma_rom(uint32_t dst, uint32_t src, uint32_t len);
void rt_boot(void) {
    dma_rom(0x80000400, 0x1000, 0x100000);
    lock();
    nslots = 1;
    T[0].thread = 0; T[0].entry = 0x80000400; T[0].pri = -1; T[0].sp = 0x80400000; T[0].spawned = 1;
    T[0].state = T_RUNNING; running = 0;
    go[0] = 1;
    unlock();
}

// ---------------------------------------------------------------- DMA
void dma_rom(uint32_t dst, uint32_t src, uint32_t len) {
    uint32_t p = PHYS(dst);
    if (((p | src | len) & 3) == 0) {
        for (uint32_t i = 0; i < len; i += 4) {
            if (p + i >= RDRAM_SIZE || src + i + 4 > sizeof(rom)) break;
            uint32_t w = ((uint32_t)rom[src + i] << 24) | ((uint32_t)rom[src + i + 1] << 16) | ((uint32_t)rom[src + i + 2] << 8) | rom[src + i + 3];
            *(uint32_t*)(rdram + p + i) = w;
        }
    } else {
        for (uint32_t i = 0; i < len; i++) if (p + i < RDRAM_SIZE) rdram[(p + i) ^ 3] = rom[src + i];
    }
}
void H_osPiStartDma(Ctx* c) {
    uint32_t mb = A0, dir = A2, dev = A3, vaddr = SARG(0), n = SARG(1), mq = SARG(2);
    if (dir == 0) dma_rom(vaddr, dev & 0x0FFFFFFF, n);
    if (mq) send(c, mq, mb, 0, 0);
    RET(0);
}
void H_osCreatePiManager(Ctx* c) {}
void H_osInvalDCache(Ctx* c) {}
void H_osInvalICache(Ctx* c) {}
void H_osWritebackDCacheAll(Ctx* c) {}
void H_osWritebackDCache(Ctx* c) {}

// ---------------------------------------------------------------- RSP tasks
void gfx_run_task(uint32_t task);
uint32_t sp_task, gfx_tasks, audio_tasks;
void H_osSpTaskLoad(Ctx* c) { sp_task = A0; }
void H_osSpTaskStartGo(Ctx* c) {
    uint32_t task = A0 ? A0 : sp_task;
    uint32_t type = MEM_W(task);
    if (type == 1) { gfx_tasks++; gfx_run_task(task); js_gfx_task(task); post(c, EV_SP); post(c, EV_DP); }
    else if (type == 2) { audio_tasks++; js_audio_task(task); post(c, EV_SP); }
    else rt_log("unknown sp task type", type, task);
    check_preempt(c);
}
void H_osSpTaskYield(Ctx* c) {}
void H_osSpTaskYielded(Ctx* c) { RET(0); }

// ---------------------------------------------------------------- video
void H_osCreateViManager(Ctx* c) {}
void H_osViSetMode(Ctx* c) {}
void H_osViBlack(Ctx* c) {}
void H_osViSetSpecialFeatures(Ctx* c) {}
void H_osViSwapBuffer(Ctx* c) { vi_next_fb = A0; }

// ---------------------------------------------------------------- time
void H_osGetTime(Ctx* c) { uint64_t t = rt_count(); c->r[2] = (int32_t)(t >> 32); c->r[3] = (int32_t)t; }
void H_osSetTime(Ctx* c) { count_base = ((uint64_t)A0 << 32) | A1; count_tick = 0; }

// ---------------------------------------------------------------- controllers (host fills pads[])
uint16_t pad_buttons[4]; int8_t pad_x[4], pad_y[4]; uint32_t pad_mask = 1;
void H_osContInit(Ctx* c) {
    uint32_t bits = A1, st = A2;
    MEM_SB(bits, pad_mask);
    for (int i = 0; i < 4; i++) {
        MEM_SH(st + 4 * i, (pad_mask >> i) & 1 ? 0x0005 : 0);
        MEM_SB(st + 4 * i + 2, 0);
        MEM_SB(st + 4 * i + 3, (pad_mask >> i) & 1 ? 0 : 8);
    }
    RET(0);
}
void H_osContStartReadData(Ctx* c) { post(c, EV_SI); RET(0); }
void H_osContGetReadData(Ctx* c) {
    uint32_t d = A0;
    for (int i = 0; i < 4; i++) {
        MEM_SH(d + 6 * i, pad_buttons[i]);
        MEM_SB(d + 6 * i + 2, pad_x[i]);
        MEM_SB(d + 6 * i + 3, pad_y[i]);
        MEM_SB(d + 6 * i + 4, (pad_mask >> i) & 1 ? 0 : 8);
    }
}
void H_osPfsInit(Ctx* c) { RET(1); }       // no Controller Pak
void H_osPfsIsPlug(Ctx* c) { MEM_SB(A1, 0); RET(0); }

// ---------------------------------------------------------------- EEPROM (4 Kbit, 64 blocks of 8 bytes)
uint8_t eeprom[512]; uint32_t eeprom_dirty;
void H_osEepromProbe(Ctx* c) { RET(1); }
void H_osEepromLongRead(Ctx* c) {
    uint32_t blk = A1 & 0xFF, buf = A2, n = A3;
    for (uint32_t i = 0; i < n && blk * 8 + i < 512; i++) MEM_SB(buf + i, eeprom[blk * 8 + i]);
    RET(0);
}
void H_osEepromLongWrite(Ctx* c) {
    uint32_t blk = A1 & 0xFF, buf = A2, n = A3;
    for (uint32_t i = 0; i < n && blk * 8 + i < 512; i++) eeprom[blk * 8 + i] = MEM_BU(buf + i);
    eeprom_dirty++;
    RET(0);
}

// ---------------------------------------------------------------- audio output
uint32_t ai_freq = 32000, ai_buf, ai_len, ai_written;
void H_osAiSetFrequency(Ctx* c) { ai_freq = A0; RET(A0); }
void H_osAiSetNextBuffer(Ctx* c) { ai_buf = A0; ai_len = A1; ai_written++; RET(0); }
void H_osAiGetLength(Ctx* c) { RET(0); }

// ---------------------------------------------------------------- status for the host
uint32_t rt_stat(int i) {
    switch (i) {
        case 0: return gfx_tasks;
        case 1: return audio_tasks;
        case 2: return vi_frames;
        case 3: return vi_cur_fb;
        case 4: return (uint32_t)running;
        case 5: return nslots;
        case 6: return ai_written;
    }
    return 0;
}
uint32_t rt_slot_info(int s, int f) {
    switch (f) { case 0: return T[s].thread; case 1: return T[s].pri; case 2: return T[s].state; case 3: return T[s].waitq; case 4: return T[s].entry; }
    return 0;
}
