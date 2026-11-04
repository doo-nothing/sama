/* render: run the plugin's own page functions on the host and dump a display
 * list, so the README's screenshots come from the real drawing code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../opus-kit/plugin-kit/sdk/opus_plugin.h"
#include "../../../opus-kit/plugin-kit/sdk/op1fw.h"

static FILE *dl;
static const struct opus_engine *ENG;
static unsigned char dmy[8];
static int pt[8][2], ptn, path_open;
static int last_x, last_y, have_last;

int opus_engine_add(struct opl_self *s, const struct opus_engine *e) { (void)s; ENG = e; return 1; }
void *opus_alloc_audio(struct opl_self *s, unsigned n) { (void)s; return calloc(1, n); }
void *opus_alloc(struct opl_self *s, unsigned n) { (void)s; return calloc(1, n); }
void opus_log(struct opl_self *s, const char *t) { (void)s; (void)t; }
int opus_engine_data(void *b, int max) { (void)b; (void)max; return 0; }
int opus_engine_data_set(const void *b, int n) { (void)b; (void)n; return 0; }
int opus_engine_knob_set(int k, int v) { (void)k; (void)v; return 1; }
int opus_setting(struct opl_self *s, const char *n) { (void)s; (void)n; return 0; }
#include <stdarg.h>
int fw_snprintf(char *d, unsigned n, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(d, n, f, ap); va_end(ap); return r; }
int fw_shift_held(void) { return 0; }
int opus_fn_held(void) { return 0; }

/* ---- the drawing calls, recorded ---- */
void opus_page_stroke_line(int x0, int y0, int x1, int y1, int w, int c)
{
    if (!path_open) fprintf(dl, "L %d %d %d %d %d %d\n", x0 / 256, y0 / 256, x1 / 256, y1 / 256, w / 256, c);
}
void opus_page_fill(int x0, int y0, int x1, int y1, int c) { fprintf(dl, "R %d %d %d %d %d\n", x0, y0, x1, y1, c); }
void opus_page_dot(int x, int y, int r, int filled, int c) { fprintf(dl, "D %d %d %d %d %d\n", x, y, r, filled, c); }
void opus_page_text(struct opl_self *s, int slot, int x, int y, int size, int c, const char *t)
{ (void)s; (void)slot; fprintf(dl, "T %d %d %d %d %s\n", x, y, size, c, t); }
void opus_page_text_right(struct opl_self *s, int slot, int x, int y, int size, int c, const char *t)
{ (void)s; (void)slot; fprintf(dl, "TR %d %d %d %d %s\n", x, y, size, c, t); }
void opus_page_path(void) { ptn = 0; path_open = 1; }
void opus_page_move(int x, int y) { if (ptn < 8) { pt[ptn][0] = x / 256; pt[ptn][1] = y / 256; ++ptn; } }
void opus_page_line_to(int x, int y) { if (ptn < 8) { pt[ptn][0] = x / 256; pt[ptn][1] = y / 256; ++ptn; } }
void opus_page_close(void) {}
void opus_page_fill_path(int c, int a)
{
    (void)a;
    if (ptn < 3) return;
    int x0 = pt[0][0], y0 = pt[0][1], x1 = x0, y1 = y0;
    for (int i = 1; i < ptn; ++i) {
        if (pt[i][0] < x0) x0 = pt[i][0];
        if (pt[i][0] > x1) x1 = pt[i][0];
        if (pt[i][1] < y0) y0 = pt[i][1];
        if (pt[i][1] > y1) y1 = pt[i][1];
    }
    fprintf(dl, "R %d %d %d %d %d\n", x0, y0, x1, y1, c);
    path_open = 0;
}
void opus_page_stroke(int a, int b, int c, int d, int e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
unsigned opus_accent_now(int i) { (void)i; return 0; }
void opus_page_line(int x0, int y0, int x1, int y1, int c) { (void)x0;(void)y0;(void)x1;(void)y1;(void)c; }
void opus_page_frame(int a, int b, int c, int d, int e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void opus_page_bar(int a, int b, int c, int d, unsigned e, unsigned f, int g) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g; }
int opus_page_still(struct opl_self *s, unsigned k) { (void)s; (void)k; return 0; }
void opus_page_still_done(struct opl_self *s) { (void)s; }
unsigned short *opus_page_pixels(int *rows) { (void)rows; return 0; }
void opus_sincos(int t, int *s, int *c) { (void)t; *s = 0; *c = 4096; }
int opus_page_art(struct opl_self *s, int p, const char *f) { (void)s;(void)p;(void)f; return 0; }
int opus_art_text(struct opl_self *s, const char *n, const char *v, unsigned l) { (void)s;(void)n;(void)v;(void)l; return -1; }

#include "../sama.c"

int opus_engine_live(short par[8]) { for (int i = 0; i < 4; ++i) par[i] = fv[face][i]; for (int i = 4; i < 8; ++i) par[i] = 0; return 1; }
int opus_engine_knobs(short par[8]) { return opus_engine_live(par); }


int main(int argc, char **argv)
{
    struct opl_self self;
    memset(&self, 0, sizeof self);
    dl = fopen(argv[1], "w");
    opl_init(&self);
    static short k[4] = { 18204, 20190, 29126, 17873 };
    for (int i = 0; i < 4; ++i) fv[0][i] = k[i];
    for (int f = 0; f < F_N; ++f) {
        face = f;
        fprintf(dl, "PAGE %d\n", f);
        page_t1();
    }
    fclose(dl);
    return 0;
}
