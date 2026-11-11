/* sama 1.0: an instrument of light and repetition, for Opus 2 on the original OP-1.
 *
 * One oscillator into one shared, tuned room. The room's filter/loss/feedback age the
 * sound; the note's sub-harmonic warps the delay time; the loop moves the pitch and the
 * level of the source; the room's audio bends the source's phase. Six voices sing into
 * one room, and the room answers.
 *
 * T1 holds six faces; T3/T4 turn to the previous/next one. T2 and T4 are the system's
 * own pages (envelope, LFO). The four encoder pushes jump to niche/space/cross/core;
 * T1 (no shift) loads the next scene; shift+T1 freezes the room.
 *
 *   niche   fold, time, decay, filter        the instrument
 *   space   tone, warp, drift, blend         how the room ages and moves
 *   cross   cross, polar, drive, resonance   the loop against the source
 *   mod     rate, shape, depth, dest         the internal modulator
 *   core    core, bias, growl, fuzz          the oscillator itself
 *   macro   body, pressure, motion, space     the performance layer
 *
 * Settings: scale (pitch-mod scale), tune (room tuned to the root), wow (tape),
 * keytrack (filter key tracking), diffuse (loop diffusion).
 */
#include "opus_plugin.h"
#include "op1fw.h"
#include "sama_art.h"
#include "sama_tab.h"

#define DMASK 65536u
#define APLEN 223
#define MAGIC 0x53414D41u          /* "SAMA" */
#define DATA_VERSION 2
#define REF_STEP 21426141u         /* A440 at 88.2 kHz, Q32 */
#define SCENE_N 4

enum { F_NICHE, F_SPACE, F_CROSS, F_MOD, F_CORE, F_MACRO, F_TUNE, F_N };

struct voice {
    unsigned ph, sub;              /* oscillator and sub phase */
    int dc;                        /* bias/DC blocker state */
    short strike;                  /* note-on transient */
    unsigned seed;                 /* per-voice noise */
    unsigned char idx;             /* 0..5, for spread */
};

struct sama_data {
    unsigned magic, version;
    short face;
    short v[F_N * 4];              /* 0..99 */
    short scene;                   /* was tail padding; v2-compatible */
};

static struct opl_self *me;
static short *line;
static short room[2][64];          /* double-buffered room -> voice feedback */
static volatile int g_room_sel;
static short ap[APLEN];
static unsigned widx, apw;
static int tbase, wlp, ic1, ic2, dcz, hlp, e1, e2;
static unsigned g_bph, g_n = 0x1234u, g_seed = 22222u, g_sh, g_wowph, g_subph;
static int g_shlast = 65535;
static volatile int g_transition;      /* key face/scene transition */
static volatile int g_load_transition; /* sound load owns this one */
static volatile int g_load_knobs_pending; /* blob owns the OS row until a turn */

static volatile int face = F_NICHE;
static volatile short fvl[F_N][4];          /* live (LFO) values; fv is stored */
static volatile short fv[F_N][4] = {
    { 14894, 23168, 30450, 16548 },   /* niche: fold 45, time 70, decay 92, filter 50 */
    { 11584,  8274,  4966, 23168 },   /* space: tone 35, warp 25, drift 15, blend 70 */
    { 11584, 13239,  8274, 14894 },   /* cross: cross 35, polar 40, drive 25, res 45 */
    {  9929,  4966, 14900,  9929 },   /* mod:   rate 30, shape 15, depth 45, dest 30 */
    { 11584, 16384, 11584,  1654 },   /* core:  core 35, bias 50, growl 35, fuzz 5 */
    { 18203, 14894, 19859, 18203 },   /* macro: 55 45 60 55 */
    { 16548,     0,  6619, 29789 }    /* tune:  50 0 20 90 */
};

/* the four scenes: 6 faces x 4 values, 0..99 */
static const unsigned char SCENES[SCENE_N][F_N * 4] = {
    { 35, 55, 92, 60,   40, 15, 15, 55,   20, 40, 15, 30,   45, 15, 30, 30,   40, 50, 20, 10,   50, 50, 50, 50,   50,  0,  0, 99 },
    { 45, 70, 92, 50,   35, 25, 15, 70,   35, 40, 25, 45,   30, 15, 45, 30,   35, 50, 35,  5,   55, 45, 60, 55,   50,  0, 20, 90 },
    { 20, 35, 95, 90,   60, 10, 10, 99,  25, 45, 10, 55,   50,  0, 20, 30,   50, 50,  0,  0,   70, 40, 30, 60,   50,  0,  0, 99 },
    { 75, 60, 95, 45,   25, 60, 50, 85,   70, 25, 60, 70,   55, 75, 80, 75,   80, 40, 70, 40,   40, 80, 80, 75,   50,  0,  0, 99 }
};
static const char *const SCENE_NAME[SCENE_N] = { "lamp", "choir", "comb", "storm" };

/* resolved once a block */
static volatile int R_fold = 45, R_time = 70, R_decay = 92, R_filter = 50;
static volatile int R_tone = 35, R_warp = 25, R_drift = 35, R_blend = 23168;
static volatile int R_cross = 35, R_polar = 40, R_drive = 25, R_res = 45;
static volatile int R_rate = 30, R_shape = 15, R_depth = 45, R_dest = 30;
static volatile int R_core = 35, R_bias = 50, R_growl = 35, R_fuzz = 5;
static volatile int R_diffuse = 30, R_wow = 18;
static volatile int R_tune = 50, R_spread = 0, R_harm = 20, R_air = 90;
static volatile int g_master = 65536, g_air_state = 0, g_out_dc = 0;
static const int MASTER[100] = { 61858, 61929, 62001, 62073, 62144, 62216, 62288, 62360, 62432, 62504, 62576, 62649, 62721, 62794, 62866, 62939, 63012, 63085, 63158, 63231, 63304, 63377, 63450, 63523, 63597, 63670, 63744, 63818, 63891, 63965, 64039, 64113, 64187, 64261, 64336, 64410, 64485, 64559, 64634, 64708, 64783, 64858, 64933, 65008, 65083, 65159, 65234, 65309, 65385, 65460, 65536, 65612, 65688, 65764, 65840, 65916, 65992, 66068, 66144, 66221, 66297, 66374, 66451, 66528, 66605, 66682, 66759, 66836, 66913, 66990, 67068, 67145, 67223, 67301, 67378, 67456, 67534, 67612, 67691, 67769, 67847, 67926, 68004, 68083, 68161, 68240, 68319, 68398, 68477, 68556, 68635, 68715, 68794, 68874, 68953, 69033, 69113, 69193, 69273, 69353 };
static volatile int R_gate = 32767;
static const int VGAIN[7] = { 32767, 32767, 23170, 18918, 16384, 14654, 13377 };

static volatile int g_breath = 16384, g_drift = 0, g_pitch_semi = 0, g_ratio = 65536, g_xmod = 0;
static volatile unsigned g_act_mask = 0;
static volatile int g_gate = 32767, g_gate_next = 32767;
static volatile int g_root_step = REF_STEP;
static volatile unsigned g_min_step = 0xFFFFFFFFu;
static volatile int g_strike = 0, g_vgain = 32767, g_active = 0;
static volatile int g_fbd = 0, g_fbd2 = 0, g_fbdc = 0;
static volatile int g_freeze = 0, g_tuned_k = 0, g_load_state = 0;
#define LOAD_IDLE 0
#define LOAD_PENDING 1
static volatile int g_scene = 0, g_msg_timer = 0;
static char g_msg[12];
static int g_scale = 0, g_tune = 0, g_wow = 18, g_keytrack = 70, g_diffuse = 30;

static int k99(short v) { if (v < 0) return 0; int n = v * 99 + 16383; return (n + (n >> 15) + 1) >> 15; }
static int cl99(int v) { return v < 0 ? 0 : v > 99 ? 99 : v; }
static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

/* soft clip: f(u) = u - u^3/3, normalised so u = 0..32767 -> 0..32767.
 * ik is the reciprocal of the knee (32767/knee), computed once a block. */
static int softx(int x, int ik, int knee, int gain)
{
    int s = x < 0 ? -1 : 1;
    unsigned ua = x < 0 ? (unsigned)(-(long long)x) : (unsigned)x;
    if (ua > (unsigned)knee) ua = (unsigned)knee;
    int u = (int)((ua * (unsigned)ik) >> 12);
    if (u > 32767) u = 32767;
    int i = u >> 8, fr = u & 255;
    int f = SOFTTAB[i] + (((SOFTTAB[i + 1] - SOFTTAB[i]) * fr) >> 8);
    if (gain) f += f >> 1;
    return s * ((f * knee) >> 15);
}

/* polyBLEP correction, t and dt in 0..65535; returns about -65536..65536 */
static int blep(int t, int dt)
{
    if (dt < 2) return 0;
    if (t < dt) {
        int x = (t * 65536) / dt;
        return 2 * x - (int)(((unsigned)x * (unsigned)x) >> 16) - 65536;
    }
    if (t > 65536 - dt) {
        int x = ((t - 65536) * 65536) / dt;
        unsigned ux = (unsigned)(x < 0 ? -x : x);
        return (int)((ux * ux) >> 16) + 2 * x + 65536;
    }
    return 0;
}

static int tri16(int ph)
{
    ph &= 0xFFFF;
    return ph < 32768 ? ph * 2 - 32768 : 98304 - ph * 2;
}

static int core_shape(int c, int ph, int width, int dt)
{
    int x;
    if (c <= 0) {
        x = tri16(ph);
    } else if (c == 1) {
        x = SINTAB[(ph >> 8) & 255];
    } else if (c == 2) {
        x = ph - 32768;
        x -= blep(ph, dt) >> 1;
    } else {
        x = ph < width ? 32767 : -32768;
        x += blep(ph, dt) >> 1;
        x -= blep((ph + 65536 - width) & 0xFFFF, dt) >> 1;
    }
    return clamp16(x);
}

static int log2q8(unsigned x)
{
    if (!x) return 0;
    int e = 0;
    unsigned y = x;
    while (y >>= 1) ++e;
    unsigned m = e >= 15 ? (x >> (e - 15)) : (x << (15 - e));
    int frac = LOG2FRAC[(m >> 11) & 15];
    return (e << 8) + frac;
}

/* delay time in Q8 samples for the tuned room: k periods of the root, k = 1..16 */
static int tuned_target(int time, int root)
{
    int want = TIMETAB[time];
    if (!g_tune || !root) { g_tuned_k = 0; return want; }
    int k = 1 + (time * 15) / 99;
    unsigned long long period = (4294967296ULL * 256ULL) / (2ULL * (unsigned)root);
    unsigned long long d = period * (unsigned)k;
    int lo = 32 * 256, hi = 52920 * 256;          /* 0.7 ms .. 1.2 s */
    if (d < (unsigned)lo) d = (unsigned)lo;
    if (d > (unsigned)hi) d = (unsigned)hi;
    g_tuned_k = k;
    return (int)d;
}

/* ---------------------------------------------------------------- a voice */

static void v_init(void *v, int index)
{
    struct voice *s = v;
    s->ph = (unsigned)index * 0x11111111u;
    s->sub = (unsigned)index * 0x22222222u;
    s->dc = 0;
    s->strike = 0;
    s->seed = 0x9E3779B9u * (unsigned)(index + 1);
    s->idx = (unsigned char)index;
    if (index == 0) {
        /* Defaults here, not only in the load block: if the load block is delayed
         * the first sound must still be the factory patch, never silence. */
        g_load_state = LOAD_PENDING;
        face = F_NICHE;
        g_active = 0;
        for (int f = 0; f < F_N; ++f)
            for (int i = 0; i < 4; ++i)
                fv[f][i] = (short)(SCENES[1][f * 4 + i] * 32767 / 99);
        for (int f = 0; f < F_N; ++f)
            for (int i = 0; i < 4; ++i) fvl[f][i] = fv[f][i];
        g_master = 65536;
        g_air_state = 0;
        g_out_dc = 0;
        g_act_mask = 0;
        hlp = 0;
        g_gate = 32767;
        g_gate_next = 32767;
        g_drift = 0;
        g_strike = 0;
        g_freeze = 0;
        g_scene = 1;
        g_msg_timer = 0;
        g_min_step = 0xFFFFFFFFu;
        g_root_step = REF_STEP;
        g_tuned_k = 0;
    }
}

static void v_note_on(void *v, const short *knobs)
{
    struct voice *s = v;
    (void)knobs;
    if (g_active < 6) ++g_active;
    g_act_mask |= 1u << s->idx;
    s->strike = 32767;
    s->seed = s->seed * 1664525u + 1013904223u;
    s->ph = s->seed * 0x9E3779B9u;
    s->sub = s->seed * 0x85EBCA6Bu;
    g_strike = 32767;
}

static void v_release(void *v, const short *knobs)
{
    struct voice *s = v;
    (void)knobs;
    if (g_active > 0) --g_active;
    g_act_mask &= ~(1u << s->idx);
}

static void v_render(void *v, short *out, const short *par, const unsigned *pitch)
{
    struct voice *s = v;
    (void)par;
    int fold = R_fold, core = R_core, bias = R_bias, growl = R_growl, fuzz = R_fuzz;
    int drift = g_tune ? 0 : (g_drift * R_drift) / 300;
    int ratio = g_ratio;
    ratio = (int)(((long long)ratio * g_master) >> 16);
    if (R_spread && g_active > 1) {
        int n = g_active, rank = 0;
        for (int j = 0; j < (int)s->idx; ++j)
            if (g_act_mask & (1u << j)) ++rank;
        int c = ((2 * rank - (n - 1)) * 10 * R_spread) / ((n - 1) * 99);
        ratio = (int)(((long long)ratio * (65536 + c * 38)) >> 16);
    }
    int gate0 = g_gate, gate1 = g_gate_next;
    int vg = g_vgain;
    int fbd = g_fbd, fbd2 = g_fbd2;
    const short *rm = room[g_room_sel];
    int key = g_keytrack;

    if (R_polar < 50) drift = -drift;             /* polar < 50 sags, > 50 lifts */

    int fold_amt = fold * 32767 / 99;
    if (fold_amt < 0) fold_amt = 0; else if (fold_amt > 32767) fold_amt = 32767;
    int gq8 = 256 + (fold_amt * 1792) / 32767;    /* x256: 1 .. 8 folds */
    int drive = 32768 + fuzz * 660;
    int width = 12000 + bias * 210 + g_drift * 2;
    int bias_dc = (bias - 50) * 120;

    if (pitch[0] < g_min_step) g_min_step = pitch[0];
    int vsemi = ((log2q8(pitch[0]) - log2q8(g_root_step)) * 12) >> 8;
    if (vsemi > 24) vsemi = 24; else if (vsemi < -24) vsemi = -24;
    fold_amt -= (vsemi * key * 2) / 100;          /* high notes fold a little less */
    if (fold_amt < 0) fold_amt = 0; else if (fold_amt > 32767) fold_amt = 32767;
    int famt_base = fold_amt;
    int strike = s->strike;
    int dt_base;

    for (int n = 0; n < 128; ++n) {
        unsigned step = pitch[n];
        int nstep = (int)step;
        if (drift) {
            nstep += ((nstep >> 12) * drift) >> 3;
            if (nstep < 1) nstep = 1;
        }

        s->sub += (unsigned)nstep >> 1;
        int st = tri16((int)(s->sub >> 16));

        if (growl) {
            long long md = ((long long)(nstep >> 12) * st) >> 4;
            nstep += (int)((md * growl) >> 8);
            if (nstep < 1) nstep = 1;
        }

        if (ratio != 65536)
            nstep = (int)(((unsigned long long)(unsigned)nstep * (unsigned)ratio) >> 16);
        if (nstep < 1) nstep = 1;
        step = (unsigned)nstep;

        s->ph += step;
        dt_base = (int)(step >> 16);
        if (dt_base > 32767) dt_base = 32767;

        int ph = (int)(s->ph >> 16);
        int famt = famt_base;
        if (strike) {
            famt += (strike * 12000) >> 15;
            if (famt > 32767) famt = 32767;
            s->strike = (short)(strike - (strike >> 9));
            strike = s->strike;
        }
        if (fbd) {
            int ri = n >> 1;
            int fb = rm[ri];
            if (n & 1) fb += (rm[ri < 63 ? ri + 1 : 63] - fb) >> 1;
            g_fbdc += (fb - g_fbdc) >> 6;
            fb -= g_fbdc;
            ph = (ph + ((fb * fbd) >> 15)) & 0xFFFF;
            if (fbd2) {
                famt += (fb * fbd2) >> 14;
                if (famt > 32767) famt = 32767; else if (famt < 0) famt = 0;
            }
        }

        int pos = core * 3;
        int ci = pos / 99;
        int cf = pos - ci * 99;
        int cfq = (cf * 32768) / 99;
        int a = core_shape(ci, ph, width, dt_base);
        int b = core_shape(ci < 3 ? ci + 1 : 3, ph, width, dt_base);
        int x = a + (((b - a) * cfq) >> 15);

        if (strike) {
            s->seed = s->seed * 1664525u + 1013904223u;
            x += ((((int)((s->seed >> 20) & 15)) - 8) * strike) >> 9;
        }

        x += bias_dc;
        x -= s->dc;
        s->dc += (x - s->dc) >> 10;

        if (fuzz) {
            if (x > 32767) x = 32767; else if (x < -32768) x = -32768;
            x = softx((x * (drive >> 1)) >> 14, 4096, 32767, 0);
        }

        {
            int fph = (x * gq8) >> 8;
            int i0 = (fph >> 8) & 255;
            int fr = fph & 255;
            int s0 = SINTAB[i0], s1 = SINTAB[(i0 + 1) & 255];
            int f = s0 + (((s1 - s0) * fr) >> 8);
            x += ((f - x) * famt) >> 15;
        }

        x += (st * 2621) >> 15;                    /* a quiet sub, -22 dB */

        int y = (x * 26) >> 5;
        if (y > 32767) y = 32767; else if (y < -32768) y = -32768;
        int gate = gate0 + (((gate1 - gate0) * n) >> 7);
        if (R_cross) y = (y * gate) >> 15;
        y = (y * vg) >> 15;
        out[n] = (short)softx(y, 4096, 32767, 0);
    }
}

/* ---------------------------------------------------------------- the room */

static void b_block(short *buf)
{
    int f = face;
    short par[8];
    if (!opus_engine_knobs(par)) return;
    short pl[8];
    int live = opus_engine_live(pl);       /* 1 while a voice sounds, 0 stored */
    if (!g_transition && !g_load_transition && !g_load_knobs_pending) {
        if (live) for (int i = 0; i < 4; ++i) fvl[f][i] = par[i];
        else for (int i = 0; i < 4; ++i) { fv[f][i] = par[i]; fvl[f][i] = par[i]; }
        if (f != face || g_transition || g_load_transition || g_load_knobs_pending) {
            /* a transition started after the check: undo the late copy */
            for (int i = 0; i < 4; ++i) fvl[f][i] = fv[f][i];
        }
    }

    if (!line) return;

    g_seed = g_seed * 1103515245u + 12345u;

    int fold = k99(fvl[F_NICHE][0]);
    int time = k99(fvl[F_NICHE][1]);
    int decay = k99(fvl[F_NICHE][2]);
    int filter = k99(fvl[F_NICHE][3]);
    int tone = k99(fvl[F_SPACE][0]);
    int warp = k99(fvl[F_SPACE][1]);
    int drift = k99(fvl[F_SPACE][2]);
    int blend = fvl[F_SPACE][3] < 0 ? 0 : fvl[F_SPACE][3];
    int cross = k99(fvl[F_CROSS][0]);
    int polar = k99(fvl[F_CROSS][1]);
    int drive = k99(fvl[F_CROSS][2]);
    int res = k99(fvl[F_CROSS][3]);
    int rate = k99(fvl[F_MOD][0]);
    int shape = k99(fvl[F_MOD][1]);
    int depth = k99(fvl[F_MOD][2]);
    int dest = k99(fvl[F_MOD][3]);
    int core = k99(fvl[F_CORE][0]);
    int bias = k99(fvl[F_CORE][1]);
    int growl = k99(fvl[F_CORE][2]);
    int fuzz = k99(fvl[F_CORE][3]);
    int body = k99(fvl[F_MACRO][0]) - 50;
    int pressure = k99(fvl[F_MACRO][1]) - 50;
    int motion = k99(fvl[F_MACRO][2]) - 50;
    int space = k99(fvl[F_MACRO][3]) - 50;
    int tune = k99(fvl[F_TUNE][0]);
    int spread = k99(fvl[F_TUNE][1]);
    int harm = k99(fvl[F_TUNE][2]);
    int air = k99(fvl[F_TUNE][3]);

    /* the performance macros are offsets on the deep values, neutral at 50 */
    filter = cl99(filter + (body * 6) / 10);
    decay = cl99(decay + (body * 4) / 10);
    tone = cl99(tone + (body * 3) / 10);
    res = cl99(res + (body * 2) / 10);
    fold = cl99(fold + (pressure * 8) / 10);
    drive = cl99(drive + (pressure * 5) / 10);
    fuzz = cl99(fuzz + (pressure * 5) / 10);
    bias = cl99(bias + (pressure * 3) / 10);
    growl = cl99(growl + (pressure * 3) / 10);
    depth = cl99(depth + (motion * 7) / 10);
    rate = cl99(rate + (motion * 4) / 10);
    warp = cl99(warp + (motion * 5) / 10);
    drift = cl99(drift + (motion * 2) / 10);
    cross = cl99(cross + (space * 2) / 10);
    tone = cl99(tone + (space * 2) / 10);
    blend += ((space * 32767) / 99) * 7 / 10;
    if (blend < 0) blend = 0; else if (blend > 32767) blend = 32767;

    int r_diffuse = cl99(g_diffuse + (space * 6) / 10);
    int r_wow = cl99(g_wow + (motion * 3) / 10);
    if (g_freeze) {                            /* frozen: a still room */
        r_diffuse = 0;
        r_wow = 0;
    }

    R_fold = fold; R_time = time; R_decay = decay; R_filter = filter;
    R_tone = tone; R_warp = warp; R_drift = (drift * drift) / 99; R_blend = blend;
    R_cross = cross; R_polar = polar; R_drive = drive; R_res = res;
    R_rate = rate; R_shape = shape; R_depth = depth; R_dest = dest;
    R_core = core; R_bias = bias; R_growl = growl; R_fuzz = fuzz;
    R_diffuse = r_diffuse; R_wow = r_wow;
    R_tune = tune; R_spread = spread; R_harm = harm; R_air = air;
    g_master = MASTER[cl99(tune)];

    if (g_min_step != 0xFFFFFFFFu) g_root_step = (int)g_min_step;
    int root = (int)(((long long)g_root_step * g_master) >> 16);
    if (root < 1) root = REF_STEP;

    int fi = filter;
    int rsemi = ((log2q8((unsigned)root) - log2q8(REF_STEP)) * 12) >> 8;
    if (rsemi > 36) rsemi = 36; else if (rsemi < -36) rsemi = -36;
    fi += (rsemi * 4 * g_keytrack) / 500;     /* 70%: ~0.56 filter steps per semitone */
    fi += (g_strike * 16) >> 15;

    int target = TIMETAB[time];
    if (g_tune) target = tuned_target(time, root);

    /* the internal modulator: a looping shape, steered to one destination */
    g_bph += BRTTAB[rate];
    int raw;
    {
        int bh = (int)(g_bph >> 16);
        int tri = tri16(bh);
        if (shape < 25) raw = tri + 32767;
        else if (shape < 50) raw = bh;
        else if (shape < 75) raw = (bh < 32768) ? 0 : 65535;
        else {
            if (bh < g_shlast) g_sh = (g_seed >> 8) & 0xFFFF;
            g_shlast = bh;
            raw = (int)g_sh;
        }
    }
    g_breath = raw;
    g_pitch_semi = 0;
    g_xmod = 0;
    g_ratio = 65536;
    {
        int m = ((raw - 32767) * depth) / 99;
        if (dest < 25) fi += (m * 28) >> 15;
        else if (dest < 50) {
            if (!g_tune) target += (int)(((long long)target * m) >> 20);
        } else if (dest < 75) {
            g_pitch_semi = (m * 12 * 256) / 32767;   /* Q8 semitones, +/- 12 */
            if (rate < 70) {
                int n = (g_pitch_semi + 128) >> 8;
                if (n < -12) n = -12; else if (n > 12) n = 12;
                if (g_scale) {
                    unsigned mask = (unsigned[]){ 0xFFFu, 0xAB5u, 0x5ADu, 0x295u }[g_scale & 3];
                    int best = n, bd = 99;
                    for (int k = -12; k <= 12; ++k) {
                        int p = k % 12; if (p < 0) p += 12;
                        if (!(mask & (1u << p))) continue;
                        int d = n - k; if (d < 0) d = -d;
                        if (d < bd) { bd = d; best = k; }
                    }
                    n = best;
                }
                g_pitch_semi = n * 256;
            }
            {
                int n = g_pitch_semi >> 8;
                if (n < -12) n = -12; else if (n > 12) n = 12;
                int fr = g_pitch_semi - n * 256;
                if (fr < 0) fr = 0; else if (fr > 255) fr = 255;
                if (n >= 12) g_ratio = (int)RATIO[24];
                else g_ratio = (int)RATIO[n + 12] +
                    (((int)RATIO[n + 13] - (int)RATIO[n + 12]) * fr >> 8);
            }
        } else {
            g_xmod = (m * 1024) / 32767;
        }
    }
    fi += ((g_breath - 32767) * 2) >> 15;       /* a touch of breath on the cutoff */
    if (fi < 0) fi = 0; else if (fi > 99) fi = 99;

    /* the loop's gate: the room's envelope ducks or lifts the source */
    {
        int bus = g_drift < 0 ? -g_drift : g_drift;
        bus = bus > 4095 ? 32767 : bus << 3;
        int dpth = (cross * (50 - polar) * 3) / 99;
        int g2 = 32767 - ((bus * dpth) >> 7);
        if (g_xmod) {
            g2 += (g2 * g_xmod) >> 12;
        }
        if (g2 < 4096) g2 = 4096; else if (g2 > 65535) g2 = 65535;
        R_gate = g2;
    }

    /* the room's audio bends the source at audio rate (one block behind) */
    {
        int fbd = (cross * 4096) / 99;
        if (polar < 50) fbd = -fbd;
        g_fbd = fbd;
        g_fbd2 = fbd >> 2;
    }

    /* filter */
    int kq = 8192 - (res * 7680) / 99;          /* Q 0.5 .. 8 */
    if (kq < 512) kq = 512;
    int g = GTAB[fi];
    int den = 4096 + ((g * (g + kq)) >> 12);
    int a1 = (4096 << 12) / den;
    int a2 = (g * a1) >> 12;
    int a3 = (g * a2) >> 12;
    if (a3 < 1 && g > 4) a3 = 1;

    int twc = TONETAB[tone];
    int wc = (WLP[time] * (50 + tone)) / 100;
    if (wc > 32767) wc = 32767;
    if (g_tune) { twc = TONETAB[tone]; wc = 32000; }
    if (g_freeze) { twc = 128; wc = 32000; hlp = 0; e1 = 0; e2 = 0; }

    int dec = 15000 + decay * 190;              /* 0.458 .. 1.002 */
    int knee = 27000 - drive * 190;
    if (knee < 6000) knee = 6000;
    int ik = (32767 * 4096) / knee;
    int apg = (r_diffuse * 19661) / 99;         /* 0 .. 0.6 */
    int wd = (warp * 40) / 99;
    int wowd = r_wow;

    unsigned subph = g_subph;
    unsigned substep = (unsigned)root;          /* f/2 at 44.1 kHz */
    int env_acc = 0;
    int hg = 0;
    if (R_harm) {
        hg = (int)(((long long)R_harm * R_harm * g_drift * 8) / (99 * 99 * 2));
        if (hg > 6000) hg = 6000;
    }

    for (int n = 0; n < 64; ++n) {
        int src = buf[n];
        tbase += (target - tbase) >> 10;
        subph += substep;
        int tnow;
        {
            int sp = (int)(subph >> 16);
            int st = sp < 32768 ? sp * 2 - 32768 : 98304 - sp * 2;
            int w = 0;
            if (!g_tune) w = (((tbase >> 8) * st) >> 23) * wd;
            if (!g_tune && wowd) {
                g_wowph += 48696;
                int ws = SINTAB[(g_wowph >> 24) & 255];
                int ww = ((tbase >> 8) * ws) >> 7;
                w += (int)((((long long)ww) * ((wowd * 32768) / 19800)) >> 15);
            }
            tnow = tbase + w;
            if (tnow < 32 * 256) tnow = 32 * 256;
            if (tnow > 52920 * 256) tnow = 52920 * 256;
        }
        if (g_freeze) { src = 0; dec = 32760; }

        unsigned didx = (unsigned)(tnow >> 8);
        unsigned idx = (widx - didx) & (DMASK - 1);
        int fr = tnow & 255;
        int x = line[idx];
        int xb = line[(idx - 1) & (DMASK - 1)];
        int y = x + (((xb - x) * fr) >> 8);
        int yo;

        if (g_tune) {
            yo = y;                             /* the loop is clean; the filter is on the output */
        } else {
            if (apg) {
                int delayed = ap[apw];
                int v = y + ((apg * delayed) >> 15);
                int o = delayed - ((apg * v) >> 15);
                ap[apw] = (short)clamp16(v);
                apw = (apw + 1) % APLEN;
                y = o;
            }
            {
                int v3 = y - ic2;
                int acc1 = a1 * ic1 + a2 * v3 + e1;
                int acc2 = a2 * ic1 + a3 * v3 + e2;
                int v1 = acc1 >> 12;
                int v2 = ic2 + (acc2 >> 12);
                e1 = acc1 - v1 * 4096;
                e2 = acc2 - (acc2 >> 12) * 4096;
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
            yo = y;
        }

        if (g_tune) {
            /* the filter/resonance and tone shape the room's output, not its tuning */
            {
                int v3 = yo - ic2;
                int acc1 = a1 * ic1 + a2 * v3 + e1;
                int acc2 = a2 * ic1 + a3 * v3 + e2;
                int v1 = acc1 >> 12;
                int v2 = ic2 + (acc2 >> 12);
                e1 = acc1 - v1 * 4096;
                e2 = acc2 - (acc2 >> 12) * 4096;
                ic1 = 2 * v1 - ic1;
                ic2 = 2 * v2 - ic2;
                if (ic1 > (1 << 18)) ic1 = 1 << 18; else if (ic1 < -(1 << 18)) ic1 = -(1 << 18);
                if (ic2 > (1 << 18)) ic2 = 1 << 18; else if (ic2 < -(1 << 18)) ic2 = -(1 << 18);
                yo = v2;
            }
            dcz += (yo - dcz) >> 9;
            yo -= dcz;
            hlp += ((yo - hlp) * twc) >> 15;
            yo -= hlp;
            if (apg) {
                int delayed = ap[apw];
                int v = yo + ((apg * delayed) >> 15);
                int o = delayed - ((apg * v) >> 15);
                ap[apw] = (short)clamp16(v);
                apw = (apw + 1) % APLEN;
                yo = o;
            }
        }

        if (y > 32767) y = 32767; else if (y < -32768) y = -32768;
        {
            int w = src + ((y * dec) >> 15);
            w = softx(w, ik, knee, 0);
            wlp += ((w - wlp) * wc) >> 15;
            int ws = softx(wlp, 4096, 32767, 0);
            line[widx] = (short)ws;
            widx = (widx + 1) & (DMASK - 1);
        }

        room[g_room_sel ^ 1][n] = (short)clamp16(y);
        env_acc += y < 0 ? -y : y;

        if (yo > 32767) yo = 32767; else if (yo < -32768) yo = -32768;
        {
            int o = src + (((yo - src) * (g_freeze ? 32767 : R_blend)) >> 15);
            if (hg) {
                int hp = (int)(subph >> 16);
                int h2 = SINTAB[((hp * 4) >> 8) & 255];
                int h3 = SINTAB[((hp * 6) >> 8) & 255];
                o += ((h2 * hg) >> 15) + ((h3 * hg) >> 16);
            }
            if (R_air < 99) {
                int w = 920 + R_air * 318;
                int d = o - g_air_state;
                g_air_state += (int)(((long long)d * w) >> 15);
                o = g_air_state;
            } else {
                g_air_state += (o - g_air_state) >> 3;
                o = g_air_state;
            }
            g_out_dc += (o - g_out_dc) >> (g_freeze ? 6 : 9);
            o -= g_out_dc;
            buf[n] = (short)softx(o, 4096, 32767, 0);
        }
    }
    g_subph = subph;
    __asm__ volatile("" ::: "memory");
    g_room_sel ^= 1;
    env_acc >>= 6;
    g_drift += ((env_acc - g_drift) >> 4);
    g_gate = g_gate_next;
    g_gate_next = R_gate;
    g_vgain = VGAIN[g_active > 6 ? 6 : (g_active < 0 ? 0 : g_active)];
    g_min_step = 0xFFFFFFFFu;
    if (g_strike) g_strike -= g_strike >> 3;
}

/* ---------------------------------------------------------------- the pages */

static const char *const LAB[F_N][4] = {
    { "fold", "time", "decay", "filter" },
    { "tone", "warp", "drift", "blend" },
    { "cross", "polar", "drive", "res" },
    { "rate", "shape", "depth", "dest" },
    { "core", "bias", "growl", "fuzz" },
    { "body", "pressure", "motion", "space" },
    { "tune", "spread", "harm", "air" }
};
static const char *const FACE_NAME[F_N] = { "niche", "space", "cross", "mod", "core", "macro", "tune" };

#define C_GOLD   OPUS_RGB(0xd9b45e)
#define C_GOLD_D OPUS_RGB(0x8a7440)
#define C_GOLD_L OPUS_RGB(0xf0d79a)
#define C_IVORY  OPUS_RGB(0xefe6d2)
#define C_TURQ   OPUS_RGB(0x3fa3a0)
#define C_LAPIS  OPUS_RGB(0x101b3c)
#define C_NIGHT  OPUS_RGB(0x0a1024)
#define C_DIM    OPUS_RGB(0x5a6280)

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
    int scq = (sc * 32768) / 1000;
    int px = cx + ((p[0][0] * scq) >> 15), py = cy + ((p[0][1] * scq) >> 15);
    for (int i = 1; i < n; ++i) {
        int x = cx + ((p[i][0] * scq) >> 15), y = cy + ((p[i][1] * scq) >> 15);
        ln_c(px, py, x, y, 256, c);
        px = x; py = y;
    }
    ln_c(px, py, cx + ((p[0][0] * scq) >> 15), cy + ((p[0][1] * scq) >> 15), 256, c);
}

static void usec(char *b, unsigned n, int v) { fw_snprintf(b, n, "%d", v); }

static void footer_static(int f)
{
    for (int i = 0; i < 4; ++i)
        opus_page_text(me, i, i * 80 + 11, 129, 7, OPUS_COLOR_DIM, LAB[f][i]);
}

static void footer_live(int f, const int v[4])
{
    char b[4][6];
    (void)f;
    for (int i = 0; i < 4; ++i) {
        usec(b[i], sizeof b[i], v[i]);
        opus_page_text_right(me, 4 + i, i * 80 + 72, 128, 9, OPUS_COLOR_E(i), b[i]);
        opus_page_fill(i * 80 + 6, 132, i * 80 + 8, 134, OPUS_COLOR_E(i));
        opus_page_fill(i * 80 + 6, 140, i * 80 + 72, 141, OPUS_COLOR_OFF);
        int w = v[i] * 66 / 99;
        if (w > 0) opus_page_fill(i * 80 + 6, 140, i * 80 + 6 + w, 141, OPUS_COLOR_E(i));
    }
}

static void draw_niche(int decay, int filt)
{
    int fh = 18 + decay * 12 / 99 + g_drift * 8 / 32768;
    int rays = 20 + filt * 16 / 99 + g_strike * 6 / 32768;
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

static void draw_cross(int cross, int polar, int drive, int res)
{
    int r = 34 + cross * 12 / 99 + g_drift * 4 / 32768;
    if (r > 40) r = 40;
    opus_page_dot(160, 62, r, 0, C_GOLD);
    opus_page_dot(160, 62, r - 6, 0, C_GOLD_D);
    int q = 1 + res * 6 / 99;
    for (int i = 1; i <= q; ++i)
        opus_page_dot(160, 62, r - 8 - i * 2, 0, C_GOLD_D);
    if (drive > 4) {
        opus_page_dot(160, 62, r - 2, 0, C_GOLD_L);
        if (drive > 50) opus_page_dot(160, 62, r - 4, 0, C_GOLD_L);
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
        int tw = 16 + res * 56 / 99;
        ln_c(160 - tw, 110, 160 + tw, 110, 256, C_DIM);
    }
}

static void draw_mod(int rate, int shape, int depth, int dest)
{
    int amp = 10 + depth * 22 / 99 + g_drift * 6 / 32768;
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

static void draw_core(int core, int bias, int growl, int fuzz)
{
    int width = 12000 + bias * 210 + g_drift * 2;
    int pos = core * 3;
    int ci = pos / 99;
    int cf = pos - ci * 99;
    int prev = 66, prevx = 30;
    for (int i = 0; i <= 128; ++i) {
        int ph = i * 512;
        int a = core_shape(ci, ph, width, 512);
        int b = core_shape(ci < 3 ? ci + 1 : 3, ph, width, 512);
        int v = a + ((b - a) * cf) / 99;
        v += (bias - 50) * 120;
        if (growl) v += (SINTAB[(ph >> 8) & 255] * growl) / 300;
        if (fuzz) v = softx((int)(((long long)v * (32768 + fuzz * 660)) >> 15), 4096, 32767, 0);
        int y = 66 - v * 34 / 32768;
        int x = 30 + i * 2;
        if (i) ln_c(prevx, prev, x, y, 256, C_GOLD);
        prev = y; prevx = x;
    }
    ln_c(30, 66, 288, 66, 256, C_DIM);
}

static void draw_tune(int tune, int spread, int harm, int air)
{
    int cy = 62;
    ln_c(40, cy, 280, cy, 256, C_DIM);
    for (int i = 0; i <= 20; ++i) {
        int x = 60 + i * 10;
        int h = (i % 5 == 0) ? 8 : 4;
        ln_c(x, cy - h, x, cy + h, 256, (i == 10) ? C_GOLD_L : C_GOLD_D);
    }
    int nx = 160 + ((tune - 50) * 200) / 99 + g_drift * 4 / 32768;
    ln_c(nx, cy - 30, nx, cy + 30, 512, C_GOLD);
    opus_page_dot(nx, cy, 3, 1, C_IVORY);
    for (int i = 0; i < 6; ++i)
        opus_page_dot(160 + ((i * 2 - 5) * (spread * 12 / 99)) / 2, 100, 2, 1, i < 3 ? C_TURQ : C_GOLD_D);
    ln_c(60, 116, 60 + harm * 30 / 99, 116, 512, C_TURQ);
    ln_c(60, 120, 60 + harm * 18 / 99, 120, 512, C_GOLD_D);
    ln_c(200, 108, 200 + air * 80 / 99, 100, 256, C_TURQ);
}

static void draw_macro(const int v[4])
{
    for (int i = 0; i < 4; ++i) {
        int cx = 60 + i * 68;
        int len = (v[i] - 50) * 40 / 50 + g_drift * 2 / 32768;
        ln_c(cx, 50, cx, 90, 256, C_DIM);
        ln_c(cx - 5, 70, cx + 5, 70, 256, C_GOLD_D);
        ln_c(cx, 70, cx, 70 - len, 256, C_GOLD);
        opus_page_dot(cx, 70 - len, 2, 1, C_GOLD_L);
    }
}

static void static_cross(void)
{
    ring_at(RING48, 48, 160, 62, 52, C_DIM);
    ln_c(140, 112, 180, 112, 256, C_GOLD_D);
}

static void static_mod(void)
{
    ln_c(100, 24, 220, 24, 256, C_GOLD);
    ln_c(160, 24, 160, 30, 256, C_GOLD_D);
    for (int i = 0; i < 13; ++i) {
        int a = 30 + i * 10;
        int x0 = 160, y0 = 28;
        int x1 = 160 + (a - 90) * 10 / 10;
        int y1 = 28 + 40 + (a > 90 ? (a - 90) / 3 : (90 - a) / 3);
        ln_c(x0, y0, x1, y1, 256, (i % 3 == 0) ? C_GOLD : C_GOLD_D);
    }
}

static void static_core(void)
{
    for (int i = 0; i < 3; ++i) ln_c(30, 46 + i * 20, 290, 46 + i * 20, 256, C_DIM);
    for (int i = 0; i < 4; ++i) ln_c(30 + i * 86, 30, 30 + i * 86, 102, 256, C_DIM);
    for (int i = 0; i < 8; ++i) ln_c(44 + i * 32, 101, 44 + i * 32, 106, 256, C_GOLD_D);
}

static void static_macro(void)
{
    ln_c(34, 28, 286, 28, 256, C_GOLD);
    for (int i = 0; i < 4; ++i) {
        int cx = 60 + i * 68;
        ln_c(cx, 28, cx, 70, 256, C_GOLD_D);
        opus_page_dot(cx, 70, 2, 0, C_GOLD_D);
    }
    ln_c(30, 114, 290, 114, 256, C_GOLD_D);
}

static void static_tune(void)
{
    ln_c(40, 62, 280, 62, 256, C_DIM);
    for (int i = 0; i <= 20; ++i) {
        int x = 60 + i * 10;
        int h = (i % 5 == 0) ? 8 : 4;
        ln_c(x, 62 - h, x, 62 + h, 256, (i == 10) ? C_GOLD_L : C_GOLD_D);
    }
    ln_c(200, 128, 200, 120, 256, C_TURQ);
}

static unsigned pg_still_key(int f)
{
    return (unsigned)f;                     /* face-only: scene dots are live */
}

static void page_t1(void)
{
    int f = face;
    int sc = g_scene;
    ++g_n;
    int v[4];
    for (int i = 0; i < 4; ++i) v[i] = k99(fv[f][i]);

    if (!opus_page_still(me, pg_still_key(f))) {
        ln_c(2, 3, 317, 3, 256, C_GOLD_D);
        ring_at(STAR8, 16, 7, 9, 3, C_GOLD_D);
        ring_at(STAR8, 16, 313, 9, 3, C_GOLD_D);
        opus_page_text(me, 13, 20, 10, 7, OPUS_COLOR_DIM, FACE_NAME[f]);
        opus_page_text(me, 14, 286, 10, 6, OPUS_COLOR_DIM, "v1.11");
        footer_static(f);
        if (f == F_NICHE) {
            polyline_c(ARCH_OUT, 50, C_GOLD);
            polyline_c(ARCH_IN, 50, C_GOLD_D);
            ln_c(82, 117, 238, 117, 256, C_GOLD_D);
            ln_c(82, 118, 238, 118, 256, C_DIM);
            ring_at(STAR8, 16, 77, 33, 4, C_GOLD_D);
            ring_at(STAR8, 16, 243, 33, 4, C_GOLD_D);
            polyline_c(BOWL, 25, C_TURQ);
        } else if (f == F_SPACE) {
            ring_at(RING48, 48, 160, 62, 58, C_DIM);
            ring_at(RING48, 48, 160, 62, 47, C_DIM);
            ring_at(SQ4, 5, 160, 62, 24, C_GOLD);
            ring_at(OCT8, 9, 160, 62, 10, C_GOLD_D);
        } else if (f == F_CROSS) {
            static_cross();
        } else if (f == F_MOD) {
            static_mod();
        } else if (f == F_CORE) {
            static_core();
        } else if (f == F_TUNE) {
            static_tune();
        } else if (f == F_MACRO) {
            static_macro();
        }
        opus_page_still_done(me);
    }
    for (int i = 0; i < 4; ++i) {
        int on = i == sc;
        opus_page_dot(66 + i * 8, 8, 2, on, on ? C_GOLD_L : C_GOLD_D);
    }

    if (f == F_NICHE) {
        draw_niche(v[2], v[3]);
        if (g_tune && g_tuned_k) {
            char tb[8];
            fw_snprintf(tb, sizeof tb, "1/%d", g_tuned_k);
            opus_page_text(me, 21, 208, 22, 7, OPUS_COLOR_TEXT, tb);
        }
    } else if (f == F_SPACE) {
        for (int i = 0; i < 4; ++i) {
            int a = i * 12, b2 = ((i + 1) & 3) * 12;
            ln_c(160 + RING48[a][0] * 34 / 1000, 62 + RING48[a][1] * 34 / 1000,
                 160 + RING48[b2][0] * 34 / 1000, 62 + RING48[b2][1] * 34 / 1000, 256, C_GOLD_D);
        }
        {
            int head = (g_n * 3) & 47;
            opus_page_dot(160 + RING48[head][0] * 58 / 1000, 62 + RING48[head][1] * 58 / 1000, 1, 1, C_GOLD_L);
        }
    } else if (f == F_CROSS) {
        draw_cross(v[0], v[1], v[2], v[3]);
    } else if (f == F_MOD) {
        draw_mod(v[0], v[1], v[2], v[3]);
    } else if (f == F_CORE) {
        draw_core(v[0], v[1], v[2], v[3]);
    } else if (f == F_TUNE) {
        draw_tune(v[0], v[1], v[2], v[3]);
    } else {
        draw_macro(v);
    }

    for (int i = 0; i < g_active && i < 6; ++i)
        opus_page_fill(306, 22 + i * 5, 310, 25 + i * 5, OPUS_COLOR_TEXT);
    if (g_strike > 6000) ln_c(2, 3, 317, 3, 512, C_GOLD_L);
    {
        int fx = 6 + R_filter * 307 / 99;
        ln_c(6, 124, 313, 124, 256, C_DIM);
        ln_c(6, 124, fx, 124, 384, C_GOLD_D);
    }
    footer_live(f, v);
    if (g_msg_timer) {
        opus_page_fill(2, 16, 317, 30, OPUS_COLOR_BACK);
        ln_c(2, 16, 317, 16, 256, C_DIM);
        ln_c(2, 30, 317, 30, 256, C_DIM);
        opus_page_text(me, 20, 100, 20, 7, OPUS_COLOR_TEXT, g_msg);
        --g_msg_timer;
    }
}

/* ---------------------------------------------------------------- the controls */

static void set_msg(const char *s)
{
    int i = 0;
    while (s[i] && i < 11) { g_msg[i] = s[i]; ++i; }
    g_msg[i] = 0;
    g_msg_timer = 90;
}

static void save_data(void)
{
    struct sama_data d;
    { unsigned char *z = (unsigned char *)&d; for (unsigned i = 0; i < sizeof d; ++i) z[i] = 0; }
    d.magic = MAGIC;
    d.version = DATA_VERSION;
    d.face = (short)face;
    d.scene = (short)g_scene;
    for (int f = 0; f < F_N; ++f)
        for (int i = 0; i < 4; ++i)
            d.v[f * 4 + i] = (short)k99(fv[f][i]);
    opus_engine_data_set(&d, sizeof d);
}

static void set_face(int f)
{
    if (f == face) return;
    for (int sp = 0; sp < 100000 && g_load_transition; ++sp) ;
    g_load_knobs_pending = 0;
    g_transition = 1;                       /* audio must not copy until we finish */
    face = (f + F_N) % F_N;
    for (int f2 = 0; f2 < F_N; ++f2)
        for (int i = 0; i < 4; ++i) fvl[f2][i] = fv[f2][i];
    for (int i = 0; i < 4; ++i) opus_engine_knob_set(i, fv[face][i]);
    save_data();
    g_transition = 0;
    set_msg(FACE_NAME[face]);
}

static void load_scene(int n)
{
    n &= (SCENE_N - 1);
    for (int sp = 0; sp < 100000 && g_load_transition; ++sp) ;
    g_load_knobs_pending = 0;
    g_transition = 1;
    g_scene = n;
    for (int f = 0; f < F_N; ++f)
        for (int i = 0; i < 4; ++i)
            fv[f][i] = (short)(cl99(SCENES[n][f * 4 + i]) * 32767 / 99);
    for (int f = 0; f < F_N; ++f)
        for (int i = 0; i < 4; ++i) fvl[f][i] = fv[f][i];
    for (int i = 0; i < 4; ++i) opus_engine_knob_set(i, fv[face][i]);
    save_data();
    g_transition = 0;
    set_msg(SCENE_NAME[n]);
}

/* T3 and T4 page through our faces while T1 stays up; T2/T4 stay the system's pages. */
static int e_key(int key, int pressed, int page)
{
    if (!pressed) return 0;
    if (page != 0) return 0;
    if (key == 0x06) {                         /* T1: FN next scene, shift freeze */
        if (fw_shift_held()) {
            g_freeze = !g_freeze;
            set_msg(g_freeze ? "freeze" : "unfreeze");
            return 1;
        }
        if (opus_fn_held()) {
            load_scene(g_scene + 1);
            return 1;
        }
        return 0;                              /* plain T1 is the OS page key */
    }
    if (key == 0x08) {
        if (opus_fn_held()) {
            g_tune = !g_tune;
            opus_set_setting(me, "tune", g_tune);
            set_msg(g_tune ? "tuned" : "free");
            return 1;
        }
        set_face(face - 1);                                /* T3: previous face on T1 */
        return 1;
    }
    if (key == 0x09) return 0;                              /* T4: the system's LFO page */
    if (key >= 0x3B && key <= 0x3E) { set_face(key == 0x3E ? F_CORE : key - 0x3B); return 1; }
    return 0;
}

static int e_turn(int encoder, int detents, int page)
{
    if (g_load_knobs_pending) {
        for (int i = 0; i < 4; ++i) opus_engine_knob_set(i, fv[face][i]);
        g_load_knobs_pending = 0;
    }
    if (page == 0 && encoder >= 0 && encoder < 4 && detents) {
        int v = k99(fv[face][encoder]) + detents;
        if (v < 0) v = 0; else if (v > 99) v = 99;
        fv[face][encoder] = fvl[face][encoder] = (short)(v * 32767 / 99);
        save_data();                            /* the blob must hold the turn */
    }
    return 0;                                  /* the OS still owns the knob */
}

static void e_setting(const char *name, int value)
{
    if (streq(name, "scale")) g_scale = value < 0 ? 0 : value > 3 ? 3 : value;
    else if (streq(name, "tune")) g_tune = value ? 1 : 0;
    else if (streq(name, "wow")) g_wow = cl99(value);
    else if (streq(name, "keytrack")) g_keytrack = value < 0 ? 0 : value > 100 ? 100 : value;
    else if (streq(name, "diffuse")) g_diffuse = cl99(value);
}

static const struct opus_engine SAMA = {
    .size = sizeof(struct opus_engine),
    .name = "sama",
    .knobs = { 14894, 23168, 30450, 16548, 0, 0, 0, 0 },
    .voice = sizeof(struct voice),
    .init = v_init,
    .note_on = v_note_on,
    .release = v_release,
    .render = v_render,
    .block = b_block,
    .page = page_t1,
    .turn = e_turn,
    .key = e_key,
};

static void clear_room_state(void);

static void e_on_block(void)
{
    if (g_transition) { g_load_state = LOAD_PENDING; return; }   /* retry next block */
    if (g_load_state != LOAD_PENDING) return;
    g_load_state = LOAD_IDLE;
    g_load_transition = 1;
    clear_room_state();
    g_active = 0;
    g_gate = 32767;
    g_gate_next = 32767;
    g_drift = 0;
    g_strike = 0;
    g_freeze = 0;
    g_msg_timer = 0;
    g_min_step = 0xFFFFFFFFu;
    g_root_step = REF_STEP;
    g_tuned_k = 0;
    struct sama_data d;
    { unsigned char *z = (unsigned char *)&d; for (unsigned i = 0; i < sizeof d; ++i) z[i] = 0; }
    int n = opus_engine_data(&d, sizeof d);
    int ok = 0;
    if (n == (int)sizeof d && d.magic == MAGIC && d.version == DATA_VERSION &&
        d.face >= 0 && d.face < F_N) {
        face = d.face;
        g_scene = d.scene;
        if (g_scene < 0) g_scene = 0; else if (g_scene >= SCENE_N) g_scene = SCENE_N - 1;
        for (int f = 0; f < F_N; ++f)
            for (int i = 0; i < 4; ++i)
                fv[f][i] = (short)(cl99(d.v[f * 4 + i]) * 32767 / 99);
        ok = 1;
    } else if (n == 60 && d.magic == MAGIC && d.version == 1 &&
               d.face >= 0 && d.face < 6) {
        face = d.face;
        for (int f = 0; f < 6; ++f)
            for (int i = 0; i < 4; ++i)
                fv[f][i] = (short)(cl99(d.v[f * 4 + i]) * 32767 / 99);
        /* v1 sounds had no tune face: neutral there, not the fresh defaults */
        fv[F_TUNE][0] = 16548; fv[F_TUNE][1] = 0; fv[F_TUNE][2] = 0; fv[F_TUNE][3] = 32767;
        ok = 1;
    }
    if (!ok) {
        face = F_NICHE;
        for (int f = 0; f < F_N; ++f)
            for (int i = 0; i < 4; ++i)
                fv[f][i] = (short)(SCENES[1][f * 4 + i] * 32767 / 99);
        g_master = 65536;
        g_air_state = 0;
        hlp = 0;
        g_scene = 1;
    }
    /* The DSP and page read the live shadow; without this every saved sound
     * played its non-active faces from the factory defaults (1.1.4 break). */
    for (int f = 0; f < F_N; ++f)
        for (int i = 0; i < 4; ++i)
            fvl[f][i] = fv[f][i];
    g_load_transition = 0;
    g_load_knobs_pending = 1;
}

static void clear_room_state(void)
{
    if (line) for (unsigned i = 0; i < DMASK; ++i) line[i] = 0;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 64; ++j) room[i][j] = 0;
    for (int i = 0; i < APLEN; ++i) ap[i] = 0;
    widx = 0;
    apw = 0;
    tbase = TIMETAB[55];
    wlp = 0;
    ic1 = ic2 = dcz = hlp = e1 = e2 = 0;
    g_subph = 0;
    g_wowph = 0;
    g_bph = 0;
    g_air_state = 0;
    g_out_dc = 0;
    g_act_mask = 0;
    g_master = 65536;
}

int opl_init(struct opl_self *self)
{
    me = self;
    line = opus_alloc_audio(self, DMASK * sizeof(short));
    if (!line) opus_log(self, "sama: no delay memory, dry");
    clear_room_state();
    g_scale = opus_setting(self, "scale"); if (g_scale < 0 || g_scale > 3) g_scale = 0;
    g_tune = opus_setting(self, "tune") ? 1 : 0;
    g_wow = cl99(opus_setting(self, "wow"));
    g_keytrack = opus_setting(self, "keytrack"); if (g_keytrack < 0) g_keytrack = 0; if (g_keytrack > 100) g_keytrack = 100;
    g_diffuse = cl99(opus_setting(self, "diffuse"));
    g_root_step = REF_STEP;
    g_min_step = 0xFFFFFFFF;
    g_subph = 0;
    g_wowph = 0;
    g_bph = 0;
    g_breath = 16384;
    g_gate = 32767;
    g_gate_next = 32767;
    g_vgain = 32767;
    g_tuned_k = 0;
    g_room_sel = 0;
    if (opus_engine_add(self, &SAMA) < 0) return -1;
    opus_on_block(self, e_on_block);
    opus_on_setting(self, e_setting);
    return 0;
}
