/* sama: an instrument of light and repetition, for Opus 2 on the original OP-1.
 *
 * Strega's architecture, inverted. One oscillator into one shared delay; the delay's filter,
 * loss and feedback age the sound; the note's sub-harmonic warps the delay time; the loop
 * moves the pitch and the level of the source. Six voices sing into one room.
 *
 * T1 holds five faces; T3 and T4 turn to the previous/next one. T2 and T4 are the system's
 * own pages (envelope, LFO) — the LFO still modulates the four T1 knobs while we own the keys.
 *
 *   niche   fold, time, decay, filter        the instrument
 *   space   absorb, warp, drift, blend       how the room ages and moves
 *   cross   cross, polar, drive, tone        the loop against the source
 *   mod     rate, shape, depth, dest         the internal modulator
 *   core    core, shape, fuzz, grit          the oscillator itself
 */
#include "opus_plugin.h"
#include "op1fw.h"
#include "sama_art.h"
#include "sama_tab.h"

#define DMASK 65536u

enum { F_NICHE, F_SPACE, F_CROSS, F_MOD, F_CORE, F_N };

struct voice { unsigned ph, sub; int dc; };

static struct opl_self *me;
static short *line;
static unsigned widx;
static int tlen, tbase, wlp, ic1, ic2, dcz, hlp, alp, e1, e2;
static unsigned g_bph, g_subph, g_n = 0x1234u, g_seed = 22222u, g_sh;
static int g_shlast = 65535;
static volatile unsigned g_substep;
static volatile int lock_blocks;

static volatile int face = F_NICHE;
static short fv[F_N][4] = {
    { 18204, 20190, 29457, 17873 },   /* niche: fold 55, time 61 (249 ms), decay 92, filter 54 */
    {  6620, 13239,  9929, 23593 },   /* space: absorb 8, warp 40, drift 30, blend 78 */
    { 11587,  9929,  4966, 13239 },   /* cross: cross 35, polar 30, drive 10, tone 40 */
    { 14900,  7600, 19000,  9929 },   /* mod:   rate 36 (0.32 Hz), shape 15, depth 50, dest 30 */
    {  9929,  9929,     0,  3000 }    /* core:  triangle, shape 30, fuzz 0, grit 9 */
};

/* resolved once a block */
static volatile int R_fold = 55, R_time = 61, R_decay = 88, R_filter = 54;
static volatile int R_abs = 20, R_warp = 40, R_drift = 30, R_blend = 23593;
static volatile int R_cross = 35, R_polar = 30, R_drive = 15, R_tone = 40;
static volatile int R_rate = 45, R_shape = 15, R_depth = 60, R_dest = 30;
static volatile int R_core = 30, R_cshape = 30, R_fuzz = 0, R_grit = 9;
static volatile int g_breath = 16384, g_drift = 0, g_pitchmod = 0, g_xmod = 0;
static volatile int R_gate = 32767;

static int k99(short v) { return v < 0 ? 0 : (v * 99 + 16383) / 32767; }

/* soft-knee with a movable knee: drive colours the loop without adding gain */
static int softk(int x, int knee)
{
    if (knee < 6000) knee = 6000;
    if (x > knee) x = knee + ((x - knee) >> 2);
    if (x > 32767) x = 32767;
    if (x < -knee) x = -knee + ((x + knee) >> 2);
    if (x < -32768) x = -32768;
    return x;
}

static int soft(int x)
{
    if (x > 27000) x = 27000 + ((x - 27000) >> 2);
    if (x > 32767) x = 32767;
    if (x < -27000) x = -27000 + ((x + 27000) >> 2);
    if (x < -32768) x = -32768;
    return x;
}

/* ---------------------------------------------------------------- a voice */

static void v_init(void *v, int index)
{
    struct voice *s = v;
    s->ph = (unsigned)index * 0x11111111u;
    s->sub = 0;
    s->dc = 0;
}

static void v_render(void *v, short *out, const short *par, const unsigned *pitch)
{
    struct voice *s = v;
    (void)par;
    int fold = R_fold;
    int core = R_core;
    int cshape = R_cshape;
    int fuzz = R_fuzz;
    int grit = R_grit;
    int drift = (g_drift * R_drift) / 300;
    int pm = g_pitchmod;

    int fold_amt = fold * 32767 / 99;                      /* k99 -> Q15 */
    if (fold_amt < 0) fold_amt = 0; else if (fold_amt > 32767) fold_amt = 32767;
    int gq8 = 256 + (fold_amt * 1792) / 32767;            /* x256: 1 .. 8 folds */
    int drive = 32768 + fuzz * 660;                       /* the core's fuzz, 1 .. 3.3x */
    int width = 12000 + cshape * 210;                     /* pulse width, 12k .. 33k of 65536 */

    for (int n = 0; n < 128; ++n) {
        unsigned step = pitch[n];
        if (drift)
            step = (unsigned)((int)step + ((((int)(step >> 12)) * drift) >> 3));
        if (pm)
            step = (unsigned)((int)step + ((((int)(step >> 12)) * pm) >> 5));

        s->sub += step >> 1;
        {
            int sp = (int)(s->sub >> 16);
            int st = sp < 32768 ? sp * 2 - 32768 : 98304 - sp * 2;
            step = (unsigned)((int)step + ((((int)(step >> 12)) * st) >> 9));
        }
        s->ph += step;
        g_substep = step >> 1;

        int ph = (int)(s->ph >> 16);
        int tri = ph < 32768 ? ph * 2 - 32768 : 98304 - ph * 2;
        int x;

        if (core < 25) x = tri;
        else if (core < 50) x = SINTAB[(ph >> 8) & 255];
        else if (core < 75) x = ph - 32768;
        else x = ph < width ? 32767 : -32768;

        if (fuzz) x = soft((x * drive) >> 15);
        if (grit) {                                           /* grit: a new random value a sample */
            g_seed = g_seed * 1103515245u + 12345u;
            x += (((int)((g_seed >> 20) & 15) - 8) * grit * 4);
        }
        if (cshape != 50) {                                    /* asymmetric: shape the fold */
            x += (cshape - 50) * 120;
            x -= s->dc;
            s->dc += (x - s->dc) >> 10;
        }

        {   /* the sine folder: bend at low fold, wrap at high */
            int fph = (x * gq8) >> 8;
            int i0 = (fph >> 8) & 255;
            int fr = fph & 255;
            int s0 = SINTAB[i0], s1 = SINTAB[(i0 + 1) & 255];
            int f = s0 + (((s1 - s0) * fr) >> 8);
            x = x + (((f - x) * fold_amt) >> 15);
        }

        int y = (x * 26) >> 5;
        if (R_cross) y = (y * R_gate) >> 15;                  /* the loop ducks the source */
        out[n] = (short)soft(y);
    }
}

/* ---------------------------------------------------------------- the room */

static void b_block(short *buf)
{
    short par[8];
    if (!opus_engine_knobs(par)) return;
    if (lock_blocks) --lock_blocks;
    else for (int i = 0; i < 4; ++i) fv[face][i] = par[i];

    R_fold = k99(fv[F_NICHE][0]);
    R_time = k99(fv[F_NICHE][1]);
    R_decay = k99(fv[F_NICHE][2]);
    R_filter = k99(fv[F_NICHE][3]);

    R_abs = k99(fv[F_SPACE][0]);
    R_warp = k99(fv[F_SPACE][1]);
    R_drift = k99(fv[F_SPACE][2]);
    R_blend = fv[F_SPACE][3] < 0 ? 0 : fv[F_SPACE][3];

    R_cross = k99(fv[F_CROSS][0]);
    R_polar = k99(fv[F_CROSS][1]);
    R_drive = k99(fv[F_CROSS][2]);
    R_tone = k99(fv[F_CROSS][3]);

    R_rate = k99(fv[F_MOD][0]);
    R_shape = k99(fv[F_MOD][1]);
    R_depth = k99(fv[F_MOD][2]);
    R_dest = k99(fv[F_MOD][3]);

    R_core = k99(fv[F_CORE][0]);
    R_cshape = k99(fv[F_CORE][1]);
    R_fuzz = k99(fv[F_CORE][2]);
    R_grit = k99(fv[F_CORE][3]);

    if (!line) return;

    int target = TIMETAB[R_time];
    int fi = R_filter;
    int dec = 15000 + R_decay * 180;
    int abswc = ABSTAB[(R_abs * R_abs) / 99];
    int twc = TONETAB[R_tone];
    int wc = WLP[R_time];

    g_seed = g_seed * 1103515245u + 12345u;

    /* the modulator: a looping shape, steered to one of four destinations */
    g_bph += BRTTAB[R_rate];
    {
        int bh = (int)(g_bph >> 16);
        int tri = bh < 32768 ? bh * 2 - 32768 : 98304 - bh * 2;
        int raw;
        if (R_shape < 25) raw = tri + 32767;                       /* triangle */
        else if (R_shape < 50) raw = bh;                            /* rising ramp */
        else if (R_shape < 75) raw = (bh < 32768) ? 0 : 65535;      /* square */
        else {                                                      /* sample and hold */
            if (bh < g_shlast) g_sh = (g_seed >> 8) & 0xFFFF;
            g_shlast = bh;
            raw = g_sh;
        }
        g_breath = raw;
        g_pitchmod = 0;
        g_xmod = 0;
        {
            int m = ((raw - 32767) * R_depth) / 99;                 /* signed, scaled */
            int d = R_dest;
            if (d < 25) fi += (m * 28) >> 15;
            else if (d < 50) target += (int)(((long long)target * m) >> 20);
            else if (d < 75) g_pitchmod = (m * 26) >> 15;
            else g_xmod = (m * 24) >> 15;
        }
        if (fi < 0) fi = 0; else if (fi > 99) fi = 99;
    }

    /* the loop draws its own pitch and level: drift, and the cross gate */
    {
        int bus = g_drift < 0 ? -g_drift : g_drift;
        bus = (bus << 3) > 32767 ? 32767 : bus << 3;
        int depth = (R_cross * (50 - R_polar) * 3) / 99;
        int g2 = 32767 - ((bus * depth) >> 7);
        if (g2 < 4096) g2 = 4096; else if (g2 > 65535) g2 = 65535;
        if (g_xmod) {
            g2 += (g2 * g_xmod) >> 12;
            if (g2 < 2048) g2 = 2048; else if (g2 > 65535) g2 = 65535;
        }
        R_gate = g2;
    }

    int g = GTAB[fi], kq = 4700;
    int den = 4096 + ((g * (g + kq)) >> 12);
    int a1 = (4096 << 12) / den;
    int a2 = (g * a1) >> 12;
    int a3 = (g * a2) >> 12;
    if (a3 < 1 && g > 4) a3 = 1;

    int wd = (R_warp * 40) / 99;
    unsigned subph = g_subph;
    unsigned substep = g_substep >> 1;
    int lasty = 0;

    for (int n = 0; n < 64; ++n) {
        int src = buf[n];
        tbase += (target - tbase) >> 10;
        subph += substep;
        int tnow;
        {
            int sp = (int)(subph >> 16);
            int st = sp < 32768 ? sp * 2 - 32768 : 98304 - sp * 2;
            tnow = tbase + (int)((((long long)tbase * st) >> 31) * wd);
            if (tnow < 256) tnow = 256;
            if (tnow > 52920 * 256) tnow = 52920 * 256;
        }

        unsigned didx = (unsigned)(tnow >> 8);
        unsigned idx = (widx - didx) & (DMASK - 1);
        int fr = tnow & 255;
        int x = line[idx];
        int xb = line[(idx - 1) & (DMASK - 1)];
        int y = x + (((xb - x) * fr) >> 8);

        {
            int v3 = y - ic2;
            int acc1 = a1 * ic1 + a2 * v3 + e1;
            int acc2 = a2 * ic1 + a3 * v3 + e2;
            int v1 = acc1 >> 12;
            int v2 = ic2 + (acc2 >> 12);
            e1 = acc1 - (v1 << 12);
            e2 = acc2 - ((acc2 >> 12) << 12);
            ic1 = 2 * v1 - ic1;
            ic2 = 2 * v2 - ic2;
            if (ic1 > (1 << 18)) ic1 = 1 << 18; else if (ic1 < -(1 << 18)) ic1 = -(1 << 18);
            if (ic2 > (1 << 18)) ic2 = 1 << 18; else if (ic2 < -(1 << 18)) ic2 = -(1 << 18);
            y = v2;
        }

        dcz += (y - dcz) >> 9;
        y -= dcz;
        hlp += ((y - hlp) * twc) >> 15;
        y -= hlp;
        alp += ((y - alp) * abswc) >> 15;
        y = alp;

        {
            int w = src + ((y * dec) >> 15);
            w = softk(w, 27000 - R_drive * 190);   /* drive = a lower knee, not more gain */
            wlp += ((w - wlp) * wc) >> 15;
            int ws = soft(wlp);
            if (R_time > 60) ws += (int)((g_seed >> 20) & 3) - 1;
            line[widx] = (short)ws;
            widx = (widx + 1) & (DMASK - 1);
        }

        {
            int o = src + (((y - src) * R_blend) >> 15);
            o = (o * 48) >> 5;
            buf[n] = (short)soft(o);
            lasty = y;
        }
    }
    g_subph = subph;
    {
        int a = lasty < 0 ? -lasty : lasty;
        g_drift += ((a - g_drift) >> 5);
    }
}

/* ---------------------------------------------------------------- the pages */

static const char *const LAB[F_N][4] = {
    { "fold", "time", "decay", "filter" },
    { "absorb", "warp", "drift", "blend" },
    { "cross", "polar", "drive", "tone" },
    { "rate", "shape", "depth", "dest" },
    { "core", "shape", "fuzz", "grit" }
};
static const char *const FACE_NAME[F_N] = { "niche", "space", "cross", "mod", "core" };

#define C_GOLD   OPUS_RGB(0xd9b45e)
#define C_GOLD_D OPUS_RGB(0x6b5a33)
#define C_GOLD_L OPUS_RGB(0xf0d79a)
#define C_IVORY  OPUS_RGB(0xefe6d2)
#define C_TURQ   OPUS_RGB(0x3fa3a0)
#define C_LAPIS  OPUS_RGB(0x101b3c)
#define C_NIGHT  OPUS_RGB(0x0a1024)
#define C_DIM    OPUS_RGB(0x2a3350)

static void ln_c(int x0, int y0, int x1, int y1, int w, unsigned c)
{
    opus_page_stroke_line(x0 * 256, y0 * 256, x1 * 256, y1 * 256, w, c);
}

static void polyline_c(const short (*p)[2], int n, unsigned c)
{
    for (int i = 1; i < n; ++i) ln_c(p[i - 1][0], p[i - 1][1], p[i][0], p[i][1], 256, c);
}

static void ring_at(const short (*p)[2], int n, int cx, int cy, int sc, unsigned c)
{
    int px = cx + p[0][0] * sc / 1000, py = cy + p[0][1] * sc / 1000;
    for (int i = 1; i < n; ++i) {
        int x = cx + p[i][0] * sc / 1000, y = cy + p[i][1] * sc / 1000;
        ln_c(px, py, x, y, 256, c);
        px = x; py = y;
    }
    ln_c(px, py, cx + p[0][0] * sc / 1000, cy + p[0][1] * sc / 1000, 256, c);
}

static void bg_fill(unsigned rgb)
{
    opus_page_path();
    opus_page_move(0, 0);
    opus_page_line_to(320 * 256, 0);
    opus_page_line_to(320 * 256, 160 * 256);
    opus_page_line_to(0, 160 * 256);
    opus_page_close();
    opus_page_fill_path(rgb, 255);
}

static void usec(char *b, unsigned n, int v) { fw_snprintf(b, n, "%d", v); }

static void footer(int f, const int v[4])
{
    char b[4][6];
    opus_page_fill(0, 126, 319, 159, OPUS_COLOR_BACK);
    for (int i = 0; i < 4; ++i) {
        usec(b[i], sizeof b[i], v[i]);
        opus_page_text(me, i, i * 80 + 11, 129, 7, OPUS_COLOR_DIM, LAB[f][i]);
        opus_page_text_right(me, 4 + i, i * 80 + 72, 128, 9, OPUS_COLOR_E(i), b[i]);
        opus_page_fill(i * 80 + 6, 132, i * 80 + 8, 134, OPUS_COLOR_E(i));
        opus_page_fill(i * 80 + 6, 140, i * 80 + 72, 141, OPUS_COLOR_OFF);
        int w = v[i] * 66 / 99;
        if (w > 0) opus_page_fill(i * 80 + 6, 140, i * 80 + 6 + w, 141, OPUS_COLOR_E(i));
    }
}

static void draw_niche(int decay, int filt)
{
    int fh = 18 + decay * 12 / 99;
    int rays = 20 + filt * 16 / 99;
    for (int y = 14; y < 68; y += 5) ln_c(160, y, 160, y + 2, 256, C_GOLD_D);
    for (int i = 0; i < 15; ++i) {
        int x0 = 160 + RAYS[i][0] * 16 / 1000, y0 = 66 + RAYS[i][1] * 16 / 1000;
        int x1 = 160 + RAYS[i][0] * rays / 1000, y1 = 66 + RAYS[i][1] * rays / 1000;
        ln_c(x0, y0, x1, y1, 256, C_GOLD_D);
    }
    for (int i = 0; i < 25; ++i) {
        int y = 84 - (fh * i) / 24;
        int w = FLAME_W[i] * fh / 25;
        if (w > 0) ln_c(160 - w, y, 160 + w, y, 256, C_GOLD_L);
    }
}

static void draw_cross(int cross, int polar, int drive, int tone)
{
    int r = 34 + cross * 12 / 99;
    ring_at(RING48, 48, 160, 62, r, C_GOLD);
    ring_at(RING48, 48, 160, 62, r - 6, C_GOLD_D);
    if (drive > 4) {
        ring_at(RING48, 48, 160, 62, r - 2, C_GOLD_L);
        if (drive > 50) ring_at(RING48, 48, 160, 62, r - 4, C_GOLD_L);
    }
    opus_page_dot(160, 62, 2, 1, C_IVORY);
    {
        unsigned c = polar < 50 ? C_TURQ : C_GOLD_L;
        int len = 26 + cross * 44 / 99;
        ln_c(160 - len, 62, 160 - 10, 62, 256, c);
        ln_c(160 + 10, 62, 160 + len, 62, 256, c);
        ln_c(160 - 10, 62, 160 - 16, 58, 256, c);
        ln_c(160 - 10, 62, 160 - 16, 66, 256, c);
        ln_c(160 + 10, 62, 160 + 16, 58, 256, c);
        ln_c(160 + 10, 62, 160 + 16, 66, 256, c);
    }
    {
        int tw = 16 + tone * 56 / 99;
        ln_c(160 - tw, 110, 160 + tw, 110, 256, C_DIM);
    }
}

static void draw_mod(int rate, int shape, int depth, int dest)
{
    int amp = 10 + depth * 22 / 99;
    int prev = 0, prev_x = 0;
    for (int i = 0; i <= 120; ++i) {
        int bh = (i * 65536 / 120) & 0xFFFF;
        int tri = bh < 32768 ? bh * 2 - 32768 : 98304 - bh * 2;
        int v;
        if (shape < 25) v = tri;
        else if (shape < 50) v = bh - 32768;
        else if (shape < 75) v = bh < 32768 ? -32768 : 32767;
        else v = ((i / 12) & 1) ? 26000 : -26000;
        int y = 66 - v * amp / 32768;
        int x = 24 + i * 2;
        if (i) ln_c(prev_x, prev, x, y, 256, C_GOLD);
        prev = y; prev_x = x;
    }
    for (int i = 0; i < 4; ++i) {
        int on = (dest / 25) == i;
        opus_page_dot(96 + i * 44, 112, 3, on, on ? C_GOLD_L : C_GOLD_D);
    }
    (void)rate;
}

static void draw_core(int core, int cshape, int fuzz, int grit)
{
    /* one cycle of the selected core, with its shape and fuzz */
    int width = 12000 + cshape * 210;
    int prev = 66, prevx = 30;
    for (int i = 0; i <= 128; ++i) {
        int ph = i * 512;
        int v;
        if (core < 25) v = ph < 32768 ? ph * 2 - 32768 : 98304 - ph * 2;
        else if (core < 50) v = SINTAB[(ph >> 8) & 255];
        else if (core < 75) v = ph - 32768;
        else v = ph < width ? 32767 : -32768;
        if (fuzz) v = soft((v * (32768 + fuzz * 330)) >> 15);
        if (grit) v += (i & 1) ? grit * 8 : -grit * 8;
        int y = 66 - v * 34 / 32768;
        int x = 30 + i * 2;
        if (i) ln_c(prevx, prev, x, y, 256, C_GOLD);
        prev = y; prevx = x;
    }
    ln_c(30, 66, 288, 66, 256, C_DIM);
}

static void page_t1(void)
{
    int f = face;
    ++g_n;
    int v[4];
    for (int i = 0; i < 4; ++i) v[i] = k99(fv[f][i]);

    bg_fill(C_LAPIS);
    ln_c(2, 3, 317, 3, 256, C_GOLD_D);
    ring_at(STAR8, 16, 7, 9, 3, C_GOLD_D);
    ring_at(STAR8, 16, 313, 9, 3, C_GOLD_D);

    if (f == F_NICHE) {
        polyline_c(ARCH_OUT, 50, C_GOLD);
        polyline_c(ARCH_IN, 50, C_GOLD_D);
        ln_c(82, 117, 238, 117, 256, C_GOLD_D);
        ln_c(82, 118, 238, 118, 256, C_DIM);
        ring_at(STAR8, 16, 77, 33, 4, C_GOLD_D);
        ring_at(STAR8, 16, 243, 33, 4, C_GOLD_D);
        polyline_c(BOWL, 25, C_TURQ);
        draw_niche(v[2], v[3]);
    } else if (f == F_SPACE) {
        ring_at(RING48, 48, 160, 62, 58, C_DIM);
        ring_at(RING48, 48, 160, 62, 47, C_DIM);
        ring_at(SQ4, 5, 160, 62, 24, C_GOLD);
        for (int i = 0; i < 4; ++i) {
            int a = i * 12, b2 = ((i + 1) & 3) * 12;
            ln_c(160 + RING48[a][0] * 34 / 1000, 62 + RING48[a][1] * 34 / 1000,
                 160 + RING48[b2][0] * 34 / 1000, 62 + RING48[b2][1] * 34 / 1000, 256, C_GOLD_D);
        }
        ring_at(OCT8, 9, 160, 62, 10, C_GOLD_D);
        {
            int head = (g_n * 3) & 47;
            opus_page_dot(160 + RING48[head][0] * 58 / 1000, 62 + RING48[head][1] * 58 / 1000, 1, 1, C_GOLD_L);
        }
    } else if (f == F_CROSS) {
        draw_cross(v[0], v[1], v[2], v[3]);
    } else if (f == F_MOD) {
        draw_mod(v[0], v[1], v[2], v[3]);
    } else {
        draw_core(v[0], v[1], v[2], v[3]);
    }

    opus_page_text(me, 13, 20, 10, 7, OPUS_COLOR_DIM, FACE_NAME[f]);
    opus_page_text(me, 14, 286, 10, 6, C_GOLD_D, "v18");
    footer(f, v);
}

/* ---------------------------------------------------------------- the controls */

static void set_face(int f)
{
    if (f == face) return;
    short par[8];
    if (!opus_engine_knobs(par)) return;
    for (int i = 0; i < 4; ++i) fv[face][i] = par[i];
    face = (f + F_N) % F_N;
    lock_blocks = 6;
    for (int i = 0; i < 4; ++i) opus_engine_knob_set(i, fv[face][i]);
}

/* T3 and T4 page through our faces while T1 stays up; T2 stays the system's envelope. */
static int e_key(int key, int pressed, int page)
{
    (void)page;
    if (!pressed) return 0;
    if (key == 0x08) { set_face(face - 1); return 1; }     /* T3: previous */
    if (key == 0x09) { set_face(face + 1); return 1; }     /* T4: next */
    return 0;
}

static int e_turn(int encoder, int detents, int page)
{
    (void)encoder; (void)detents; (void)page;
    return 0;
}

static const struct opus_engine SAMA = {
    .size = sizeof(struct opus_engine),
    .name = "sama",
    .knobs = { 18204, 20190, 29457, 17873, 0, 0, 0, 0 },
    .voice = sizeof(struct voice),
    .init = v_init,
    .render = v_render,
    .block = b_block,
    .page = page_t1,
    .turn = e_turn,
    .key = e_key,
};

int opl_init(struct opl_self *self)
{
    me = self;
    line = opus_alloc_audio(self, DMASK * sizeof(short));
    if (!line) opus_log(self, "sama: no delay memory, dry");
    widx = 0;
    tlen = TIMETAB[R_time];
    tbase = tlen;
    return opus_engine_add(self, &SAMA) < 0 ? -1 : 0;
}
