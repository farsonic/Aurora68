/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/a2card_test.c - the Apple II card on the host: emu68/src/psmac/psmac_a2.c itself (its start, slices, input
 * ring, drawing and write-back) with Aurora65's host library (libaurora65.a for x86-64), drawing into a pretend Mac
 * screen, the way the kernel runs it on a Pi core (docs/20-apple-ii-card.md). Built after tools/pccard_test.c.
 *
 * Backstory (23 November 2026). QEMU's Pi is shared and slow, and has no one-bit 512 x 342 screen on the solo board;
 * so the card's own file runs here too, compiled with PSMAC_A2_HOST: the kernel's few services are a page of C below
 * (options, files through stdio for the testbed's semihosting, a screen in memory at the depth asked, the clock), the
 * place is set as Aurora68 Apple II sets it (a2.testwin), keys go into the ring as the program puts them (Mac key
 * downs: character and virtual key code), and the screen is written out as a picture (PGM for one bit, PPM otherwise).
 * With -DWITH_PC the PC card (psmac_pc.c with Aurora86's host library) is linked in too and both run through the guest
 * scheduler (psmac_guest.c), the front window's machine first: the "sched" run measures what each gets.
 *
 *   a2card_test WxHxDEPTH OUT.pnm SECONDS [WAIT_TEXT ACTION]...
 * A step "WAIT_TEXT ACTION": when WAIT_TEXT is on the Apple's text screen, do ACTION: keys to type (\r Return), or a
 * command of the program's: @attach:IMAGE:UNIT (UNIT a number, or - for the image's kind's first free unit),
 * @eject:UNIT, @reset, @cold, @off, @on, @model:N, @scale (scaled to the window), @sense (white on black), @front:0|1.
 * A WAIT_TEXT "#model=N", "#state=N" or "#unitU=K" (image K in unit U, -1 none) waits for the block to say so. A last WAIT_TEXT alone is only waited for. Options of the card in A2CARD_OPTS ("key=value ..."). The run ends when
 * every step is done, or after SECONDS; then the disks are written back (the card's 2 s) and the picture written.
 * Exit 0 when every step's text was seen and nothing is left to write back.
 *   a2card_test sched SECONDS FRONT   (-DWITH_PC: PC_OPTS for the PC; FRONT pc or a2): both guests on core 2
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "psmac_a2.h"
#include "psmac_guest.h"
#ifdef WITH_PC
#include "psmac_pc.h"
#endif

/* ---- the kernel around the card ---- */
void *tlsf = (void *)1;
void *tlsf_init_with_memory(void *m, uintptr_t n) { (void)n; return m; }
void *tlsf_malloc(void *h, uintptr_t n) { (void)h; return calloc(1, n); }
void *tlsf_malloc_aligned(void *h, uintptr_t n, uintptr_t a) { (void)h; void *p = 0; if (posix_memalign(&p, a < 16 ? 16 : a, n ? n : 1)) return 0; memset(p, 0, n); return p; }
void tlsf_free(void *h, void *p) { (void)h; free(p); }
uintptr_t tlsf_get_total_size(void *h) { (void)h; return 0; }
uintptr_t tlsf_get_free_size(void *h) { (void)h; return 0; }
static uint64_t host_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }
uint64_t a2_host_now(void) { return host_us(); }
uint64_t guest_host_now(void) { return host_us(); }
uint64_t pc_host_now(void) { return host_us(); }
int psmac_board_se, psmac_board_solo = 1;
static char opts[32][200];
static int nopts;
const char *psmac_option(const char *key)
{
    size_t n = strlen(key);
    for (int i = 0; i < nopts; i++) if (!strncmp(opts[i], key, n) && opts[i][n] == '=') return opts[i] + n + 1;
    return 0;
}
void psmac_mmu_set_a2(uintptr_t w, uintptr_t r) { (void)w; (void)r; }
void psmac_mmu_set_pc(uintptr_t w, uintptr_t r) { (void)w; (void)r; }
static FILE *fh[32]; static int nfh;
int64_t psmac_sh_open(const char *name, uint64_t mode) { FILE *f = fopen(name, mode == 3 ? "r+b" : "rb"); if (!f) return -1; if (nfh == 32) { fclose(f); return -1; } fh[nfh] = f; return nfh++; }
int psmac_sh_read(int64_t fd, void *buf, uint64_t len) { return fread(buf, 1, len, fh[fd]) == len; }
int64_t psmac_sh_seek(int64_t fd, uint64_t pos) { return fseek(fh[fd], (long)pos, SEEK_SET); }
int64_t psmac_sh_flen(int64_t fd) { long p = ftell(fh[fd]); fseek(fh[fd], 0, SEEK_END); long n = ftell(fh[fd]); fseek(fh[fd], p, SEEK_SET); return n; }
int psmac_sh_write(int64_t fd, const void *buf, uint64_t len) { int ok = fwrite(buf, 1, len, fh[fd]) == len; fflush(fh[fd]); return ok; }
int psmac_sh_available(void) { return 1; }
int psmac_card_pc_open(const char *name, uint32_t *size) { (void)name; (void)size; return -1; }
int psmac_card_pc_read(int file, uint32_t off, void *buf, uint32_t n) { (void)file; (void)off; (void)buf; (void)n; return -1; }
int psmac_card_pc_write(int file, uint32_t off, const void *buf, uint32_t n) { (void)file; (void)off; (void)buf; (void)n; return -1; }
int psmac_card_pc_writable(void) { return 0; }
int psmac_card_pc_list(void (*cb)(void *, const char *, uint32_t), void *ctx) { (void)cb; (void)ctx; return -1; }
uint32_t psmac_ram_size(void) { return 0; }                 /* no Mac RAM: no region, no pointer, no menu */
uint8_t *psmac_mac_ram_ptr(void) { return 0; }
int psmac_display_info(int i, uint32_t *w, uint32_t *h, uint32_t *d, uint32_t *b) { (void)i; (void)w; (void)h; (void)d; (void)b; return 0; }
/* the sound output's sixth source: the samples counted, and their toggles (the test ROM's beep) */
static uint64_t snd_samples, snd_loud;
int psmac_audio_guest_open(uint32_t rate) { (void)rate; return 1; }
uint32_t psmac_audio_guest_room(void) { return 4096; }
void psmac_audio_guest_push(const int16_t *s, uint32_t n) { for (uint32_t i = 0; i < n; i++) { snd_samples++; if (s[i] > 2000 || s[i] < -2000) snd_loud++; } }
extern struct psmac_guest psmac_a2_guest;
#ifdef WITH_PC
extern struct psmac_guest psmac_pc_guest;
#endif

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

/* ---- the program's half: keys into the ring and commands as Aurora68 Apple II puts them ---- */
static volatile struct a68a_block *blk;
static uint8_t *win;
static void put_event(unsigned type, unsigned code, unsigned ch, unsigned mods)
{
    struct a68a_in *e = (struct a68a_in *)(win + A68A_OFF_INPUT) + (blk->in_head & (A68A_IN_SLOTS - 1));
    memset(e, 0, sizeof *e); e->type = (uint16_t)type; e->code = (uint16_t)code; e->dx = (int16_t)ch; e->modifiers = (uint16_t)mods;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    blk->in_head = blk->in_head + 1;
}
static int vk(char c)
{
    static const char *row = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.";    /* Mac virtual key codes 0x00-0x2F */
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c == ' ') return 0x31;
    if (c == '\r') return 0x24;
    for (int i = 0; i < 0x30; i++) if (row[i] == c && c) return i;
    return 0x7F;
}
static void cmd(uint32_t c, uint32_t a0, uint32_t a1)
{
    blk->cmd = c; blk->arg0 = a0; blk->arg1 = a1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    blk->cmd_seq = blk->cmd_seq + 1;
}
static void place(int scale, uint32_t options)
{
    blk->scale = (uint32_t)scale; blk->options = options;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    blk->geom_seq = blk->geom_seq + 1;
}
static int scale_now;
static uint32_t opts_now = A68A_O_GROWBOX;
static const char *pending; static uint64_t next_key_t;
static void type_step(void)
{
    if (!pending || !*pending || host_us() < next_key_t) return;
    if (*pending == '@') {
        const char *a = pending + 1;
        if (!strncmp(a, "attach:", 7)) { int i = atoi(a + 7); const char *u = strchr(a + 7, ':'); cmd(A68A_C_ATTACH, (uint32_t)i, u && u[1] != '-' ? (uint32_t)atoi(u + 1) : A68A_UNIT_AUTO); }
        else if (!strncmp(a, "eject:", 6)) cmd(A68A_C_DETACH, 0, (uint32_t)atoi(a + 6));
        else if (!strcmp(a, "reset")) cmd(A68A_C_RESET, 0, 0);
        else if (!strcmp(a, "cold")) cmd(A68A_C_RESET, 1, 0);
        else if (!strcmp(a, "off")) cmd(A68A_C_POWER_OFF, 0, 0);
        else if (!strcmp(a, "on")) cmd(A68A_C_POWER_ON, 0, 0);
        else if (!strncmp(a, "model:", 6)) cmd(A68A_C_MODEL, (uint32_t)atoi(a + 6), 0);
        else if (!strcmp(a, "scale")) { scale_now = A68A_SCALE_FIT; place(scale_now, opts_now); }
        else if (!strcmp(a, "sense")) { opts_now ^= A68A_O_APPLESENSE; place(scale_now, opts_now); }
        else if (!strncmp(a, "front:", 6)) blk->front = (uint32_t)atoi(a + 6);
        printf("a2card_test: %s\n", pending);
        pending += strlen(pending);
        next_key_t = host_us() + 300000;
        return;
    }
    if (blk->in_head - blk->in_tail > A68A_IN_SLOTS - 4) return;
    char c = *pending++;
    put_event(A68A_IN_KEYDOWN, (unsigned)vk(c), (unsigned char)c, 0);
    put_event(A68A_IN_KEYUP, (unsigned)vk(c), 0, 0);
    next_key_t = host_us() + 100000;                                /* a key every 100 ms, as a person types */
}
static int on_screen(const char *t)
{
    if (t[0] == '#') {                                          /* "#model=N", "#state=N": the block says so */
        const char *v = strchr(t, '=');
        uint32_t n = v ? (uint32_t)atoi(v + 1) : 0;
        if (!strncmp(t, "#model", 6)) return blk->model == n;
        if (!strncmp(t, "#state", 6)) return blk->state == n;
        if (!strncmp(t, "#unit", 5)) {                          /* "#unitU=K": image K in unit U (-1: none) */
            const struct a68a_disks *dk = (const struct a68a_disks *)(win + A68A_OFF_DISKS);
            return dk->unit[atoi(t + 5) % A68A_UNITS] == (v ? atoi(v + 1) : -2);
        }
        return 0;
    }
    const uint16_t *cells = (const uint16_t *)(win + A68A_OFF_TEXT);
    size_t n = strlen(t);
    uint32_t cols = blk->cols ? blk->cols : 40;
    for (uint32_t r = 0; r < 24; r++)
        for (uint32_t c = 0; c + n <= cols; c++) {
            size_t k = 0;
            while (k < n && (char)(cells[r * 80 + c + k] & 0x7f) == t[k]) k++;
            if (k == n) return 1;
        }
    return 0;
}
static char *unescape(const char *s)
{
    char *o = strdup(s), *p = o;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) { s++; *p++ = *s == 'r' ? '\r' : *s; }
        else *p++ = *s;
    }
    *p = 0;
    return o;
}
static void more_opts(const char *env)
{
    static char buf[2][800];
    static int k;
    snprintf(buf[k], sizeof buf[k], "%s", getenv(env) ? getenv(env) : "");
    for (char *w = strtok(buf[k], " "); w && nopts < 32; w = strtok(0, " ")) snprintf(opts[nopts++], 200, "%s", w);
    k++;
}
static void poll(void) { psmac_guest_poll(2); }      /* the kernel's call (psmac_guest.c), the card a guest of core 2 */

int main(int argc, char **argv)
{
    int sched = argc >= 2 && !strcmp(argv[1], "sched");
    if (argc < 4) { fprintf(stderr, "usage: a2card_test WxHxDEPTH OUT.pnm SECONDS [WAIT_TEXT ACTION]... | sched SECONDS pc|a2\n"); return 2; }
    if (sched) { SW = 1024; SH = 768; SD = 8; } else sscanf(argv[1], "%ux%ux%u", &SW, &SH, &SD);
    RB = (SW * SD / 8 + 15) & ~15u;
    scr = calloc(RB, SH);
    for (int i = 0; i < 216; i++) clut[i] = (uint32_t)(i / 36 * 51) << 16 | (uint32_t)(i / 6 % 6 * 51) << 8 | (uint32_t)(i % 6 * 51);
    for (int i = 216; i < 256; i++) { uint32_t g = (uint32_t)(i - 216) * 255 / 39; clut[i] = g << 16 | g << 8 | g; }
    memset(scr, SD == 1 ? 0x55 : 0x80, (size_t)RB * SH);    /* a desktop pattern: what the card does not draw stays so */
    /* the window as Aurora68 Apple II opens it: at 1:1, 284 x 192 for 40 columns on one bit, 560 x 384 deeper */
    uint32_t wx = 16, wy = 40, ww = SD == 1 ? 284 : 560, wh = SD == 1 ? 192 : 384;
    const char *ws = getenv("A2CARD_WIN");                  /* "W H": another size (the scaled runs) */
    if (ws) sscanf(ws, "%u %u", &ww, &wh);
    snprintf(opts[nopts++], 200, "a2=on");
    snprintf(opts[nopts++], 200, "a2.testwin=0,%u,%u,%u,%u", wx, wy, wx + ww, wy + wh);
    snprintf(opts[nopts++], 200, "a2.textlog=1");
    more_opts("A2CARD_OPTS");
#ifdef WITH_PC
    snprintf(opts[nopts++], 200, "pc=on");
    more_opts("PC_OPTS");
    uint32_t psize = psmac_pccard_size();
    void *pmem = 0;
    if (psize && posix_memalign(&pmem, 1 << 16, psize)) return 2;
    if (psize) { psmac_pccard_map((uintptr_t)pmem); psmac_pccard_init(); }
#endif
    uint32_t size = psmac_a2card_size();
    void *mem = 0;
    if (posix_memalign(&mem, 1 << 16, size)) return 2;
    psmac_a2card_map((uintptr_t)mem);
    psmac_a2card_init();
    win = mem; blk = (volatile struct a68a_block *)win;
    blk->focus = 1; blk->options = A68A_O_GROWBOX;
    uint64_t t0 = host_us();
    int ok = 1;
    if (sched) {
#ifdef WITH_PC
        /* both guests on core 2, one of them front, for SECONDS: what each got */
        volatile struct a68p_block *pb = pmem;
        int a2front = !strcmp(argv[3], "a2");
        uint64_t end = t0 + (uint64_t)atoi(argv[2]) * 1000000u, half = t0 + 3000000u;
        uint64_t a_busy0 = 0, p_busy0 = 0, a_runs0 = 0, p_runs0 = 0, cyc0 = 0, tcyc0 = 0;
        int marked = 0;
        while (host_us() < end) {
            pb->front = (uint32_t)!a2front; blk->front = (uint32_t)a2front;
            poll();
            if (!marked && host_us() >= half) {            /* the first 3 s are the PC's start: measured from then on */
                marked = 1; a_busy0 = psmac_a2_guest.busy; p_busy0 = psmac_pc_guest.busy; a_runs0 = psmac_a2_guest.runs; p_runs0 = psmac_pc_guest.runs;
                cyc0 = (uint64_t)blk->cycles_hi << 32 | blk->cycles_lo; tcyc0 = host_us();
            }
        }
        double el = (double)(host_us() - half);
        uint64_t cyc1 = (uint64_t)blk->cycles_hi << 32 | blk->cycles_lo;        /* the block's count, once a second */
        double speed = (double)(cyc1 - cyc0) / ((double)(host_us() - tcyc0) * 1.020484) * 1000.0;
        printf("a2card_test: sched, guest.floor=%u, %s in front: the PC %.0f per mille of the core in %llu slices, the Apple %.0f per mille in %llu slices (%llu by the floor); the Apple ran at about %.0f per mille of its own speed (PC: %u per mille busy, %u idle in the last second)\n",
               psmac_guest_floor(), a2front ? "the Apple" : "the PC",
               (double)(psmac_pc_guest.busy - p_busy0) * 1000.0 / el, (unsigned long long)(psmac_pc_guest.runs - p_runs0),
               (double)(psmac_a2_guest.busy - a_busy0) * 1000.0 / el, (unsigned long long)(psmac_a2_guest.runs - a_runs0),
               (unsigned long long)psmac_a2_guest.floor_runs, speed, pb->load, pb->idle_pm);
        psmac_a2_report();
        psmac_pc_report();
        return 0;
#else
        fprintf(stderr, "a2card_test: sched needs -DWITH_PC\n");
        return 2;
#endif
    }
    uint64_t end = t0 + (uint64_t)atoi(argv[3]) * 1000000u;
    int step = 4;
    int expect_fail = getenv("A2CARD_FAIL") != 0;               /* the run expects the card to refuse (no ROM) */
    while (host_us() < end && (expect_fail || blk->state != A68A_S_FAILED)) {
        poll();
        type_step();
        if (step < argc && (!pending || !*pending) && host_us() >= next_key_t && on_screen(argv[step])) {
            printf("a2card_test: \"%s\" on the screen after %.1f s\n", argv[step], (double)(host_us() - t0) / 1e6);
            if (step + 1 < argc) { pending = unescape(argv[step + 1]); next_key_t = host_us() + 300000; }
            step += 2;
        }
        if (step >= argc && (!pending || !*pending)) break;
    }
    if (blk->state == A68A_S_FAILED) printf("a2card_test: the card says: %s\n", (const char *)blk->message);
    for (int k = 0; k < 30; k++) { uint64_t t = host_us() + 40000; while (host_us() < t) poll(); }   /* the last pictures */
    /* the Apple's writes back in the image files before the end (the card writes back 2 s after the last change) */
    for (uint64_t tw = host_us(), t = tw + 20000000u; (blk->disk_dirty || host_us() < tw + 3000000u) && host_us() < t; ) poll();
    if (blk->disk_dirty) { ok = 0; printf("a2card_test: FAIL: %u units were not written back\n", blk->disk_dirty); }
    if (step < argc) { ok = 0; printf("a2card_test: FAIL: \"%s\" never appeared\n", argv[step]); }
    psmac_a2_report();
    printf("a2card_test: the speaker gave %llu samples, %llu of them loud; the window's natural size %ux%u; model %u; the card's message: %s\n",
           (unsigned long long)snd_samples, (unsigned long long)snd_loud, blk->nat_w, blk->nat_h, blk->model, (const char *)blk->message);
    write_picture(argv[2]);
    printf("a2card_test: %s: %ux%u at %u bit%s, the window at %u,%u %ux%u; %s\n", argv[2], SW, SH, SD, SD == 1 ? "" : "s", wx, wy, ww, wh, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
