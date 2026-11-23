/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/pccard_test.c - the PC card on the host: emu68/src/psmac/psmac_pc.c itself (its start, slices, input ring and
 * drawing) with Aurora86's host library (libaurora86.a for x86-64), drawing into a pretend Mac screen, the way the
 * kernel runs it on a Pi core (docs/19-pc-card.md).
 *
 * Backstory (23 November 2026). The card was written while the shared QEMU was busy with other work, and QEMU's
 * Pi is slow at Bochs (21 MIPS) and has no one-bit 512 x 342 screen on the solo board. So the card's own file runs here
 * too, compiled with PSMAC_PC_HOST: the kernel's few services are a page of C below (options, files through stdio for
 * the testbed's semihosting, a screen in memory at the depth asked, the clock), the place is set as Aurora68 PC sets it
 * (pc.testwin), keys go into the ring as the program puts them (Mac virtual key codes, down and up), and the screen is
 * written out as a picture (PGM for one bit, PPM otherwise; tools/pccard_test.sh makes PNGs).
 *
 *   pccard_test ROM VGAROM DISK|- WxHxDEPTH OUT.pnm SECONDS [WAIT_TEXT KEYS]...
 * A step "WAIT_TEXT KEYS": when WAIT_TEXT is on the PC's text screen, type KEYS (letters, digits, space; \r Return,
 * \e Escape, \d Down) into the ring; a last WAIT_TEXT alone is only waited for. The run ends when every step is done,
 * or after SECONDS. Exit 0 when every step's text was seen.
 *
 * The program's half (23 November 2026, PCCARD_PROG=1): instead of pc.testwin the place is given as Aurora68 PC gives
 * it, with the window's visRgn as a handle into a pretend Mac memory (a heap zone, its master pointer and the region's
 * block, as psmac_wclip_rgn_ok reads them), prog = A68P_PROG_LIVE and alive moving every 100 ms. PCCARD_TWO=1 adds a
 * second screen, 1024 x 768 at 8 bits, as screen 1 (HDMI on the SE board), and PCCARD_WIN=S,L,T puts the window on
 * screen S at L,T. Keys "@quit" (the program quits: gone, A68P_C_NOWINDOW), "@stall" (its loop stops: alive stands
 * still), "@crash" (its heap goes: the region's block freed, alive still), "@free" (the block freed, the loop going on:
 * the live-region check alone), each followed by a check that nothing at all
 * is drawn on either screen any more, and "@live" (a program again: LIVE, alive, the place), followed by a check that
 * the window is drawn again and nothing outside it. At the end, with two screens: the window drawn on its screen only,
 * and only inside its rectangle (the owner's SE, its own screen main and HDMI beside it: the card drew outside any
 * window after Aurora68 PC had quit).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "psmac_pc.h"
#include "aurora86.h"

/* ---- the kernel around the card ---- */
void *tlsf = (void *)1;
void *tlsf_init_with_memory(void *m, uintptr_t n) { (void)n; return m; }
void *tlsf_malloc(void *h, uintptr_t n) { (void)h; return calloc(1, n); }
void *tlsf_malloc_aligned(void *h, uintptr_t n, uintptr_t a) { (void)h; void *p = 0; if (posix_memalign(&p, a < 16 ? 16 : a, n ? n : 1)) return 0; memset(p, 0, n); return p; }
void tlsf_free(void *h, void *p) { (void)h; free(p); }
uintptr_t tlsf_get_total_size(void *h) { (void)h; return 0; }
uintptr_t tlsf_get_free_size(void *h) { (void)h; return 0; }
uint64_t pc_host_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }
int psmac_board_se, psmac_board_solo = 1;
static char opts[16][200];
static int nopts;
const char *psmac_option(const char *key)
{
    size_t n = strlen(key);
    for (int i = 0; i < nopts; i++) if (!strncmp(opts[i], key, n) && opts[i][n] == '=') return opts[i] + n + 1;
    return 0;
}
void psmac_mmu_set_pc(uintptr_t w, uintptr_t r) { (void)w; (void)r; }
static FILE *fh[8]; static int nfh;
int64_t psmac_sh_open(const char *name, uint64_t mode) { FILE *f = fopen(name, mode == 3 ? "r+b" : "rb"); if (!f || nfh == 8) return -1; fh[nfh] = f; return nfh++; }
int psmac_sh_read(int64_t fd, void *buf, uint64_t len) { return fread(buf, 1, len, fh[fd]) == len; }
int64_t psmac_sh_seek(int64_t fd, uint64_t pos) { return fseek(fh[fd], (long)pos, SEEK_SET); }
int64_t psmac_sh_flen(int64_t fd) { long p = ftell(fh[fd]); fseek(fh[fd], 0, SEEK_END); long n = ftell(fh[fd]); fseek(fh[fd], p, SEEK_SET); return n; }
int psmac_sh_available(void) { return 1; }
int psmac_card_pc_open(const char *name, uint32_t *size) { (void)name; (void)size; return -1; }
int psmac_card_pc_read(int file, uint32_t off, void *buf, uint32_t n) { (void)file; (void)off; (void)buf; (void)n; return -1; }
int psmac_card_pc_write(int file, uint32_t off, const void *buf, uint32_t n) { (void)file; (void)off; (void)buf; (void)n; return -1; }
int psmac_card_pc_writable(void) { return 0; }
int psmac_card_pc_list(void (*cb)(void *, const char *, uint32_t), void *ctx) { (void)cb; (void)ctx; return -1; }
int psmac_sh_write(int64_t fd, const void *buf, uint64_t len) { return fwrite(buf, 1, len, fh[fd]) == len; }
int psmac_display_info(int i, uint32_t *w, uint32_t *h, uint32_t *d, uint32_t *b) { (void)i; (void)w; (void)h; (void)d; (void)b; return 0; }
void psmac_guest_yield(void) { }

/* the pretend Mac memory (PCCARD_PROG): aligned longs in the host's order, as the kernel reads the Mac's with one load */
#define MRAM (1u << 20)
static uint32_t *mram;
static int prog_on;
uint32_t psmac_ram_size(void) { return mram ? MRAM : 0; }
uint8_t *psmac_mac_ram_ptr(void) { return (uint8_t *)mram; }
static void m32(uint32_t a, uint32_t v) { mram[a / 4] = v; }
static void m16(uint32_t a, uint32_t v) { uint32_t *p = &mram[(a & ~3u) / 4]; *p = a & 2 ? (*p & 0xFFFF0000u) | (v & 0xFFFFu) : (*p & 0xFFFFu) | v << 16; }
#define ZONE 0x80000u
#define HRGN (ZONE + 0x100u)
#define BLK  (ZONE + 0x400u)
static void visrgn(int32_t w, int32_t h)            /* a 32-bit zone's relocatable block holding the rectangle 0,0,h,w */
{
    m32(ZONE, ZONE + 0x10000u);                     /* bkLim */
    m32(BLK, 0x80000000u); m32(BLK + 4, 12 + 12); m32(BLK + 8, HRGN - ZONE);
    m32(HRGN, BLK + 12);
    m16(BLK + 12, 10); m16(BLK + 14, 0); m16(BLK + 16, 0); m16(BLK + 18, (uint32_t)h); m16(BLK + 20, (uint32_t)w);
}

/* the Mac's screen: W x H at DEPTH, an 8-bit one with the 6 x 6 x 6 cube and greys; with PCCARD_TWO a second one */
static uint32_t SW, SH, SD, RB;
static uint8_t *scr;
static uint32_t clut[256];
#define S1W 1024u
#define S1H 768u
static uint8_t *scr1;
int psmac_display_surface(int i, uint8_t **pix, uint32_t *w, uint32_t *h, uint32_t *rb, uint32_t *depth,
                          const volatile uint32_t **c, uint32_t *seq, int *gray)
{
    if (i == 1 && scr1) { *pix = scr1; *w = S1W; *h = S1H; *rb = S1W; *depth = 8; *c = clut; *seq = 1; *gray = 0; return 1; }
    if (i) return 0;
    *pix = scr; *w = SW; *h = SH; *rb = RB; *depth = SD; *c = clut; *seq = 1; *gray = 0;
    return 1;
}
static void desktop(void)                           /* a desktop pattern: what the card does not draw stays so */
{
    memset(scr, SD == 1 ? 0x55 : 0x80, (size_t)RB * SH);
    if (scr1) memset(scr1, 0x80, (size_t)S1W * S1H);
}
/* pixels that are not the desktop's: inside (1) or outside (0) the rectangle l,t-r,b of screen sn */
static uint32_t changed(int sn, int inside, int32_t l, int32_t t, int32_t r, int32_t b)
{
    uint32_t n = 0, w = sn ? S1W : SW, h = sn ? S1H : SH;
    if (sn && !scr1) return 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            int in = (int32_t)x >= l && (int32_t)x < r && (int32_t)y >= t && (int32_t)y < b;
            if (in != inside) continue;
            if (sn) n += scr1[y * S1W + x] != 0x80;
            else if (SD == 1) n += ((scr[y * RB + x / 8] ^ 0x55) >> (7 - (x & 7))) & 1;
            else n += scr[y * RB + x * SD / 8] != 0x80;
        }
    return n;
}
static void write_picture(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    if (SD == 1) {
        fprintf(f, "P5\n%u %u\n255\n", SW, SH);
        for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) fputc(scr[y * RB + x / 8] & (0x80 >> (x & 7)) ? 0 : 255, f);
    } else {
        fprintf(f, "P6\n%u %u\n255\n", SW, SH);
        for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) {
            uint32_t c = SD == 8 ? clut[scr[y * RB + x]] : SD == 32 ? ((uint32_t *)(scr + y * RB))[x] : 0;
            if (SD == 16) { uint16_t v = ((uint16_t *)(scr + y * RB))[x]; c = (uint32_t)((v >> 10 & 31) * 255 / 31) << 16 | (uint32_t)((v >> 5 & 31) * 255 / 31) << 8 | (v & 31) * 255 / 31; }
            fputc((int)(c >> 16 & 255), f); fputc((int)(c >> 8 & 255), f); fputc((int)(c & 255), f);
        }
    }
    fclose(f);
}

/* ---- the program's half: keys into the ring as Aurora68 PC puts them ---- */
static volatile struct a68p_block *blk;
static uint8_t *win;
static void put_event(unsigned type, unsigned code)
{
    struct a68p_in *e = (struct a68p_in *)(win + A68P_OFF_INPUT) + (blk->in_head & (A68P_IN_SLOTS - 1));
    memset(e, 0, sizeof *e); e->type = (uint16_t)type; e->code = (uint16_t)code;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    blk->in_head = blk->in_head + 1;
}
static int vk(char c, int *shift)
{
    static const char *row = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.";    /* Mac virtual key codes 0x00-0x2F */
    *shift = 0;
    if (c >= 'A' && c <= 'Z') { *shift = 1; c = (char)(c - 'A' + 'a'); }
    if (c == ' ') return 0x31;
    if (c == '\r') return 0x24;
    if (c == 27) return 0x35;
    if (c == 1) return 0x7D;                                    /* \d: Down */
    if (c == '>') { *shift = 1; return 0x2F; }                  /* Shift-period */
    if (c == ':') { *shift = 1; return 0x29; }                  /* Shift-semicolon */
    for (int i = 0; i < 0x30; i++) if (row[i] == c && c) return i;
    return -1;
}
/* the program's half (PCCARD_PROG) */
static int32_t wscr, wl, wt, wr, wb;                 /* the window: its screen, its content in that screen's pixels */
static int stalled;
static uint64_t beat_t;
static void give_place(void)
{
    blk->prog = A68P_PROG_LIVE;
    blk->screen = (uint32_t)wscr; blk->top = wt; blk->left = wl; blk->bottom = wb; blk->right = wr; blk->visible = 1;
    visrgn(wr - wl, wb - wt);
    blk->vis_rgn = HRGN; blk->vis_flags = 0; blk->clip_tag = A68P_CLIP_TAG + blk->geom_seq + 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    blk->geom_seq = blk->geom_seq + 1;
}
static void poll_for(uint32_t ms)                    /* the card's core, and the program's loop beside it */
{
    uint64_t end = pc_host_now() + (uint64_t)ms * 1000u;
    while (pc_host_now() < end) {
        psmac_pc_poll(2);
        if (prog_on && !stalled && pc_host_now() >= beat_t) { blk->alive = blk->alive + 1; beat_t = pc_host_now() + 100000; }
    }
}
static int prog_fails;
static void prog_step(const char *what)
{
    if (!strcmp(what, "@live")) {
        stalled = 0; blk->alive = blk->alive + 1; give_place();
        poll_for(300); desktop(); poll_for(1200);         /* a redraw comes at once; then the cursor's blinks and the text */
        uint32_t in = changed(wscr, 1, wl, wt, wr, wb), out0 = changed(0, 0, wscr ? 0 : wl, wscr ? 0 : wt, wscr ? 0 : wr, wscr ? 0 : wb),
                 out1 = changed(1, 0, wscr ? wl : 0, wscr ? wt : 0, wscr ? wr : 0, wscr ? wb : 0);
        int ok = in > 0 && out0 == 0 && out1 == 0 && blk->nodraw == A68P_ND_DRAWS;
        printf("pccard_test: @live: %u pixels drawn in the window, %u outside it on screen 0, %u on screen 1 (nodraw %u): %s\n", in, out0, out1, blk->nodraw, ok ? "drawn again, in the window only" : "FAIL");
        if (!ok) prog_fails++;
        return;
    }
    if (!strcmp(what, "@quit")) {                     /* as pc.c quits: gone(), then A68P_C_NOWINDOW, awaited */
        blk->prog = A68P_PROG_GONE; blk->focus = 0; blk->visible = 0; blk->clip_tag = 0; blk->vis_rgn = 0;
        blk->geom_seq = blk->geom_seq + 1;
        blk->cmd = A68P_C_NOWINDOW; blk->arg0 = blk->arg1 = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        uint32_t seq = blk->cmd_seq + 1;
        blk->cmd_seq = seq;
        uint64_t t0 = pc_host_now();
        while (blk->done_seq != seq && pc_host_now() - t0 < 3000000u) psmac_pc_poll(2);
        stalled = 1;                                  /* and its loop is gone */
        m32(BLK, 0);                                  /* its heap freed: the region's block free memory */
        m32(HRGN, 0x12345678u);
    } else if (!strcmp(what, "@stall")) stalled = 1;
    else if (!strcmp(what, "@crash")) { m32(BLK, 0); stalled = 1; }   /* the heap gone, nobody said so: the block freed */
    else if (!strcmp(what, "@free")) m32(BLK, 0);                      /* the block freed while the loop goes on: the region check alone */
    poll_for(!strcmp(what, "@quit") ? 100 : 1200);
    desktop(); poll_for(1500);
    uint32_t n0 = changed(0, 0, 0, 0, 0, 0), n1 = changed(1, 0, 0, 0, 0, 0);
    int ok = n0 == 0 && n1 == 0 && blk->nodraw != A68P_ND_DRAWS;
    printf("pccard_test: %s: %u pixels drawn on screen 0, %u on screen 1 in 1.5 s after it (nodraw %u, stalls %u): %s\n", what, n0, n1, blk->nodraw, blk->stalls,
           ok ? "nothing drawn" : "FAIL");
    if (!ok) prog_fails++;
}

static const char *pending; static uint64_t next_key_t;
static void type_step(void)
{
    if (!pending || !*pending || pc_host_now() < next_key_t) return;
    if (!strcmp(pending, "@quit") || !strcmp(pending, "@stall") || !strcmp(pending, "@crash") || !strcmp(pending, "@free") || !strcmp(pending, "@live")) {
        prog_step(pending);
        pending += strlen(pending);
        next_key_t = pc_host_now();
        return;
    }
    /* "@attach:N" / "@eject:N": the Disks menu's choice of image N (Aurora68 PC's command), instead of keys */
    if (*pending == '@') {
        int n = atoi(strchr(pending, ':') ? strchr(pending, ':') + 1 : "0");
        blk->cmd = !strncmp(pending, "@attach", 7) ? A68P_C_ATTACH : A68P_C_DETACH;
        blk->arg0 = (uint32_t)n; blk->arg1 = A68P_PLACE_AUTO;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        blk->cmd_seq = blk->cmd_seq + 1;
        printf("pccard_test: %s\n", pending);
        pending += strlen(pending);
        next_key_t = pc_host_now() + 1000000;
        return;
    }
    int shift, k = vk(*pending++, &shift);
    if (k < 0) return;
    if (blk->in_head - blk->in_tail > A68P_IN_SLOTS - 4) { pending--; return; }
    if (shift) put_event(A68P_IN_KEYDOWN, 0x38);
    put_event(A68P_IN_KEYDOWN, (unsigned)k);
    put_event(A68P_IN_KEYUP, (unsigned)k);
    if (shift) put_event(A68P_IN_KEYUP, 0x38);
    next_key_t = pc_host_now() + 150000;                             /* a key every 150 ms, as a person types */
}
static int on_screen(const char *t)
{
    const struct a68p_tinfo *ti = (const struct a68p_tinfo *)(win + A68P_OFF_TINFO);
    const uint16_t *cells = (const uint16_t *)(win + A68P_OFF_TEXT);
    size_t n = strlen(t);
    for (uint32_t r = 0; r < ti->rows; r++)
        for (uint32_t c = 0; c + n <= ti->cols; c++) {
            size_t k = 0;
            while (k < n && (char)(cells[r * ti->cols + c + k] & 0xff) == t[k]) k++;
            if (k == n) return 1;
        }
    return 0;
}
static char *unescape(const char *s)
{
    char *o = strdup(s), *p = o;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) { s++; *p++ = *s == 'r' ? '\r' : *s == 'e' ? 27 : *s == 'd' ? 1 : *s; }
        else *p++ = *s;
    }
    *p = 0;
    return o;
}

int main(int argc, char **argv)
{
    if (argc < 7) { fprintf(stderr, "usage: pccard_test ROM VGAROM DISK|- WxHxDEPTH OUT.pnm SECONDS [WAIT_TEXT KEYS]...\n"); return 2; }
    sscanf(argv[4], "%ux%ux%u", &SW, &SH, &SD);
    RB = (SW * SD / 8 + 15) & ~15u;
    scr = calloc(RB, SH);
    for (int i = 0; i < 216; i++) clut[i] = (uint32_t)(i / 36 * 51) << 16 | (uint32_t)(i / 6 % 6 * 51) << 8 | (uint32_t)(i % 6 * 51);
    for (int i = 216; i < 256; i++) { uint32_t g = (uint32_t)(i - 216) * 255 / 39; clut[i] = g << 16 | g << 8 | g; }
    if (getenv("PCCARD_TWO")) scr1 = calloc(S1W, S1H);
    desktop();
    uint32_t wx = SW >= 520 ? 16 : 15, wy = 40, ww = 484, wh = SD == 1 && SH < 366 ? 300 : 325;
    if (SD > 1) { ww = SW >= 724 + 32 ? 724 : 644; wh = 400; }
    prog_on = getenv("PCCARD_PROG") != 0;
    wscr = 0; wl = (int32_t)wx; wt = (int32_t)wy;
    if (getenv("PCCARD_WIN")) sscanf(getenv("PCCARD_WIN"), "%d,%d,%d", &wscr, &wl, &wt);
    if (wscr == 1 && scr1) { ww = 484; wh = 300; }     /* the SE's one-bit text size on the 8-bit HDMI screen: the PC's font in colour */
    wr = wl + (int32_t)ww; wb = wt + (int32_t)wh;
    if (prog_on) { mram = calloc(MRAM / 4, 4); if (!mram) return 2; }
    snprintf(opts[nopts++], 200, "pc=on");
    snprintf(opts[nopts++], 200, "pc.rom=%s", argv[1]);
    snprintf(opts[nopts++], 200, "pc.vgarom=%s", argv[2]);
    if (strcmp(argv[3], "-")) snprintf(opts[nopts++], 200, "pc.disk0=%s", argv[3]);
    if (!prog_on) snprintf(opts[nopts++], 200, "pc.testwin=0,%u,%u,%u,%u", wx, wy, wx + ww, wy + wh);
    snprintf(opts[nopts++], 200, "pc.textlog=1");
    /* PCCARD_OPTS="key=value ...": more of the card's options (pc.disk0ro=on, pc.images=...) */
    static char more[400];
    snprintf(more, sizeof more, "%s", getenv("PCCARD_OPTS") ? getenv("PCCARD_OPTS") : "");
    for (char *w = strtok(more, " "); w && nopts < 16; w = strtok(0, " ")) snprintf(opts[nopts++], 200, "%s", w);
    uint32_t size = psmac_pccard_size();
    void *mem = 0;
    if (posix_memalign(&mem, 1 << 16, size)) return 2;
    psmac_pccard_map((uintptr_t)mem);
    psmac_pccard_init();
    win = mem; blk = (volatile struct a68p_block *)win;
    blk->focus = 1;
    if (prog_on) { give_place(); beat_t = 0; }
    uint64_t t0 = pc_host_now(), end = t0 + (uint64_t)atoi(argv[6]) * 1000000u;
    int step = 7, ok = 1;
    while (pc_host_now() < end && blk->state != A68P_S_FAILED) {
        psmac_pc_poll(2);
        if (prog_on && !stalled && pc_host_now() >= beat_t) { blk->alive = blk->alive + 1; beat_t = pc_host_now() + 100000; }
        type_step();
        if (step < argc && (!pending || !*pending) && pc_host_now() >= next_key_t && on_screen(argv[step])) {   /* the keys before typed */
            printf("pccard_test: \"%s\" on the screen after %.1f s\n", argv[step], (double)(pc_host_now() - t0) / 1e6);
            if (step + 1 < argc) { pending = unescape(argv[step + 1]); next_key_t = pc_host_now() + 500000; }
            step += 2;
        }
        if (step >= argc && (!pending || !*pending)) break;     /* every step done */
    }
    poll_for(1600);                                            /* the last pictures drawn */
    /* what the PC wrote, back in the image file before the end (the card writes back after 2 s without writes) */
    for (uint64_t t0w = pc_host_now(), t = t0w + 20000000u; (blk->disk_dirty || pc_host_now() < t0w + 6000000u) && pc_host_now() < t; ) psmac_pc_poll(2);
    if (blk->disk_dirty) { ok = 0; printf("pccard_test: FAIL: %u sectors were not written back\n", blk->disk_dirty); }
    if (step < argc) { ok = 0; printf("pccard_test: FAIL: \"%s\" never appeared\n", argv[step]); }
    if (prog_fails) ok = 0;
    if (scr1 && !stalled) {                                    /* two screens: the window's own screen only, inside the window only */
        uint32_t in = changed(wscr, 1, wl, wt, wr, wb), o0 = changed(0, 0, wscr ? 0 : wl, wscr ? 0 : wt, wscr ? 0 : wr, wscr ? 0 : wb),
                 o1 = changed(1, 0, wscr ? wl : 0, wscr ? wt : 0, wscr ? wr : 0, wscr ? wb : 0);
        int good = in > 0 && o0 == 0 && o1 == 0;
        printf("pccard_test: two screens, the window on screen %d at %d,%d-%d,%d: %u pixels drawn inside, %u outside on screen 0, %u on screen 1: %s\n",
               wscr, wl, wt, wr, wb, in, o0, o1, good ? "on its screen and in its place only" : "FAIL");
        if (!good) ok = 0;
    }
    psmac_pc_report();
    write_picture(argv[5]);
    printf("pccard_test: %s: %ux%u at %u bit%s, the window at %u,%u %ux%u; %s\n", argv[5], SW, SH, SD, SD == 1 ? "" : "s", wx, wy, ww, wh, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
