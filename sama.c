/* sama: an instrument of light and repetition, for Opus 2 on the original OP-1.
 *
 * Strega's architecture, inverted. One oscillator into one shared delay; the delay's filter
 * and absorb age the tail; the note's sub-harmonic warps the delay time; the loop feeds back
 * into pitch and level. Six voices sing into one niche.
 *
 *   T1 holds three faces, and T3 turns to the next:
 *     niche    tones, time, decay, filter   (the instrument)
 *     space    absorb, breath, warp, drift  (how the room moves and ages)
 *     kindle   attack, hold, release, cross (how a note swells, lingers, and is gated by the loop)
 *   T2 is the kindle page; T4 is the OS's LFO.
 *
 * Encoder turns reach an engine only on its own T1 page, so every control lives on the four
 * T1 knobs: the OS moves them (we never take a turn), and a face only decides what they mean.
 * Each face keeps its own four values.
 */
#include "opus_plugin.h"
#include "op1fw.h"
#include "sama_art.h"
#include "sama_tab.h"

#define DMASK   65536u
#define BLEND   25559                 /* the WET share of the output, x 32768 (about 0.78); the dry is 0.22 */

enum { F_NICHE, F_SPACE, F_KINDLE, F_CROSS, F_N };
enum { ST_OFF, ST_ATTACK, ST_SUSTAIN, ST_HOLD, ST_RELEASE };

struct voice { unsigned ph, sub; int env; unsigned stage, hold; };

static struct opl_self *me;
static short *line;
static unsigned widx;
static int tlen, tbase, wlp, ic1, ic2, dcz, hlp, alp, e1, e2;
static volatile int R_gate = 32767;
static unsigned g_bph, g_subph, g_n, g_seed = 22222u;
static volatile unsigned g_substep;

static volatile int lock_blocks;
static volatile int face = F_NICHE;
static short fv[F_N][4] = {
    { 18204, 20190, 29126, 17873 },   /* niche:  tones 55, time 61 (249 ms), decay 88, filter 54 */
    {  6620, 11915, 13239,  9929 },   /* space:  absorb 20, breath 36 (0.32 Hz), warp 40, drift 30 */
    {  9929, 29126, 25816,  9929 },   /* kindle: attack 30, hold 88, release 78, swell 30 */
    { 11587,  9929,  3310, 13239 }    /* cross:  cross 35, polar 30 (duck), drive 10, tone 40 */
};
#define ENV_TOP (32767 << 16)

/* resolved once a block from each face's four values */
static volatile int R_tones = 21182, R_time = 60, R_decay = 86, R_filter = 58;
static volatile int R_abs = 20, R_brt = 36, R_warp = 40, R_drift = 30;
static volatile int R_att = 12, R_rel = 16, R_swell = 30;
static volatile int R_cross = 35, R_polar = 30, R_drive = 10, R_tone = 40;
static volatile unsigned R_hold = 17600;
static volatile int g_breath = 16384, g_drift = 0;

static int k99(short v) { return v < 0 ? 0 : (v * 99 + 16383) / 32767; }

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
    s->env = 0;
    s->stage = ST_OFF;
    s->hold = 0;
}

static void v_on(void *v, const short *knobs)
{
    struct voice *s = v;
    if (s->env < (400 << 16)) s->env = 0;
    s->stage = ST_ATTACK;
    s->hold = R_hold;
    (void)knobs;
}

static void v_off(void *v, const short *knobs)
{
    struct voice *s = v;
    if (s->stage == ST_OFF) return;
    s->stage = ST_HOLD;
    s->hold = R_hold;
    (void)knobs;
}

static int v_fin(void *v)
{
    struct voice *s = v;
    if (s->stage == ST_HOLD) {
        if (s->hold) --s->hold;
        else s->stage = ST_RELEASE;
    }
    return s->stage == ST_RELEASE && s->env <= 0;
}

static void v_render(void *v, short *out, const short *par, const unsigned *pitch)
{
    struct voice *s = v;
    (void)par;
    int tones = R_tones;
    int breath = g_breath;
    int drift = (g_drift * R_drift) / 300;

    int saw_amt = (tones - 3276) * 32767 / 22937;
    int fold_amt = (tones - 14745) * 32767 / 18022;
    if (saw_amt < 0) saw_amt = 0; else if (saw_amt > 32767) saw_amt = 32767;
    if (fold_amt < 0) fold_amt = 0; else if (fold_amt > 32767) fold_amt = 32767;

    int gq8 = 256 + (fold_amt * 1792) / 32767;           /* x256: 1 .. 8 folds */

    for (int n = 0; n < 128; ++n) {
        unsigned step = pitch[n];
        if (drift)
            step = (unsigned)((int)step + ((((int)(step >> 12)) * drift) >> 3));

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
        int saw = ph - 32768;
        int x = tri + ((((saw - tri) >> 1) * saw_amt) >> 14);

        int fph = (x * gq8) >> 8;                         /* the sine's phase, Q8 */
        int i0 = (fph >> 8) & 255;
        int fr = fph & 255;
        int s0 = SINTAB[i0], s1 = SINTAB[(i0 + 1) & 255];
        int f = s0 + (((s1 - s0) * fr) >> 8);             /* interpolated, no stair-steps */
        int z = x + (((f - x) * fold_amt) >> 15);          /* bend at low drive, fold at high */

        if (s->stage == ST_ATTACK) {
            s->env += ((ENV_TOP - s->env) >> R_att) + 1;
            if (s->env > ENV_TOP - 65536) { s->env = ENV_TOP; s->stage = ST_SUSTAIN; }
        } else if (s->stage == ST_SUSTAIN || s->stage == ST_HOLD) {
            s->env += ((ENV_TOP - s->env) >> (R_att + 3)) + 1;
            if (s->env > ENV_TOP - 65536) s->env = ENV_TOP;
        } else if (s->stage == ST_RELEASE) {
            s->env -= (s->env >> R_rel) + 1;
            if (s->env < (1500 << 16)) s->env = 0;
        }

        int y = (z * (s->env >> 16)) >> 15;
        y = (y * 26) >> 5;                                   /* headroom for six voices */
        int amp = 32767 - (((32767 - breath) * R_swell) / 99);  /* the breath, as a tremolo */
        if (R_cross) amp = (amp * R_gate) >> 15;              /* the loop ducks the source */
        y = (y * amp) >> 15;
        out[n] = (short)soft(y);
    }
}

/* ---------------------------------------------------------------- the niche */

static void b_block(short *buf)
{
    /* read the sound's own knobs, not the voice's block (which is zero until a note sounds) */
    short par[8];
    if (!opus_engine_knobs(par)) return;

    /* the knob positions belong to the face on screen; the other faces keep theirs.
       just after a switch the knobs still read the old face, so leave them alone briefly. */
    if (lock_blocks) --lock_blocks;
    else for (int i = 0; i < 4; ++i) fv[face][i] = par[i];

    R_tones = fv[F_NICHE][0] < 0 ? 0 : fv[F_NICHE][0];
    R_time = k99(fv[F_NICHE][1]);
    R_decay = k99(fv[F_NICHE][2]);
    R_filter = k99(fv[F_NICHE][3]);

    R_abs = k99(fv[F_SPACE][0]);
    R_brt = k99(fv[F_SPACE][1]);
    R_warp = k99(fv[F_SPACE][2]);
    R_drift = k99(fv[F_SPACE][3]);

    R_att = 9 + ((99 - k99(fv[F_KINDLE][0])) * 11) / 99;
    R_hold = (unsigned)((k99(fv[F_KINDLE][1]) * 20000u) / 99u);
    R_rel = 9 + ((99 - k99(fv[F_KINDLE][2])) * 14) / 99;
    R_swell = k99(fv[F_KINDLE][3]);

    R_cross = k99(fv[F_CROSS][0]);
    R_polar = k99(fv[F_CROSS][1]);
    R_drive = k99(fv[F_CROSS][2]);
    R_tone = k99(fv[F_CROSS][3]);

    if (!line) return;

    int target = TIMETAB[R_time];                  /* the base delay, Q8 samples */
    int fi = R_filter;
    int dec = 15000 + R_decay * 180;               /* 0.458 .. 1.002: decay owns the tail */
    int abswc = ABSTAB[R_abs];                     /* absorb: 18 kHz .. 300 Hz, per pass */
    int twc = TONETAB[R_tone];                     /* the loop's highpass: 80 Hz .. 1.2 kHz */
    int drv = 32768 + R_drive * 330;               /* loop drive 1.0 .. 2.0 */
    int wc = WLP[R_time];

    g_seed = g_seed * 1103515245u + 12345u;

    /* the breath: a looping AD with an angle */
    g_bph += BRTTAB[R_brt];
    {
        int bh = (int)(g_bph >> 16);
        int bt = bh < 32768 ? bh * 2 - 32768 : 98304 - bh * 2;
        int t = ((bt + 32768) * 1000) >> 16;
        int aa = 50 + 50 * 9;
        g_breath = t < aa ? (t * 32767) / (aa ? aa : 1) : ((1000 - t) * 32767) / (1000 - aa);
        target += (int)(((long long)target * (g_breath - 16384)) >> 22);
        if (target < 2048) target = 2048;
        if (target > 52920 * 256) target = 52920 * 256;
        fi += ((g_breath - 16384) * 55) >> 15;
    }
    /* the loop colours its own filter */
    fi += ((g_drift - 16384) * 24) >> 15;
    if (fi < 0) fi = 0; else if (fi > 99) fi = 99;

    /* the loop's level moves the source: duck below 50, lift above it */
    {
        int bus = g_drift < 0 ? -g_drift : g_drift;
        bus = (bus << 3) > 32767 ? 32767 : bus << 3;
        int depth = (R_cross * (50 - R_polar) * 2) / 99;  /* +duck .. -lift, 0 at 50 */
        int g2 = 32767 - ((bus * depth) >> 7);
        if (g2 < 4096) g2 = 4096; else if (g2 > 65535) g2 = 65535;
        R_gate = g2;
    }

    int g = GTAB[fi], kq = 3400;
    int den = 4096 + ((g * (g + kq)) >> 12);
    int a1 = (4096 << 12) / den;
    int a2 = (g * a1) >> 12;
    int a3 = (g * a2) >> 12;
    if (a3 < 1 && g > 4) a3 = 1;          /* keep the lp path from becoming an integrator */

    int wd = (R_warp * 28) / 99;                   /* warp depth, 0 .. 28 */
    unsigned subph = g_subph;
    unsigned substep = g_substep >> 1;             /* one sample at 44.1 kHz */
    int lasty = 0;

    for (int n = 0; n < 64; ++n) {
        int src = buf[n];

        tbase += (target - tbase) >> 10;           /* only the base is slewed */
        subph += substep;
        {
            int sp = (int)(subph >> 16);
            int st = sp < 32768 ? sp * 2 - 32768 : 98304 - sp * 2;
            int tnow = tbase + (int)((((long long)tbase * st) >> 31) * wd);
            if (tnow < 256) tnow = 256;
            if (tnow > 52920 * 256) tnow = 52920 * 256;

            unsigned didx = (unsigned)(tnow >> 8);
            unsigned idx = (widx - didx) & (DMASK - 1);
            int fr = tnow & 255;
            int x = line[idx];
            int xb = line[(idx - 1) & (DMASK - 1)];
            int y = x + (((xb - x) * fr) >> 8);

            int v3 = y - ic2;
            int acc1 = a1 * ic1 + a2 * v3 + e1;
            int acc2 = a2 * ic1 + a3 * v3 + e2;
            int v1 = acc1 >> 12;
            int v2 = ic2 + (acc2 >> 12);
            e1 = acc1 - (v1 << 12);            /* carry the bits the shift dropped */
            e2 = acc2 - ((acc2 >> 12) << 12);
            ic1 = 2 * v1 - ic1;
            ic2 = 2 * v2 - ic2;
            if (ic1 > (1 << 18)) ic1 = 1 << 18; else if (ic1 < -(1 << 18)) ic1 = -(1 << 18);
            if (ic2 > (1 << 18)) ic2 = 1 << 18; else if (ic2 < -(1 << 18)) ic2 = -(1 << 18);
            y = v2;

            dcz += (y - dcz) >> 9;                 /* no DC in a loop with unity gain */
            y -= dcz;
            hlp += ((y - hlp) * twc) >> 15;        /* tight, not muddy */
            y -= hlp;

            alp += ((y - alp) * abswc) >> 15;      /* absorb: the loop's own lowpass */
            y = alp;

            int w = src + ((y * dec) >> 15);
            w = soft((w * drv) >> 15);
            wlp += ((w - wlp) * wc) >> 15;         /* the delay's bandwidth follows its length */
            int ws = soft(wlp);
            if (R_time > 60) ws += (int)((g_seed >> 20) & 3) - 1;
            line[widx] = (short)ws;
            widx = (widx + 1) & (DMASK - 1);

            int o = src + (((y - src) * BLEND) >> 15);
            o = (o * 40) >> 5;                     /* output trim: the loop runs quiet */
            buf[n] = (short)soft(o);
            lasty = y;
        }
    }
    g_subph = subph;
    {   /* the loop's envelope: rectify, then smooth (a signed lowpass averages to zero) */
        int a = lasty < 0 ? -lasty : lasty;
        g_drift += ((a - g_drift) >> 5);
    }
}

/* ---------------------------------------------------------------- the pages */

static const char *const LAB[4][4] = {
    { "tones", "time", "decay", "filter" },
    { "absorb", "breath", "warp", "drift" },
    { "attack", "hold", "release", "swell" },
    { "cross", "polar", "drive", "tone" }
};
static const char *const FACE_NAME[4] = { "niche", "space", "kindle", "cross" };

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

static void draw_kindle(int a, int h, int r)
{
    int wa = 16 + a * 44 / 99;
    int wh = 40 + h * 110 / 99;
    int wr = 20 + r * 60 / 99;
    int tot = wa + wh + wr, sc = 250 * 1000 / (tot ? tot : 1);
    int ax = wa * sc / 1000, hw = wh * sc / 1000, rw = wr * sc / 1000;

    for (int i = 1; i <= 12; ++i)
        ln_c(40 + ax * (i - 1) / 12, 104 - (28 * (i - 1)) / 12,
             40 + ax * i / 12, 104 - (28 * i) / 12, 256, C_GOLD);
    ln_c(40 + ax, 76, 40 + ax + hw, 76, 256, C_GOLD);
    for (int i = 1; i <= 12; ++i)
        ln_c(40 + ax + hw + rw * (i - 1) / 12, 76 + (28 * (i - 1) * (i - 1)) / 144,
             40 + ax + hw + rw * i / 12, 76 + (28 * i * i) / 144, 256, C_GOLD);
    ln_c(30, 108, 290, 108, 256, C_DIM);
}

static void draw_whirl(int spin)
{
    ring_at(RING48, 48, 160, 62, 58, C_DIM);
    ring_at(RING48, 48, 160, 62, 47, C_DIM);
    ring_at(SQ4, 5, 160, 62, 24, C_GOLD);
    for (int i = 0; i < 4; ++i) {
        int a = i * 12, b2 = ((i + 1) & 3) * 12;      /* 0, 90, 180, 270: the second square, turned */
        ln_c(160 + RING48[a][0] * 34 / 1000, 62 + RING48[a][1] * 34 / 1000,
             160 + RING48[b2][0] * 34 / 1000, 62 + RING48[b2][1] * 34 / 1000, 256, C_GOLD_D);
    }
    ring_at(OCT8, 9, 160, 62, 10, C_GOLD_D);
    for (int k = 0; k < 3; ++k) {
        int r = 58 - k * 11;
        int head = (spin * (1 + k) / 3 + k * 9) & 47;
        for (int s2 = 0; s2 < 8; ++s2) {
            int i0 = (head - 8 + s2 + 48) & 47, i1 = (head - 8 + s2 + 1 + 48) & 47;
            unsigned c = (s2 > 4) ? C_GOLD : (s2 > 1 ? C_TURQ : C_DIM);
            ln_c(160 + RING48[i0][0] * r / 1000, 62 + RING48[i0][1] * r / 1000,
                 160 + RING48[i1][0] * r / 1000, 62 + RING48[i1][1] * r / 1000, 256, c);
        }
        opus_page_dot(160 + RING48[head][0] * r / 1000, 62 + RING48[head][1] * r / 1000, 1, 1, C_GOLD_L);
    }
    opus_page_dot(160, 62, 1, 1, C_IVORY);
}

/* the whole page, every frame: nothing is cached, so nothing can stack */
static void draw_cross(int cross, int polar, int drive, int tone)
{
    int r = 34 + cross * 12 / 99;
    ring_at(RING48, 48, 160, 62, r, C_GOLD);            /* the loop */
    ring_at(RING48, 48, 160, 62, r - 6, C_GOLD_D);
    if (drive > 4) {                                    /* drive thickens the rim */
        ring_at(RING48, 48, 160, 62, r - 2, C_GOLD_L);
        if (drive > 50) ring_at(RING48, 48, 160, 62, r - 4, C_GOLD_L);
    }
    opus_page_dot(160, 62, 2, 1, C_IVORY);              /* the source */
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
        int tw = 16 + tone * 56 / 99;                   /* tone: the loop's floor */
        ln_c(160 - tw, 110, 160 + tw, 110, 256, C_DIM);
    }
}

static void page_t1(void)
{
    int f = face;
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
        draw_whirl(++g_n);
    } else if (f == F_KINDLE) {
        draw_kindle(v[0], v[1], v[2]);
    } else {
        draw_cross(v[0], v[1], v[2], v[3]);
    }

    opus_page_text(me, 13, 20, 10, 7, OPUS_COLOR_DIM, FACE_NAME[f]);
    opus_page_text(me, 10, 292, 116, 6, C_GOLD_D, "v16");
    footer(f, v);
}

static void page_t2(void)
{
    int v[4];
    for (int i = 0; i < 4; ++i) v[i] = k99(fv[F_KINDLE][i]);
    bg_fill(C_NIGHT);
    ln_c(2, 3, 317, 3, 256, C_GOLD_D);
    opus_page_text(me, 13, 20, 10, 7, OPUS_COLOR_DIM, "kindle");
    draw_kindle(v[0], v[1], v[2]);
    opus_page_text(me, 11, 92, 116, 6, C_GOLD_D, "edit on t1");
    footer(F_KINDLE, v);
}

static void page_t4(void)
{
    int v[4];
    for (int i = 0; i < 4; ++i) v[i] = k99(fv[F_SPACE][i]);
    bg_fill(C_NIGHT);
    ln_c(2, 3, 317, 3, 256, C_GOLD_D);
    opus_page_text(me, 13, 20, 10, 7, OPUS_COLOR_DIM, "whirl");
    draw_whirl(++g_n);
    opus_page_text(me, 12, 92, 116, 6, C_GOLD_D, "edit on t1");
    footer(F_SPACE, v);
}

/* ---------------------------------------------------------------- the controls */

static void set_face(int f)
{
    if (f == face) return;
    short par[8];
    if (!opus_engine_knobs(par)) return;
    for (int i = 0; i < 4; ++i) fv[face][i] = par[i];
    face = f;
    lock_blocks = 6;
    for (int i = 0; i < 4; ++i) opus_engine_knob_set(i, fv[face][i]);
}

/* T3 turns the face. T1 is left to the OS. Pushes turn it too, as a second way. */
static int e_key(int key, int pressed, int page)
{
    (void)page;
    if (!pressed) return 0;
    if (key == 0x08) { set_face((face + 1) % F_N); return 1; }        /* T3: next face */
    if (key == 0x07) { set_face(F_KINDLE); return 0; }                 /* T2: the kindle page */
    if (key == 0x09) { set_face(F_SPACE); return 0; }                  /* T4: the whirl page */
    return 0;
}

static int e_turn(int encoder, int detents, int page)
{
    (void)encoder; (void)detents; (void)page;
    return 0;                       /* the OS always moves the knobs */
}

static const struct opus_engine SAMA = {
    .size = sizeof(struct opus_engine),
    .name = "sama",
    .knobs = { 18204, 20190, 29126, 17873, 0, 0, 0, 0 },
    .voice = sizeof(struct voice),
    .init = v_init,
    .note_on = v_on,
    .release = v_off,
    .render = v_render,
    .block = b_block,
    .page = page_t1,
    .page_t2 = page_t2,
    .page_t4 = page_t4,
    .turn = e_turn,
    .key = e_key,
    .flags = OPUS_ENGINE_OWN_ENVELOPE,
    .finished = v_fin,
};

int opl_init(struct opl_self *self)
{
    me = self;
    line = opus_alloc_audio(self, DMASK * sizeof(short));
    if (!line) opus_log(self, "sama: no delay memory, dry");
    widx = 0;
    tlen = TIMETAB[61];
    tbase = tlen;
    return opus_engine_add(self, &SAMA) < 0 ? -1 : 0;
}
