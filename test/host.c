/* host harness: the plugin's own DSP, rendered on this machine.
 * Builds against the kit's headers; every opus_/fw_ call it makes is stubbed here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "opus_plugin.h"
#include "op1fw.h"

static const struct opus_engine *ENG;
static unsigned char data[256];
static int data_n;
static short *delay_mem;

int opus_engine_add(struct opl_self *s, const struct opus_engine *e) { (void)s; ENG = e; return 1; }
void *opus_alloc_audio(struct opl_self *s, unsigned n) { (void)s; return delay_mem = calloc(1, n); }
void *opus_alloc(struct opl_self *s, unsigned n) { (void)s; return calloc(1, n); }
void opus_log(struct opl_self *s, const char *t) { (void)s; fprintf(stderr, "log: %s\n", t); }
static unsigned char data_dummy[4];
int opus_engine_data(void *b, int max) { int n = data_n < max ? data_n : max; if (n > 0) memcpy(b, data, n); return n; }
int opus_engine_data_set(const void *b, int n) { if (n > 0 && n <= 256) { memcpy(data, b, n); data_n = n; } return 1; }
int opus_page_still(struct opl_self *s, unsigned k) { (void)s; (void)k; return 0; }
void opus_page_still_done(struct opl_self *s) { (void)s; }
void opus_page_text(struct opl_self *s, int a, int b, int c, int d, int e, const char *t) { (void)s;(void)a;(void)b;(void)c;(void)d;(void)e;(void)t; }
void opus_page_text_right(struct opl_self *s, int a, int b, int c, int d, int e, const char *t) { (void)s;(void)a;(void)b;(void)c;(void)d;(void)e;(void)t; }
void opus_page_fill(int a, int b, int c, int d, int e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void opus_page_stroke_line(int a, int b, int c, int d, int e, int f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void opus_page_dot(int a, int b, int c, int d, int e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void opus_page_path(void) {}
void opus_page_move(int a, int b) { (void)a;(void)b; }
void opus_page_line_to(int a, int b) { (void)a;(void)b; }
void opus_page_close(void) {}
void opus_page_fill_path(int a, int b) { (void)a;(void)b; }
void opus_page_stroke(int a, int b, int c, int d, int e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
unsigned opus_accent_now(int i) { (void)i; return 0; }
int opus_setting(struct opl_self *s, const char *n) { (void)s;(void)n; return 0; }

int fw_snprintf(char *d, unsigned n, const char *f, ...) { (void)n; (void)f; strcpy(d, "0"); return 1; }
int fw_shift_held(void) { return 0; }
int opus_fn_held(void) { return 0; }

#include "../sama.c"

int opus_engine_live(short par[8]) { for (int i = 0; i < 4; ++i) par[i] = fv[face][i]; for (int i = 4; i < 8; ++i) par[i] = 0; return 1; }
int opus_engine_knobs(short par[8]) { return opus_engine_live(par); }
int opus_engine_knob_set(int k, int v) { if (k >= 0 && k < 4) fv[face][k] = (short)v; return 1; }


static short KT = 18000, KM = 8000, KD = 24000, KF = 13000;
static struct voice VO[6];
static int active[6];
static double peak;
static long zero_cross;

static void block_out(short *out, int n, FILE *w)
{
    short buf[64];
    for (int i = 0; i < n; i += 64) {
        short mix[64] = { 0 };
        for (int v = 0; v < 6; ++v) {
            if (!active[v]) continue;
            short vo[128];
            unsigned pitch[128];
            for (int s = 0; s < 128; ++s) pitch[s] = 21428571u;   /* 440 Hz at 88.2k */
            short par[8] = { fv[face][0], fv[face][1], fv[face][2], fv[face][3], 0, 0, 0, 0 };
            ENG->render(&VO[v], vo, par, pitch);
            for (int s = 0; s < 64; ++s) {
                int x = (vo[2 * s] + vo[2 * s + 1]) / 2;
                if (x > 32767) x = 32767;
                if (x < -32768) x = -32768;
                mix[s] = (short)x;
            }
        }
        ENG->block(mix);
        for (int v = 0; v < 6; ++v)
            if (active[v] && ENG->finished && ENG->finished(&VO[v])) active[v] = 0;
        for (int s = 0; s < 64; ++s) {
            int x = mix[s];
            if (abs(x) > peak) peak = abs(x);
            if (s && ((out[i + s - 1] < 0) != (x < 0))) ++zero_cross;
            out[i + s] = (short)x;
        }
        fwrite(mix, 2, 64, w);
    }
    (void)active;
}


/* usage: host out.wav tone time decay filter absorb breath warp drift
 *        attack hold release cross   (0-99; 32767-style knob values are derived) */
int main(int argc, char **argv)
{
    struct opl_self self;
    memset(&self, 0, sizeof self);
    memset(&VO, 0, sizeof VO);
    opl_init(&self);
    if (!ENG) { fprintf(stderr, "no engine\n"); return 1; }

    int a[16] = { 55, 61, 88, 54, 20, 36, 40, 30, 30, 88, 78, 30, 35, 30, 10, 40 };
    for (int i = 0; i < 16 && i + 2 < argc; ++i) a[i] = atoi(argv[i + 2]);
    for (int i = 0; i < 4; ++i) {
        fv[F_NICHE][i]  = (short)(a[i] * 32767 / 99);
        fv[F_SPACE][i]  = (short)(a[4 + i] * 32767 / 99);
        fv[F_KINDLE][i] = (short)(a[8 + i] * 32767 / 99);
        fv[F_CROSS][i]  = (short)(a[12 + i] * 32767 / 99);
    }
    fprintf(stderr, "args %d %d %d %d | fv0 %d %d %d %d | face %d | TT %d %d %d\n",
            a[0], a[1], a[2], a[3], fv[0][0], fv[0][1], fv[0][2], fv[0][3], face,
            TIMETAB[0], TIMETAB[30], TIMETAB[48]);
    for (int v = 0; v < 6; ++v) ENG->init(&VO[v], v);

    int frames = 44100 * 8;
    short *buf = calloc(frames, 2);
    FILE *w = fopen(argv[1], "wb");
    int held = 0;
    for (int i = 0; i < frames; i += 64) {
        if (i >= 44100 / 5 && !held) {
            for (int v = 0; v < 6; ++v) { if (ENG->note_on) ENG->note_on(&VO[v], NULL); active[v] = 1; }
            held = 1;
        }
        if (i >= 44100 * 3 && held == 1) {
            for (int v = 0; v < 6; ++v) if (ENG->release) ENG->release(&VO[v], NULL);
            held = 2;
        }
        { static int last = -1; int sec = i / 44100;
          if (sec != last) { last = sec;
            fprintf(stderr, "t=%d R_hold=%u stage=%u env=%d hold=%u R_att=%d R_rel=%d\n",
                    sec, R_hold, VO[0].stage, VO[0].env, VO[0].hold, R_att, R_rel); } }
        block_out(buf + i, 64, w);
    }
    fclose(w);
    {
        FILE *f = fopen(argv[1], "r+b");
        fseek(f, 0, SEEK_END);
        int n = ftell(f) / 2;
        fseek(f, 0, SEEK_SET);
        short *pcm = calloc(n, 2);
        fread(pcm, 2, n, f);
        fclose(f);
        char path[512]; snprintf(path, sizeof path, "%s.wav", argv[1]);
        FILE *o = fopen(path, "wb");
        int sr = 44100, ch = 1, bits = 16, bytes = n * 2;
        (void)ch; (void)bits;
        fwrite("RIFF", 1, 4, o); fwrite(&(int){ 36 + bytes }, 4, 1, o);
        fwrite("WAVEfmt ", 1, 8, o); fwrite(&(int){ 16 }, 4, 1, o);
        fwrite(&(short){ 1 }, 2, 1, o); fwrite(&(short){ 1 }, 2, 1, o);
        fwrite(&sr, 4, 1, o); fwrite(&(int){ sr * 2 }, 4, 1, o);
        fwrite(&(short){ 2 }, 2, 1, o); fwrite(&(short){ 16 }, 2, 1, o);
        fwrite("data", 1, 4, o); fwrite(&bytes, 4, 1, o);
        fwrite(pcm, 2, n, o); fclose(o);
        {
        double r1 = 0, r2 = 0, r3 = 0; long n1 = 0, n2 = 0, n3 = 0;
        for (int i = 44100; i < 44100 * 2; ++i) { r1 += (double)pcm[i] * pcm[i]; ++n1; }
        for (int i = 44100 * 2; i < 44100 * 3; ++i) { r2 += (double)pcm[i] * pcm[i]; ++n2; }
        for (int i = 44100 * 5; i < 44100 * 6; ++i) { r3 += (double)pcm[i] * pcm[i]; ++n3; }
        printf("peak %d rms1 %.0f rms2 %.0f rms5 %.0f\n", (int)peak,
               sqrt(r1 / n1), sqrt(r2 / n2), sqrt(r3 / n3));
    }
    }
    return 0;
}
