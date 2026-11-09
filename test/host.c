/* host: compile the real sama.c against stub API calls and render it here.
 *
 * The harness mimics the core's geometry: six voices at 88.2 kHz, summed, decimated to
 * 44.1 kHz, then one block() call of 64 samples per 64 output samples.
 *
 *   ./host out.wav [20 k99 values for the first five faces]
 *
 * Environment (tests use these):
 *   SAMA_VOICES=1..6        active voices (default 1)
 *   SAMA_CHORD=0,4,7,...    per-voice semitone offsets (default 0,4,7,12,16,19)
 *   SAMA_MACRO=a,b,c,d      the macro face values 0..99 (default 50,50,50,50)
 *   SAMA_SCALE=0..3         chromatic, major, minor, pentatonic
 *   SAMA_TUNE=0|1           tuned room
 *   SAMA_WOW=0..99          tape wow
 *   SAMA_KEYTRACK=0..100    filter key tracking
 *   SAMA_DIFFUSE=0..99      loop diffusion
 *   SAMA_FREEZE_AT=sec      shift+T1 at that time
 *   SAMA_SCENE_AT=sec       T1 (next scene) at that time
 *   SAMA_PLAY=sec           note length (default 2)
 *   SAMA_TOTAL=sec          render length (default 8)
 *   SAMA_PERSIST=1          run the save/load face test and print "persist 1"
 *
 * It prints one line of measurements for the regression suite:
 *   peak <n> rms1 <n> rms2 <n> rms5 <n> zc <n> clip <n>
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opus_plugin.h"
#include "op1fw.h"

static const struct opus_engine *ENG;
static short VO[6][16];                /* the engine voice is 20 bytes; core gives 32 */
static unsigned char DATA[256];
static int DATA_N;
static int HOST_SHIFT;
static int HOST_FN;

static int env_int(const char *name, int dflt)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : dflt;
}

static double env_dbl(const char *name, double dflt)
{
    const char *v = getenv(name);
    return v && *v ? atof(v) : dflt;
}

int opus_engine_add(struct opl_self *s, const struct opus_engine *e) { (void)s; ENG = e; return 1; }
void *opus_alloc_audio(struct opl_self *s, unsigned n) { (void)s; return calloc(1, n); }
void *opus_alloc(struct opl_self *s, unsigned n) { (void)s; return calloc(1, n); }
void opus_log(struct opl_self *s, const char *t) { (void)s; (void)t; }

int opus_engine_data(void *b, int max)
{
    int n = DATA_N;
    if (n > max) n = max;
    if (n > 0) memcpy(b, DATA, (size_t)n);
    return n;
}
int opus_engine_data_set(const void *b, int n)
{
    if (n < 0) n = 0;
    if (n > (int)sizeof DATA) n = (int)sizeof DATA;
    memcpy(DATA, b, (size_t)n);
    DATA_N = n;
    return 1;
}

int opus_setting(struct opl_self *s, const char *n)
{
    (void)s;
    if (!strcmp(n, "scale")) return env_int("SAMA_SCALE", 0);
    if (!strcmp(n, "tune")) return env_int("SAMA_TUNE", 0);
    if (!strcmp(n, "wow")) return env_int("SAMA_WOW", 18);
    if (!strcmp(n, "keytrack")) return env_int("SAMA_KEYTRACK", 70);
    if (!strcmp(n, "diffuse")) return env_int("SAMA_DIFFUSE", 30);
    return 0;
}
void opus_on_setting(struct opl_self *s, void (*fn)(const char *, int)) { (void)s; (void)fn; }
int opus_set_setting(struct opl_self *s, const char *n, int v) { (void)s; (void)n; (void)v; return 1; }
void opus_on_block(struct opl_self *s, void (*fn)(void)) { (void)s; (void)fn; }
int fw_snprintf(char *d, unsigned n, const char *f, ...)
{
    if (n) d[0] = 0;
    (void)f;
    return 0;
}
int fw_shift_held(void) { return HOST_SHIFT; }
int opus_fn_held(void) { return HOST_FN; }

void opus_page_stroke_line(int a, int b, int c, int d, int e, int f)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void opus_page_fill(int a, int b, int c, int d, int e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void opus_page_dot(int a, int b, int c, int d, int e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void opus_page_text(struct opl_self *s, int a, int b, int c, int d, int e, const char *f)
{ (void)s;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void opus_page_text_right(struct opl_self *s, int a, int b, int c, int d, int e, const char *f)
{ (void)s;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void opus_page_path(void) {}
void opus_page_move(int a, int b) { (void)a;(void)b; }
void opus_page_line_to(int a, int b) { (void)a;(void)b; }
void opus_page_close(void) {}
void opus_page_fill_path(int a, int b) { (void)a;(void)b; }
void opus_page_stroke(int a, int b, int c, int d, int e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
unsigned opus_accent_now(int i) { (void)i; return 0; }
int opus_page_still(struct opl_self *s, unsigned k) { (void)s; (void)k; return 0; }
void opus_page_still_done(struct opl_self *s) { (void)s; }
unsigned short *opus_page_pixels(int *rows) { (void)rows; return 0; }
void opus_sincos(int t, int *s, int *c) { (void)t; *s = 0; *c = 4096; }
int opus_page_art(struct opl_self *s, int p, const char *f) { (void)s;(void)p;(void)f; return 0; }
int opus_art_text(struct opl_self *s, const char *n, const char *v, unsigned l)
{ (void)s;(void)n;(void)v;(void)l; return -1; }

#include "../sama.c"

int opus_engine_knob_set(int k, int v) { if (k >= 0 && k < 4) fv[face][k] = (short)v; return 1; }
int opus_engine_live(short par[8])
{
    for (int i = 0; i < 4; ++i) par[i] = fv[face][i];
    for (int i = 4; i < 8; ++i) par[i] = 0;
    return 1;
}
int opus_engine_knobs(short par[8]) { return opus_engine_live(par); }

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: host out.wav [20 k99 values]\n");
        return 2;
    }
    struct opl_self self;
    memset(&self, 0, sizeof self);
    opl_init(&self);

    for (int v = 0; v < 6; ++v) ENG->init(VO[v], v);

    int a[20];
    for (int i = 0; i < 20; ++i) a[i] = 0;
    for (int i = 0; i < 20 && i + 2 < argc; ++i) a[i] = atoi(argv[i + 2]);
    for (int f = 0; f < F_N && f < 5; ++f)
        for (int i = 0; i < 4; ++i)
            fv[f][i] = (short)(a[f * 4 + i] * 32767 / 99);
    {
        const char *m = getenv("SAMA_MACRO");
        int mv[4] = { 50, 50, 50, 50 };
        if (m && *m) sscanf(m, "%d,%d,%d,%d", &mv[0], &mv[1], &mv[2], &mv[3]);
        for (int i = 0; i < 4; ++i) {
            if (mv[i] < 0) mv[i] = 0; else if (mv[i] > 99) mv[i] = 99;
            fv[F_MACRO][i] = (short)(mv[i] * 32767 / 99);
        }
    }
    for (int f = 0; f < F_N; ++f)
        for (int i = 0; i < 4; ++i) fvl[f][i] = fv[f][i];
    {
        /* init() resets to the factory defaults (new-sound fix); apply the requested
         * face values as the OS knobs and let the block seed the live shadow. */
        short seed[64] = { 0 };
        ENG->block(seed);
    }

    if (env_int("SAMA_KEYTEST", 0)) {
        int a = ENG->key(0x09, 1, 1);      /* T4 on T2: the system's page */
        int b = ENG->key(0x09, 1, 0);      /* T4 on T1: the system's page */
        int c = ENG->key(0x08, 1, 0);      /* T3 on T1: our page */
        int d = ENG->key(0x06, 1, 0);      /* plain T1: the OS page key */
        printf("keypage_t2 %d\nkeypage_t1 %d\nkeypage_t3 %d\nkeypage_t1key %d\n", a, b, c, d);
        return 0;
    }

    int nv = env_int("SAMA_VOICES", 1);
    if (nv < 1) nv = 1; else if (nv > 6) nv = 6;

    int offs[6] = { 0, 4, 7, 12, 16, 19 };
    {
        const char *c = getenv("SAMA_CHORD");
        if (c && *c) {
            char buf[80];
            strncpy(buf, c, sizeof buf - 1);
            buf[sizeof buf - 1] = 0;
            int i = 0;
            char *tok = strtok(buf, ",");
            while (tok && i < 6) { offs[i++] = atoi(tok); tok = strtok(0, ","); }
        }
    }
    unsigned pitch[6][128];
    for (int v = 0; v < 6; ++v) {
        double hz = 440.0 * pow(2.0, offs[v] / 12.0);
        unsigned step = (unsigned)(hz * 4294967296.0 / 88200.0);
        for (int i = 0; i < 128; ++i) pitch[v][i] = step;
    }

    double play = env_dbl("SAMA_PLAY", 2.0);
    double total = env_dbl("SAMA_TOTAL", 8.0);
    int total_n = (int)(total * 44100.0);
    if (total_n < 64) total_n = 64;
    double freeze_at = env_dbl("SAMA_FREEZE_AT", -1.0);
    double scene_at = env_dbl("SAMA_SCENE_AT", -1.0);
    int play_n = (int)(play * 44100.0);

    short par[8];
    memset(par, 0, sizeof par);
    for (int v = 0; v < nv; ++v) ENG->note_on(VO[v], par);

    short *pcm = calloc((size_t)total_n, sizeof(short));
    FILE *wav = fopen(argv[1], "wb");
    int released = 0, froze = 0, scened = 0;

    for (int i = 0; i + 64 <= total_n; i += 64) {
        if (!released && i >= play_n) {
            for (int v = 0; v < nv; ++v) ENG->release(VO[v], par);
            released = 1;
        }
        if (freeze_at >= 0.0 && !froze && i >= (int)(freeze_at * 44100.0)) {
            HOST_SHIFT = 1;
            ENG->key(0x06, 1, 0);
            HOST_SHIFT = 0;
            froze = 1;
        }
        if (scene_at >= 0.0 && !scened && i >= (int)(scene_at * 44100.0)) {
            HOST_SHIFT = 0;
            HOST_FN = 1;
            ENG->key(0x06, 1, 0);          /* FN+T1: next scene */
            HOST_FN = 0;
            scened = 1;
        }

        int sum[128];
        for (int k = 0; k < 128; ++k) sum[k] = 0;
        if (i < play_n) {
            for (int v = 0; v < nv; ++v) {
                short buf[128];
                ENG->render(VO[v], buf, par, pitch[v]);
                for (int k = 0; k < 128; ++k) sum[k] += buf[k];
            }
        }
        short mix[64];
        for (int k = 0; k < 64; ++k) {
            int s = (sum[2 * k] + sum[2 * k + 1]) / 2;
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            mix[k] = (short)s;
        }
        ENG->block(mix);
        for (int k = 0; k < 64; ++k) pcm[i + k] = mix[k];
    }

    long peak = 0, zc = 0, clipped = 0;
    for (int i = 0; i < total_n; ++i) {
        long v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > peak) peak = v;
        if (v >= 32767) ++clipped;
        if (i && ((pcm[i - 1] < 0) != (pcm[i] < 0))) ++zc;
    }
    double r1 = 0, r2 = 0, r5 = 0;
    for (int i = 44100; i < 88200 && i < total_n; ++i) r1 += (double)pcm[i] * pcm[i];
    for (int i = 88200; i < 132300 && i < total_n; ++i) r2 += (double)pcm[i] * pcm[i];
    for (int i = 220500; i < 264600 && i < total_n; ++i) r5 += (double)pcm[i] * pcm[i];
    printf("peak %ld rms1 %.0f rms2 %.0f rms5 %.0f zc %ld clip %ld\n",
           peak, sqrt(r1 / 44100.0), sqrt(r2 / 44100.0), sqrt(r5 / 44100.0), zc, clipped);

    if (env_int("SAMA_PERSIST", 0)) {
        int exp_face = F_CROSS;
        set_face(exp_face);
        int exp0 = k99(fv[exp_face][0]);
        int exp3 = k99(fv[exp_face][3]);
        for (int f = 0; f < F_N; ++f)
            for (int i = 0; i < 4; ++i) fv[f][i] = 0;
        face = F_NICHE;
        v_init(VO[0], 0);
        e_on_block();                       /* the sequencer-thread load */
        int ok = (face == exp_face && k99(fv[exp_face][0]) == exp0 && k99(fv[exp_face][3]) == exp3);
        printf("persist %d\n", ok);
    }

    fwrite("RIFF", 1, 4, wav); put32(wav, 36 + (unsigned)total_n * 2); fwrite("WAVEfmt ", 1, 8, wav);
    put32(wav, 16); put16(wav, 1); put16(wav, 1); put32(wav, 44100); put32(wav, 88200);
    put16(wav, 2); put16(wav, 16); fwrite("data", 1, 4, wav); put32(wav, (unsigned)total_n * 2);
    fwrite(pcm, 2, (size_t)total_n, wav);
    fclose(wav);
    return 0;
}
