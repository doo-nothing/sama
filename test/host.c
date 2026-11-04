/* host: compile the real sama.c against stub API calls and render it here.
 *
 * The harness mimics the core's geometry: six voices at 88.2 kHz, summed, decimated to
 * 44.1 kHz, then one block() call of 64 samples per 64 output samples.
 *
 *   ./host out.wav [20 k99 values for the five faces]
 *
 * It prints one line of measurements for the regression suite:
 *   peak <n> rms1 <n> rms2 <n> rms5 <n> zc <n>
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opus_plugin.h"
#include "op1fw.h"

static const struct opus_engine *ENG;
static short VO[6][8];
static int act[6] = { 1, 0, 0, 0, 0, 0 };           /* one voice unless asked */

int opus_engine_add(struct opl_self *s, const struct opus_engine *e) { (void)s; ENG = e; return 1; }
void *opus_alloc_audio(struct opl_self *s, unsigned n) { (void)s; return calloc(1, n); }
void *opus_alloc(struct opl_self *s, unsigned n) { (void)s; return calloc(1, n); }
void opus_log(struct opl_self *s, const char *t) { (void)s; (void)t; }
int opus_engine_data(void *b, int max) { (void)b; (void)max; return 0; }
int opus_engine_data_set(const void *b, int n) { (void)b; (void)n; return 0; }
int opus_setting(struct opl_self *s, const char *n) { (void)s; (void)n; return 0; }
int fw_snprintf(char *d, unsigned n, const char *f, ...) { if (n) d[0] = 0; return 0; }
int fw_shift_held(void) { return 0; }
int opus_fn_held(void) { return 0; }

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
    struct opl_self self;
    memset(&self, 0, sizeof self);
    opl_init(&self);

    int a[20];
    for (int i = 0; i < 20; ++i) a[i] = 0;
    for (int i = 0; i < 20 && i + 2 < argc; ++i) a[i] = atoi(argv[i + 2]);
    for (int f = 0; f < F_N && f < 5; ++f)
        for (int i = 0; i < 4; ++i)
            fv[f][i] = (short)(a[f * 4 + i] * 32767 / 99);

    for (int v = 0; v < 6; ++v) ENG->init(VO[v], v);

    unsigned pitch[128];
    unsigned step = (unsigned)(440.0 * 4294967296.0 / 88200.0);
    for (int i = 0; i < 128; ++i) pitch[i] = step;

    const int total = 44100 * 8;
    short *pcm = calloc(total, sizeof(short));
    FILE *wav = fopen(argv[1], "wb");

    for (int i = 0; i < total; i += 64) {
        short sum[128], mix[64];
        for (int k = 0; k < 128; ++k) sum[k] = 0;
        int playing = i < 2 * 44100;                     /* two seconds of notes, then silence */
        for (int v = 0; v < 6; ++v) {
            if (!act[v] || !playing) continue;
            short buf[128];
            short par[8];
            for (int k = 0; k < 7; ++k) par[k] = 0;
            par[7] = 32767;
            ENG->render(VO[v], buf, par, pitch);
            for (int k = 0; k < 128; ++k) sum[k] += buf[k];
        }
        for (int k = 0; k < 64; ++k) mix[k] = (short)((sum[2 * k] + sum[2 * k + 1]) / 2);
        ENG->block(mix);
        for (int k = 0; k < 64; ++k) pcm[i + k] = mix[k];
    }

    long peak = 0, zc = 0;
    for (int i = 0; i < total; ++i) {
        long v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > peak) peak = v;
        if (i && ((pcm[i - 1] < 0) != (pcm[i] < 0))) ++zc;
    }
    double r1 = 0, r2 = 0, r3 = 0;
    for (int i = 44100; i < 88200; ++i) r1 += (double)pcm[i] * pcm[i];
    for (int i = 88200; i < 132300; ++i) r2 += (double)pcm[i] * pcm[i];
    for (int i = 220500; i < 264600; ++i) r3 += (double)pcm[i] * pcm[i];
    printf("peak %ld rms1 %.0f rms2 %.0f rms5 %.0f zc %ld\n",
           peak, sqrt(r1 / 44100), sqrt(r2 / 44100), sqrt(r3 / 44100), zc);

    fwrite("RIFF", 1, 4, wav); put32(wav, 36 + total * 2); fwrite("WAVEfmt ", 1, 8, wav);
    put32(wav, 16); put16(wav, 1); put16(wav, 1); put32(wav, 44100); put32(wav, 88200);
    put16(wav, 2); put16(wav, 16); fwrite("data", 1, 4, wav); put32(wav, total * 2);
    fwrite(pcm, 2, total, wav);
    fclose(wav);
    return 0;
}
