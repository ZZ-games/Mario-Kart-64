// High-level emulation of the N64 RSP (F3DEX microcode) and RDP.
// Walks a display list, transforms and lights vertices, emulates texture memory
// (TMEM) loads and decodes textures, and emits draw batches + vertices into a
// shared buffer that the browser draws with WebGL.
#include "rt.h"

// GBI1 / F3DEX opcodes
enum {
    G_SPNOOP = 0x00, G_MTX = 0x01, G_MOVEMEM = 0x03, G_VTX = 0x04, G_DL = 0x06,
    G_LOAD_UCODE = 0xAF, G_BRANCH_Z = 0xB0, G_TRI2 = 0xB1, G_MODIFYVTX = 0xB2, G_RDPHALF_2 = 0xB3, G_RDPHALF_1 = 0xB4,
    G_QUAD = 0xB5, G_CLEARGEOMETRYMODE = 0xB6, G_SETGEOMETRYMODE = 0xB7, G_ENDDL = 0xB8, G_SETOTHERMODE_L = 0xB9,
    G_SETOTHERMODE_H = 0xBA, G_TEXTURE = 0xBB, G_MOVEWORD = 0xBC, G_POPMTX = 0xBD, G_CULLDL = 0xBE, G_TRI1 = 0xBF,
    G_TEXRECT = 0xE4, G_TEXRECTFLIP = 0xE5, G_SETSCISSOR = 0xED, G_SETPRIMDEPTH = 0xEE, G_RDPSETOTHERMODE = 0xEF,
    G_LOADTLUT = 0xF0, G_SETTILESIZE = 0xF2, G_LOADBLOCK = 0xF3, G_LOADTILE = 0xF4, G_SETTILE = 0xF5,
    G_FILLRECT = 0xF6, G_SETFILLCOLOR = 0xF7, G_SETFOGCOLOR = 0xF8, G_SETBLENDCOLOR = 0xF9, G_SETPRIMCOLOR = 0xFA,
    G_SETENVCOLOR = 0xFB, G_SETCOMBINE = 0xFC, G_SETTIMG = 0xFD, G_SETZIMG = 0xFE, G_SETCIMG = 0xFF
};
#define GM_ZBUFFER 0x1
#define GM_SHADE 0x4
#define GM_CULL_FRONT 0x1000
#define GM_CULL_BACK 0x2000
#define GM_FOG 0x10000
#define GM_LIGHTING 0x20000
#define GM_TEXGEN 0x40000
#define GM_TEXGEN_LINEAR 0x80000

// ---------------------------------------------------------------- output (shared with the host)
#define MAXV 240000
#define MAXB 6000
#define BI 32
float gfx_verts[2][MAXV * 12];
int32_t gfx_batches[2][MAXB * BI];
int32_t gfx_nv[2], gfx_nb[2];
volatile int32_t gfx_ready = -1, gfx_frame;
static int cur_out;
float* rt_gfx_verts(int i) { return gfx_verts[i]; }
int32_t* rt_gfx_batches(int i) { return gfx_batches[i]; }
int32_t rt_gfx_info(int i) { return i == 0 ? gfx_ready : i == 1 ? gfx_frame : i == 2 ? gfx_nv[gfx_ready & 1] : gfx_nb[gfx_ready & 1]; }

// texture pool
#define POOL_TEXELS (6 * 1024 * 1024)
#define MAXTEX 8192
uint32_t tex_pool[POOL_TEXELS];
int32_t tex_info[MAXTEX * 3];   // offset, w, h
int32_t tex_gen;
static int tex_count, pool_used;
static uint32_t hash_keys[16384]; static int32_t hash_ids[16384];
int32_t* rt_tex_info(void) { return tex_info; }
uint32_t* rt_tex_pool(void) { return tex_pool; }

// ---------------------------------------------------------------- state
typedef struct { float x, y, z, w; float r, g, b, a; float s, t; uint8_t clip; } V;
static V vbuf[64];
static float mv_stack[11][4][4], proj[4][4], mvp[4][4];
static int mv_top;
static uint32_t seg[16];
static uint32_t geom, oh, ol, combine_hi, combine_lo;
static uint32_t prim_color, env_color, fog_color, fill_color, blend_color;
static uint32_t prim_lod;
static float tex_sc_s, tex_sc_t; static int tex_on, tex_tile;
static float vp_scale[3], vp_trans[3];
static int scissor[4];
static int num_lights; static uint8_t light_col[8][3]; static int8_t light_dir[8][3];
static float light_model[8][3]; static int lights_valid;
static int8_t lookat_dir[2][3] = {{127, 0, 0}, {0, 127, 0}}; static float lookat_model[2][3];
static int16_t fog_mul, fog_off;
static uint32_t cimg, zimg, timg_addr; static int timg_fmt, timg_siz, timg_w;
static uint32_t rdphalf1, rdphalf2;
typedef struct { int fmt, siz, line, tmem, pal, cmt, maskt, shiftt, cms, masks, shifts; int uls, ult, lrs, lrt; } Tile;
static Tile tiles[8];
static uint8_t tmem[4096];
static uint16_t tlut[256];

static inline uint32_t seg_addr(uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0x00FFFFFF)) | 0x80000000u; }

// ---------------------------------------------------------------- matrices
static void mtx_load(float m[4][4], uint32_t a) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        int16_t hi = MEM_H(a + (i * 4 + j) * 2);
        uint16_t lo = MEM_HU(a + 32 + (i * 4 + j) * 2);
        m[i][j] = (float)((int32_t)((hi << 16) | lo)) / 65536.0f;
    }
}
static void mtx_mul(float r[4][4], float a[4][4], float b[4][4]) {
    float t[4][4];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++)
        t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(r, t, sizeof(t));
}
static void update_mvp(void) { mtx_mul(mvp, mv_stack[mv_top], proj); lights_valid = 0; }

// ---------------------------------------------------------------- textures
static int tex_hash_lookup(uint32_t key) {
    uint32_t h = (key * 2654435761u) >> 18;
    for (int i = 0; i < 16384; i++) {
        uint32_t k = (h + i) & 16383;
        if (hash_ids[k] == 0) return -(int)k - 1;
        if (hash_keys[k] == key) return hash_ids[k] - 1;
    }
    return -1 - 0;
}
static void tex_reset(void) {
    tex_count = 0; pool_used = 0; tex_gen++;
    for (int i = 0; i < 16384; i++) hash_ids[i] = 0;
}
static inline uint8_t tm(uint32_t addr, int row) { return tmem[((addr) ^ ((row & 1) ? 4 : 0)) & 4095]; }
static inline uint32_t c16(uint16_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31, a = c & 1;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | (a ? 0xFF000000u : 0);
}
static inline uint32_t ia16(uint16_t c) { uint32_t i = c >> 8, a = c & 0xFF; return i | (i << 8) | (i << 16) | (a << 24); }

// decode `tile` from TMEM into the pool; returns texture id and its size
static int tile_texture(int t, int* ow, int* oh_) {
    Tile* T = &tiles[t];
    int w = ((T->lrs - T->uls) >> 2) + 1, h = ((T->lrt - T->ult) >> 2) + 1;
    if (T->masks && (1 << T->masks) < w) w = 1 << T->masks;
    if (T->maskt && (1 << T->maskt) < h) h = 1 << T->maskt;
    if (w <= 0) w = 1; if (h <= 0) h = 1;
    if (w > 1024) w = 1024; if (h > 1024) h = 1024;
    *ow = w; *oh_ = h;
    uint32_t base = T->tmem * 8, line = T->line * 8;
    int tlut_mode = (oh >> 14) & 3;  // 2 = RGBA16, 3 = IA16
    // hash the TMEM bytes this tile reads + its format
    uint32_t hsh = 2166136261u ^ (uint32_t)(T->fmt * 977 + T->siz * 131 + w * 7919 + h * 104729 + T->pal * 31 + tlut_mode);
    uint32_t span = line * h; if (span > 4096) span = 4096;
    if (T->siz == 3) span *= 2;
    for (uint32_t i = 0; i < span && i < 4096; i += 4) {
        uint32_t a = (base + i) & 4095;
        hsh = (hsh ^ (tmem[a] | (tmem[a + 1] << 8) | (tmem[(a + 2) & 4095] << 16) | (tmem[(a + 3) & 4095] << 24))) * 16777619u;
    }
    if (T->fmt == 2) for (int i = 0; i < (T->siz == 0 ? 16 : 256); i++) hsh = (hsh ^ tlut[(T->siz == 0 ? T->pal * 16 : 0) + i]) * 16777619u;
    int found = tex_hash_lookup(hsh);
    if (found >= 0) return found;
    if (tex_count >= MAXTEX || pool_used + w * h > POOL_TEXELS) { tex_reset(); found = tex_hash_lookup(hsh); }
    int id = tex_count++;
    uint32_t k = (uint32_t)(-found - 1);
    hash_keys[k] = hsh; hash_ids[k] = id + 1;
    uint32_t* out = tex_pool + pool_used;
    tex_info[id * 3] = pool_used; tex_info[id * 3 + 1] = w; tex_info[id * 3 + 2] = h;
    pool_used += w * h;
    for (int y = 0; y < h; y++) {
        uint32_t rowa = base + y * line;
        for (int x = 0; x < w; x++) {
            uint32_t col = 0xFFFF00FFu;
            switch (T->siz) {
                case 0: {  // 4-bit
                    uint8_t b = tm(rowa + (x >> 1), y); uint8_t v = (x & 1) ? (b & 15) : (b >> 4);
                    if (T->fmt == 2) { uint16_t e = tlut[(T->pal * 16 + v) & 255]; col = tlut_mode == 3 ? ia16(e) : c16(e); }
                    else if (T->fmt == 3) { uint32_t i = (v >> 1) * 255 / 7, a = (v & 1) ? 255 : 0; col = i | (i << 8) | (i << 16) | (a << 24); }
                    else { uint32_t i = v * 17; col = i | (i << 8) | (i << 16) | (i << 24); }
                    break;
                }
                case 1: {  // 8-bit
                    uint8_t v = tm(rowa + x, y);
                    if (T->fmt == 2) { uint16_t e = tlut[v]; col = tlut_mode == 3 ? ia16(e) : c16(e); }
                    else if (T->fmt == 3) { uint32_t i = (v >> 4) * 17, a = (v & 15) * 17; col = i | (i << 8) | (i << 16) | (a << 24); }
                    else col = v | (v << 8) | (v << 16) | ((uint32_t)v << 24);
                    break;
                }
                case 2: {  // 16-bit
                    uint16_t v = (tm(rowa + x * 2, y) << 8) | tm(rowa + x * 2 + 1, y);
                    if (T->fmt == 3) col = ia16(v);
                    else if (T->fmt == 2) { uint16_t e = tlut[v & 255]; col = tlut_mode == 3 ? ia16(e) : c16(e); }
                    else col = c16(v);
                    break;
                }
                case 3: {  // 32-bit (stored linearly by our loader)
                    uint32_t a = rowa * 2 + x * 4;
                    col = tm(a, y) | (tm(a + 1, y) << 8) | (tm(a + 2, y) << 16) | ((uint32_t)tm(a + 3, y) << 24);
                    break;
                }
            }
            out[y * w + x] = col;
        }
    }
    return id;
}

static void load_block(int t, int uls, int ult, int lrs, int dxt) {
    Tile* T = &tiles[t];
    int bpt_shift = timg_siz;  // bytes per texel = (1 << siz) / 2
    uint32_t n = lrs - uls + 1;
    uint32_t bytes = timg_siz == 0 ? (n + 1) / 2 : n << (timg_siz - 1);
    uint32_t src = timg_addr + ((ult * timg_w + uls) << bpt_shift >> 1);
    uint32_t dst = T->tmem * 8;
    if (timg_siz == 3) bytes = n * 4;
    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t word = i >> 3;
        int odd = dxt ? (((word * dxt) >> 11) & 1) : 0;
        tmem[((dst + i) ^ (odd ? 4 : 0)) & 4095] = MEM_BU(src + i);
    }
}
static void load_tile(int t, int uls, int ult, int lrs, int lrt) {
    Tile* T = &tiles[t];
    int x0 = uls >> 2, y0 = ult >> 2, x1 = lrs >> 2, y1 = lrt >> 2;
    uint32_t rowbytes = timg_siz == 0 ? (x1 - x0 + 2) / 2 : (uint32_t)(x1 - x0 + 1) << (timg_siz - 1);
    uint32_t line = T->line * 8;
    if (timg_siz == 3) { rowbytes = (x1 - x0 + 1) * 4; line *= 2; }
    for (int y = y0; y <= y1; y++) {
        uint32_t src = timg_addr + ((y * timg_w + x0) << timg_siz >> 1);
        if (timg_siz == 3) src = timg_addr + (y * timg_w + x0) * 4;
        uint32_t dst = T->tmem * 8 + (y - y0) * line;
        for (uint32_t i = 0; i < rowbytes; i++)
            tmem[((dst + i) ^ (((y - y0) & 1) ? 4 : 0)) & 4095] = MEM_BU(src + i);
    }
}
static void load_tlut(int t, int uls, int lrs) {
    Tile* T = &tiles[t];
    int start = T->tmem - 256, n = ((lrs - uls) >> 2) + 1;
    uint32_t src = timg_addr + (uls >> 2) * 2;
    for (int i = 0; i < n && start + i < 256; i++) if (start + i >= 0) tlut[start + i] = MEM_HU(src + i * 2);
}

// ---------------------------------------------------------------- batches
static int nv, nb;
static int32_t* B;  // current batch
static float* VO;
static int32_t key[BI];

static void begin_batch(int rect, int tex0, int tex1, int fl0, int fl1) {
    int32_t k[BI] = {0};
    k[2] = tex0; k[3] = tex1; k[4] = fl0; k[5] = fl1;
    k[6] = combine_hi; k[7] = combine_lo; k[8] = oh; k[9] = ol; k[10] = geom;
    k[11] = prim_color; k[12] = env_color; k[13] = fog_color;
    float sx = vp_scale[0], sy = vp_scale[1];
    if (rect) { k[14] = 0; k[15] = 0; k[16] = 320; k[17] = 240; }
    else { k[14] = (int)(vp_trans[0] - sx); k[15] = (int)(vp_trans[1] - sy); k[16] = (int)(2 * sx); k[17] = (int)(2 * sy); }
    k[18] = scissor[0]; k[19] = scissor[1]; k[20] = scissor[2]; k[21] = scissor[3];
    k[22] = rect; k[23] = blend_color; k[24] = prim_lod; k[25] = fill_color;
    if (nb > 0 && B) {
        int same = 1;
        for (int i = 2; i < BI; i++) if (B[i] != k[i]) { same = 0; break; }
        if (same) return;
    }
    if (nb >= MAXB) return;
    B = &gfx_batches[cur_out][nb * BI];
    for (int i = 0; i < BI; i++) B[i] = k[i];
    B[0] = nv; B[1] = 0;
    nb++;
}
static void special_batch(int kind) {  // 1 = clear depth
    if (nb >= MAXB) return;
    B = &gfx_batches[cur_out][nb * BI];
    for (int i = 0; i < BI; i++) B[i] = 0;
    B[0] = nv; B[1] = 0; B[22] = 16 + kind;
    nb++;
}
static inline void put_vert(float x, float y, float z, float w, float r, float g, float b, float a, float u0, float v0, float u1, float v1) {
    if (nv >= MAXV) return;
    float* o = VO + nv * 12;
    o[0] = x; o[1] = y; o[2] = z; o[3] = w; o[4] = r; o[5] = g; o[6] = b; o[7] = a; o[8] = u0; o[9] = v0; o[10] = u1; o[11] = v1;
    nv++; B[1]++;
}

static void tex_params(int t, int* id, int* flags, float* sw, float* sh, float* offs, float* offt, float* mul_s, float* mul_t) {
    int w, h;
    *id = tile_texture(t, &w, &h);
    Tile* T = &tiles[t];
    int filt = ((oh >> 12) & 3) == 2;  // bilinear
    int cms = (T->cms & 2) ? 2 : (T->cms & 1) ? 1 : 0, cmt = (T->cmt & 2) ? 2 : (T->cmt & 1) ? 1 : 0;
    if (!T->masks) cms = 2;
    if (!T->maskt) cmt = 2;
    *flags = cms | (cmt << 2) | (filt << 4);
    *sw = (float)w; *sh = (float)h;
    *offs = T->uls / 4.0f; *offt = T->ult / 4.0f;
    *mul_s = T->shifts <= 10 ? 1.0f / (1 << T->shifts) : (float)(1 << (16 - T->shifts));
    *mul_t = T->shiftt <= 10 ? 1.0f / (1 << T->shiftt) : (float)(1 << (16 - T->shiftt));
}

static int uses_texel1(void) {
    uint32_t h = combine_hi, l = combine_lo;
    int sel[16] = { (h >> 20) & 15, (l >> 28) & 15, (h >> 15) & 31, (l >> 15) & 7,   // cycle 1 color a b c d
                    (h >> 12) & 7, (l >> 12) & 7, (h >> 9) & 7, (l >> 9) & 7,        // cycle 1 alpha a b c d
                    (h >> 5) & 15, (l >> 24) & 15, h & 31, (l >> 6) & 7,             // cycle 2 color
                    (l >> 21) & 7, (l >> 3) & 7, (l >> 18) & 7, l & 7 };             // cycle 2 alpha
    int two = ((oh >> 20) & 3) == 1;
    for (int i = 0; i < (two ? 16 : 8); i++) if (sel[i] == 2) return 1;
    return 0;
}

static void draw_tri(int i0, int i1, int i2) {
    V* v[3] = { &vbuf[i0], &vbuf[i1], &vbuf[i2] };
    if (v[0]->clip & v[1]->clip & v[2]->clip) return;
    if (geom & (GM_CULL_FRONT | GM_CULL_BACK)) {
        float dx1 = v[1]->x / v[1]->w - v[0]->x / v[0]->w, dy1 = v[1]->y / v[1]->w - v[0]->y / v[0]->w;
        float dx2 = v[2]->x / v[2]->w - v[0]->x / v[0]->w, dy2 = v[2]->y / v[2]->w - v[0]->y / v[0]->w;
        float cross = dx1 * dy2 - dy1 * dx2;
        if ((v[0]->w < 0) ^ (v[1]->w < 0) ^ (v[2]->w < 0)) cross = -cross;
        if ((geom & GM_CULL_BACK) && cross <= 0) return;
        if ((geom & GM_CULL_FRONT) && cross >= 0) return;
    }
    int id0 = -1, id1 = -1, f0 = 0, f1 = 0;
    float w0 = 1, h0 = 1, os0 = 0, ot0 = 0, ms0 = 1, mt0 = 1, w1 = 1, h1 = 1, os1 = 0, ot1 = 0, ms1 = 1, mt1 = 1;
    if (tex_on) {
        tex_params(tex_tile, &id0, &f0, &w0, &h0, &os0, &ot0, &ms0, &mt0);
        if (uses_texel1()) tex_params((tex_tile + 1) & 7, &id1, &f1, &w1, &h1, &os1, &ot1, &ms1, &mt1);
    }
    begin_batch(0, id0, id1, f0, f1);
    for (int k = 0; k < 3; k++) {
        V* p = v[k];
        float s = p->s, t = p->t;  // texel units before shift
        put_vert(p->x, p->y, p->z, p->w, p->r, p->g, p->b, p->a,
                 (s * ms0 - os0) / w0, (t * mt0 - ot0) / h0, (s * ms1 - os1) / w1, (t * mt1 - ot1) / h1);
    }
}

static void calc_lights(void) {
    for (int l = 0; l < num_lights; l++) {
        float d[3] = { light_dir[l][0] / 127.0f, light_dir[l][1] / 127.0f, light_dir[l][2] / 127.0f };
        float (*m)[4] = mv_stack[mv_top];
        float x = d[0] * m[0][0] + d[1] * m[0][1] + d[2] * m[0][2];
        float y = d[0] * m[1][0] + d[1] * m[1][1] + d[2] * m[1][2];
        float z = d[0] * m[2][0] + d[1] * m[2][1] + d[2] * m[2][2];
        float len = __builtin_sqrtf(x * x + y * y + z * z); if (len == 0) len = 1;
        light_model[l][0] = x / len; light_model[l][1] = y / len; light_model[l][2] = z / len;
    }
    for (int l = 0; l < 2; l++) {
        float d[3] = { lookat_dir[l][0] / 127.0f, lookat_dir[l][1] / 127.0f, lookat_dir[l][2] / 127.0f };
        float (*m)[4] = mv_stack[mv_top];
        float x = d[0] * m[0][0] + d[1] * m[0][1] + d[2] * m[0][2];
        float y = d[0] * m[1][0] + d[1] * m[1][1] + d[2] * m[1][2];
        float z = d[0] * m[2][0] + d[1] * m[2][1] + d[2] * m[2][2];
        float len = __builtin_sqrtf(x * x + y * y + z * z); if (len == 0) len = 1;
        lookat_model[l][0] = x / len; lookat_model[l][1] = y / len; lookat_model[l][2] = z / len;
    }
    lights_valid = 1;
}

static void load_vertices(uint32_t a, int n, int v0) {
    for (int i = 0; i < n && v0 + i < 64; i++) {
        uint32_t p = a + i * 16;
        float x = MEM_H(p), y = MEM_H(p + 2), z = MEM_H(p + 4);
        int16_t s = MEM_H(p + 8), t = MEM_H(p + 10);
        uint8_t cr = MEM_BU(p + 12), cg = MEM_BU(p + 13), cb = MEM_BU(p + 14), ca = MEM_BU(p + 15);
        V* o = &vbuf[v0 + i];
        o->x = x * mvp[0][0] + y * mvp[1][0] + z * mvp[2][0] + mvp[3][0];
        o->y = x * mvp[0][1] + y * mvp[1][1] + z * mvp[2][1] + mvp[3][1];
        o->z = x * mvp[0][2] + y * mvp[1][2] + z * mvp[2][2] + mvp[3][2];
        o->w = x * mvp[0][3] + y * mvp[1][3] + z * mvp[2][3] + mvp[3][3];
        float r, g, b;
        if (geom & GM_LIGHTING) {
            if (!lights_valid) calc_lights();
            float nx = (int8_t)cr / 127.0f, ny = (int8_t)cg / 127.0f, nz = (int8_t)cb / 127.0f;
            r = light_col[num_lights][0]; g = light_col[num_lights][1]; b = light_col[num_lights][2];
            for (int l = 0; l < num_lights; l++) {
                float d = nx * light_model[l][0] + ny * light_model[l][1] + nz * light_model[l][2];
                if (d > 0) { r += d * light_col[l][0]; g += d * light_col[l][1]; b += d * light_col[l][2]; }
            }
            if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
            if (geom & GM_TEXGEN) {  // reflection mapping: RSP dots the normal with the look-at axes
                float dx = nx * lookat_model[0][0] + ny * lookat_model[0][1] + nz * lookat_model[0][2];
                float dy = nx * lookat_model[1][0] + ny * lookat_model[1][1] + nz * lookat_model[1][2];
                o->s = (dx * 0.5f + 0.5f) * tex_sc_s * 65536.0f / 32.0f;
                o->t = (dy * 0.5f + 0.5f) * tex_sc_t * 65536.0f / 32.0f;
            }
        } else { r = cr; g = cg; b = cb; }
        if (!(geom & GM_TEXGEN) || !(geom & GM_LIGHTING)) {
            o->s = s * tex_sc_s / 32.0f;
            o->t = t * tex_sc_t / 32.0f;
        }
        float a_ = ca;
        if (geom & GM_FOG) {
            float w = o->w != 0 ? o->w : 0.001f;
            float f = (o->z / w) * fog_mul + fog_off;
            if (f < 0) f = 0; if (f > 255) f = 255;
            a_ = f;
        }
        o->r = r / 255.0f; o->g = g / 255.0f; o->b = b / 255.0f; o->a = a_ / 255.0f;
        uint8_t cl = 0;
        if (o->x < -o->w) cl |= 1; if (o->x > o->w) cl |= 2;
        if (o->y < -o->w) cl |= 4; if (o->y > o->w) cl |= 8;
        if (o->w < 0.001f) cl |= 16;
        o->clip = cl;
    }
}

static void tex_rect(uint32_t w0, uint32_t w1, int flip) {
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
    int tile = (w1 >> 24) & 7;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
    int16_t s = rdphalf1 >> 16, t = rdphalf1 & 0xFFFF;
    float dsdx = (int16_t)(rdphalf2 >> 16) / 1024.0f, dtdy = (int16_t)(rdphalf2 & 0xFFFF) / 1024.0f;
    int cyc = (oh >> 20) & 3;
    if (cyc == 2) { dsdx /= 4; lrx += 1; lry += 1; }  // copy mode
    else if (cyc == 3) { lrx += 1; lry += 1; }
    int id, f; float w, h, os, ot, ms, mt;
    tex_params(tile, &id, &f, &w, &h, &os, &ot, &ms, &mt);
    int id1 = -1, f1 = 0; float tw1 = 1, h1 = 1, os1 = 0, ot1 = 0, ms1 = 1, mt1 = 1;
    if (uses_texel1() && cyc < 2) tex_params((tile + 1) & 7, &id1, &f1, &tw1, &h1, &os1, &ot1, &ms1, &mt1);
    begin_batch(1, id, id1, f, f1);
    float s0 = s / 32.0f, t0 = t / 32.0f;
    float s1 = s0 + dsdx * (lrx - ulx), t1 = t0 + dtdy * (lry - uly);
    float X0 = ulx / 160.0f - 1, X1 = lrx / 160.0f - 1, Y0 = 1 - uly / 120.0f, Y1 = 1 - lry / 120.0f;
    float U0 = (s0 * ms - os) / w, U1 = (s1 * ms - os) / w, V0 = (t0 * mt - ot) / h, V1 = (t1 * mt - ot) / h;
    float u0b = (s0 * ms1 - os1) / tw1, u1b = (s1 * ms1 - os1) / tw1, v0b = (t0 * mt1 - ot1) / h1, v1b = (t1 * mt1 - ot1) / h1;
    float c[4] = { 1, 1, 1, 1 };
    #define PV(X, Y, U, Vv, U2, V2) put_vert(X, Y, 0, 1, c[0], c[1], c[2], c[3], U, Vv, U2, V2)
    if (!flip) {
        PV(X0, Y0, U0, V0, u0b, v0b); PV(X1, Y0, U1, V0, u1b, v0b); PV(X0, Y1, U0, V1, u0b, v1b);
        PV(X1, Y0, U1, V0, u1b, v0b); PV(X1, Y1, U1, V1, u1b, v1b); PV(X0, Y1, U0, V1, u0b, v1b);
    } else {
        PV(X0, Y0, U0, V0, u0b, v0b); PV(X1, Y0, U0, V1, u0b, v1b); PV(X0, Y1, U1, V0, u1b, v0b);
        PV(X1, Y0, U0, V1, u0b, v1b); PV(X1, Y1, U1, V1, u1b, v1b); PV(X0, Y1, U1, V0, u1b, v0b);
    }
}

static void fill_rect(uint32_t w0, uint32_t w1) {
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
    int cyc = (oh >> 20) & 3;
    if (cimg == zimg) { special_batch(1); return; }  // depth buffer clear
    float r, g, b, a;
    if (cyc == 3) {
        uint16_t c = fill_color >> 16; uint32_t cc = c16(c);
        r = (cc & 255) / 255.0f; g = ((cc >> 8) & 255) / 255.0f; b = ((cc >> 16) & 255) / 255.0f; a = 1;
        lrx += 1; lry += 1;
    } else {
        r = g = b = a = 1;  // colors come from the combiner (primitive color)
    }
    begin_batch(cyc == 3 ? 2 : 1, -1, -1, 0, 0);
    float X0 = ulx / 160.0f - 1, X1 = lrx / 160.0f - 1, Y0 = 1 - uly / 120.0f, Y1 = 1 - lry / 120.0f;
    put_vert(X0, Y0, 0, 1, r, g, b, a, 0, 0, 0, 0); put_vert(X1, Y0, 0, 1, r, g, b, a, 0, 0, 0, 0); put_vert(X0, Y1, 0, 1, r, g, b, a, 0, 0, 0, 0);
    put_vert(X1, Y0, 0, 1, r, g, b, a, 0, 0, 0, 0); put_vert(X1, Y1, 0, 1, r, g, b, a, 0, 0, 0, 0); put_vert(X0, Y1, 0, 1, r, g, b, a, 0, 0, 0, 0);
}

// ---------------------------------------------------------------- display list interpreter
uint32_t gfx_unknown_ops; uint32_t gfx_unknown_hist[256];
uint32_t* rt_gfx_unknown(void) { return gfx_unknown_hist; }
static void run_dl(uint32_t dl) {
    uint32_t stack[18]; int sp = 0;
    uint32_t pc = dl;
    for (int guard = 0; guard < 2000000; guard++) {
        uint32_t w0 = (uint32_t)MEM_W(pc), w1 = (uint32_t)MEM_W(pc + 4);
        pc += 8;
        uint32_t op = w0 >> 24;
        switch (op) {
            case G_SPNOOP: case 0xC0: case 0xE6: case 0xE7: case 0xE8: case 0xE9: case G_SETPRIMDEPTH: case G_LOAD_UCODE: break;
            case G_MTX: {
                int p = (w0 >> 16) & 0xFF;
                float m[4][4]; mtx_load(m, seg_addr(w1));
                if (p & 1) {  // projection
                    if (p & 2) memcpy(proj, m, sizeof(m)); else mtx_mul(proj, m, proj);
                } else {
                    if ((p & 4) && mv_top < 10) { memcpy(mv_stack[mv_top + 1], mv_stack[mv_top], sizeof(m)); mv_top++; }
                    if (p & 2) memcpy(mv_stack[mv_top], m, sizeof(m)); else mtx_mul(mv_stack[mv_top], m, mv_stack[mv_top]);
                }
                update_mvp();
                break;
            }
            case G_POPMTX: if (mv_top > 0) mv_top--; update_mvp(); break;
            case G_MOVEMEM: {
                int idx = (w0 >> 16) & 0xFF; uint32_t a = seg_addr(w1);
                if (idx == 0x80) {
                    for (int i = 0; i < 3; i++) { vp_scale[i] = MEM_H(a + i * 2) / 4.0f; vp_trans[i] = MEM_H(a + 8 + i * 2) / 4.0f; }
                } else if (idx == 0x82 || idx == 0x84) {
                    int l = idx == 0x84 ? 0 : 1;
                    for (int i = 0; i < 3; i++) lookat_dir[l][i] = MEM_B(a + 8 + i);
                    lights_valid = 0;
                } else if (idx >= 0x86 && idx <= 0x94) {
                    int l = (idx - 0x86) / 2;
                    for (int i = 0; i < 3; i++) { light_col[l][i] = MEM_BU(a + i); light_dir[l][i] = MEM_B(a + 8 + i); }
                    lights_valid = 0;
                }
                break;
            }
            case G_MOVEWORD: {
                int idx = w0 & 0xFF, off = (w0 >> 8) & 0xFFFF;
                if (idx == 6) seg[(off >> 2) & 15] = w1 & 0x1FFFFFFF;
                else if (idx == 2) { num_lights = ((w1 - 0x80000000u) >> 5) - 1; if (num_lights < 0) num_lights = 0; if (num_lights > 7) num_lights = 7; lights_valid = 0; }
                else if (idx == 8) { fog_mul = w1 >> 16; fog_off = w1 & 0xFFFF; }
                break;
            }
            case G_VTX: {
                int n = (w0 >> 10) & 0x3F, v0 = ((w0 >> 16) & 0xFF) / 2;
                load_vertices(seg_addr(w1), n, v0);
                break;
            }
            case G_MODIFYVTX: break;
            case G_TRI1: draw_tri(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2); break;
            case G_TRI2:
                draw_tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                draw_tri(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                break;
            case G_QUAD: {
                int a = ((w1 >> 16) & 0xFF) / 2, b = ((w1 >> 8) & 0xFF) / 2, c2 = (w1 & 0xFF) / 2, d = ((w1 >> 24) & 0xFF) / 2;
                draw_tri(a, b, c2); draw_tri(a, c2, d);
                break;
            }
            case G_CULLDL: {
                int a = ((w0 & 0xFFFF) / 2) & 63, b = ((w1 & 0xFFFF) / 2) & 63;
                uint8_t all = 0x1F;
                for (int i = a; i <= b; i++) all &= vbuf[i].clip;
                if (all) { if (sp == 0) return; pc = stack[--sp]; }
                break;
            }
            case G_BRANCH_Z: break;  // never taken (no depth test emulation here)
            case G_DL:
                if (((w0 >> 16) & 0xFF) == 0) { if (sp < 18) stack[sp++] = pc; }
                pc = seg_addr(w1);
                break;
            case G_ENDDL: if (sp == 0) return; pc = stack[--sp]; break;
            case G_SETGEOMETRYMODE: geom |= w1; break;
            case G_CLEARGEOMETRYMODE: geom &= ~w1; break;
            case G_SETOTHERMODE_H: case G_SETOTHERMODE_L: {
                int sft = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
                uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << sft;
                if (op == G_SETOTHERMODE_H) oh = (oh & ~mask) | (w1 & mask); else ol = (ol & ~mask) | (w1 & mask);
                break;
            }
            case G_RDPSETOTHERMODE: oh = w0 & 0x00FFFFFF; ol = w1; break;
            case G_TEXTURE:
                tex_sc_s = (w1 >> 16) / 65536.0f; tex_sc_t = (w1 & 0xFFFF) / 65536.0f;
                tex_tile = (w0 >> 8) & 7; tex_on = w0 & 0xFF;
                if ((w1 >> 16) == 0xFFFF) tex_sc_s = 1.0f;  // 0xFFFF means 1.0
                if ((w1 & 0xFFFF) == 0xFFFF) tex_sc_t = 1.0f;
                break;
            case G_RDPHALF_1: rdphalf1 = w1; break;
            case G_RDPHALF_2: rdphalf2 = w1; break;
            case G_TEXRECT: case G_TEXRECTFLIP: {
                // followed by RDPHALF_1 (s,t) and RDPHALF_2 (dsdx,dtdy)
                uint32_t a0 = (uint32_t)MEM_W(pc), a1 = (uint32_t)MEM_W(pc + 4);
                uint32_t b0 = (uint32_t)MEM_W(pc + 8), b1 = (uint32_t)MEM_W(pc + 12);
                if ((a0 >> 24) == G_RDPHALF_1 || (a0 >> 24) == G_RDPHALF_2) { rdphalf1 = a1; pc += 8; }
                if ((b0 >> 24) == G_RDPHALF_2 || (b0 >> 24) == 0xB2) { rdphalf2 = b1; pc += 8; }
                tex_rect(w0, w1, op == G_TEXRECTFLIP);
                break;
            }
            case G_SETSCISSOR:
                scissor[0] = ((w0 >> 12) & 0xFFF) >> 2; scissor[1] = (w0 & 0xFFF) >> 2;
                scissor[2] = ((w1 >> 12) & 0xFFF) >> 2; scissor[3] = (w1 & 0xFFF) >> 2;
                break;
            case G_SETTIMG: timg_fmt = (w0 >> 21) & 7; timg_siz = (w0 >> 19) & 3; timg_w = (w0 & 0xFFF) + 1; timg_addr = seg_addr(w1); break;
            case G_SETZIMG: zimg = seg_addr(w1) & 0x1FFFFFFF; break;
            case G_SETCIMG: cimg = seg_addr(w1) & 0x1FFFFFFF; break;
            case G_SETTILE: {
                Tile* T = &tiles[(w1 >> 24) & 7];
                T->fmt = (w0 >> 21) & 7; T->siz = (w0 >> 19) & 3; T->line = (w0 >> 9) & 0x1FF; T->tmem = w0 & 0x1FF;
                T->pal = (w1 >> 20) & 15; T->cmt = (w1 >> 18) & 3; T->maskt = (w1 >> 14) & 15; T->shiftt = (w1 >> 10) & 15;
                T->cms = (w1 >> 8) & 3; T->masks = (w1 >> 4) & 15; T->shifts = w1 & 15;
                break;
            }
            case G_SETTILESIZE: {
                Tile* T = &tiles[(w1 >> 24) & 7];
                T->uls = (w0 >> 12) & 0xFFF; T->ult = w0 & 0xFFF; T->lrs = (w1 >> 12) & 0xFFF; T->lrt = w1 & 0xFFF;
                break;
            }
            case G_LOADBLOCK: load_block((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF); break;
            case G_LOADTILE: {
                int t = (w1 >> 24) & 7;
                load_tile(t, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
                Tile* T = &tiles[t];
                T->uls = (w0 >> 12) & 0xFFF; T->ult = w0 & 0xFFF; T->lrs = (w1 >> 12) & 0xFFF; T->lrt = w1 & 0xFFF;
                break;
            }
            case G_LOADTLUT: load_tlut((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, (w1 >> 12) & 0xFFF); break;
            case G_FILLRECT: fill_rect(w0, w1); break;
            case G_SETFILLCOLOR: fill_color = w1; break;
            case G_SETFOGCOLOR: fog_color = w1; break;
            case G_SETBLENDCOLOR: blend_color = w1; break;
            case G_SETPRIMCOLOR: prim_color = w1; prim_lod = w0 & 0xFF; break;
            case G_SETENVCOLOR: env_color = w1; break;
            case G_SETCOMBINE: combine_hi = w0 & 0x00FFFFFF; combine_lo = w1; break;
            default: gfx_unknown_ops++; gfx_unknown_hist[op]++; break;
        }
    }
}

void gfx_run_task(uint32_t task) {
    uint32_t dl = (uint32_t)MEM_W(task + 0x30);
    cur_out = (gfx_ready + 1) & 1;
    VO = gfx_verts[cur_out]; nv = 0; nb = 0; B = 0;
    mv_top = 0; geom = 0;
    scissor[0] = 0; scissor[1] = 0; scissor[2] = 320; scissor[3] = 240;
    vp_scale[0] = 160; vp_scale[1] = 120; vp_trans[0] = 160; vp_trans[1] = 120;
    for (int i = 0; i < 16; i++) seg[i] = 0;
    run_dl(dl | 0x80000000u);
    gfx_nv[cur_out] = nv; gfx_nb[cur_out] = nb;
    __atomic_store_n(&gfx_ready, cur_out, __ATOMIC_SEQ_CST);
    gfx_frame++;
}
