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
static char opts[8][200];
static int nopts;
const char *psmac_option(const char *key)
{
    size_t n = strlen(key);
    for (int i = 0; i < nopts; i++) if (!strncmp(opts[i], key, n) && opts[i][n] == '=') return opts[i] + n + 1;
    return 0;
}
void psmac_mmu_set_pc(uintptr_t w, uintptr_t r) { (void)w; (void)r; }
static FILE *fh[8]; static int nfh;
int64_t psmac_sh_open(const char *name, uint64_t mode) { (void)mode; FILE *f = fopen(name, "rb"); if (!f || nfh == 8) return -1; fh[nfh] = f; return nfh++; }
int psmac_sh_read(int64_t fd, void *buf, uint64_t len) { return fread(buf, 1, len, fh[fd]) == len; }
int64_t psmac_sh_seek(int64_t fd, uint64_t pos) { return fseek(fh[fd], (long)pos, SEEK_SET); }
int64_t psmac_sh_flen(int64_t fd) { long p = ftell(fh[fd]); fseek(fh[fd], 0, SEEK_END); long n = ftell(fh[fd]); fseek(fh[fd], p, SEEK_SET); return n; }
int psmac_sh_available(void) { return 1; }
int psmac_card_pc_open(const char *name, uint32_t *size) { (void)name; (void)size; return -1; }
int psmac_card_pc_read(int file, uint32_t off, void *buf, uint32_t n) { (void)file; (void)off; (void)buf; (void)n; return -1; }
uint32_t psmac_ram_size(void) { return 0; }                 /* no Mac RAM: no region, no pointer, no menu */
uint8_t *psmac_mac_ram_ptr(void) { return 0; }
int psmac_display_info(int i, uint32_t *w, uint32_t *h, uint32_t *d, uint32_t *b) { (void)i; (void)w; (void)h; (void)d; (void)b; return 0; }

/* the Mac's screen: W x H at DEPTH, an 8-bit one with the 6 x 6 x 6 cube and greys */
static uint32_t SW, SH, SD, RB;
static uint8_t *scr;
static uint32_t clut[256];
int psmac_display_surface(int i, uint8_t **pix, uint32_t *w, uint32_t *h, uint32_t *rb, uint32_t *depth,
                          const volatile uint32_t **c, uint32_t *seq, int *gray)
{
    if (i) return 0;
    *pix = scr; *w = SW; *h = SH; *rb = RB; *depth = SD; *c = clut; *seq = 1; *gray = 0;
    return 1;
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
    for (int i = 0; i < 0x30; i++) if (row[i] == c && c) return i;
    return -1;
}
static const char *pending; static uint64_t next_key_t;
static void type_step(void)
{
    if (!pending || !*pending || pc_host_now() < next_key_t) return;
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
    memset(scr, SD == 1 ? 0x55 : 0x80, (size_t)RB * SH);    /* a desktop pattern: what the card does not draw stays so */
    uint32_t wx = SW >= 520 ? 16 : 15, wy = 40, ww = 480, wh = SD == 1 && SH < 366 ? 300 : 325;
    if (SD > 1) { ww = SW >= 720 + 32 ? 720 : 640; wh = 400; }
    snprintf(opts[nopts++], 200, "pc=on");
    snprintf(opts[nopts++], 200, "pc.rom=%s", argv[1]);
    snprintf(opts[nopts++], 200, "pc.vgarom=%s", argv[2]);
    if (strcmp(argv[3], "-")) snprintf(opts[nopts++], 200, "pc.disk0=%s", argv[3]);
    snprintf(opts[nopts++], 200, "pc.testwin=0,%u,%u,%u,%u", wx, wy, wx + ww, wy + wh);
    snprintf(opts[nopts++], 200, "pc.textlog=1");
    uint32_t size = psmac_pccard_size();
    void *mem = 0;
    if (posix_memalign(&mem, 1 << 16, size)) return 2;
    psmac_pccard_map((uintptr_t)mem);
    psmac_pccard_init();
    win = mem; blk = (volatile struct a68p_block *)win;
    blk->focus = 1;
    uint64_t t0 = pc_host_now(), end = t0 + (uint64_t)atoi(argv[6]) * 1000000u;
    int step = 7, ok = 1;
    while (pc_host_now() < end && blk->state != A68P_S_FAILED) {
        psmac_pc_poll(2);
        type_step();
        if (step < argc && on_screen(argv[step])) {
            printf("pccard_test: \"%s\" on the screen after %.1f s\n", argv[step], (double)(pc_host_now() - t0) / 1e6);
            if (step + 1 < argc) { pending = unescape(argv[step + 1]); next_key_t = pc_host_now() + 500000; }
            step += 2;
        }
        if (step >= argc && (!pending || !*pending)) break;     /* every step done */
    }
    for (int k = 0; k < 40; k++) { uint64_t t = pc_host_now() + 40000; while (pc_host_now() < t) psmac_pc_poll(2); }   /* the last pictures drawn */
    if (step < argc) { ok = 0; printf("pccard_test: FAIL: \"%s\" never appeared\n", argv[step]); }
    psmac_pc_report();
    write_picture(argv[5]);
    printf("pccard_test: %s: %ux%u at %u bit%s, the window at %u,%u %ux%u; %s\n", argv[5], SW, SH, SD, SD == 1 ? "" : "s", wx, wy, ww, wh, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
