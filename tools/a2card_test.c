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
 * The program's half (23 November 2026, A2CARD_PROG=1, as tools/pccard_test.c's): the place and the window's visRgn as
 * Aurora68 Apple II gives them (a handle into a pretend Mac memory), prog = A68A_PROG_LIVE, alive moving every 100 ms;
 * actions @quit (gone, A68A_C_NOWINDOW), @stall (alive stands still), @crash (the region's block freed, alive still),
 * @free (the block freed, the loop going on), each followed by a check that nothing at all is drawn, and @live (a
 * program again), followed by a check that the window is drawn and nothing outside it.
 * Disks from the Mac (24 November 2026, A2CARD_MAC=1): a pretend Aurora68 Apple II serving the card's request ring from
 * host files as the program serves it from the Mac's (the Mac's half of psmac_a2.h, "disks from the Mac"): it answers
 * every A2CARD_MACLAT microseconds (16,000: an event loop's turn), in the order of the requests' seq, and closes a file
 * once the card has let go of it. Actions @macins:PATH:UNIT (UNIT a number or -: the card's choice; PATH ending in !ro:
 * the file locked), @maceject:SLOT, @macquit (every disk ejected, the requests answered until the card let go of all,
 * then the program stops serving, as apple.c quits), @macdie (the program gone without ejecting: its ExitToShell patch);
 * WAIT_TEXT "#msN=S": media slot N in state S (A68A_MS_*).
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
/* the pretend Mac memory (A2CARD_PROG): aligned longs in the host's order, as the kernel reads the Mac's */
#define MRAM (1u << 20)
static uint32_t *mram;
uint32_t psmac_ram_size(void) { return mram ? MRAM : 0; }   /* none: no region, no pointer, no menu */
uint8_t *psmac_mac_ram_ptr(void) { return (uint8_t *)mram; }
static void m32(uint32_t a, uint32_t v) { mram[a / 4] = v; }
static void m16(uint32_t a, uint32_t v) { uint32_t *p = &mram[(a & ~3u) / 4]; *p = a & 2 ? (*p & 0xFFFF0000u) | (v & 0xFFFFu) : (*p & 0xFFFFu) | v << 16; }
#define ZONE 0x80000u
#define HRGN (ZONE + 0x100u)
#define RBLK (ZONE + 0x400u)
static void visrgn(int32_t w, int32_t h)            /* a 32-bit zone's relocatable block holding the rectangle 0,0,h,w */
{
    m32(ZONE, ZONE + 0x10000u);
    m32(RBLK, 0x80000000u); m32(RBLK + 4, 24); m32(RBLK + 8, HRGN - ZONE);
    m32(HRGN, RBLK + 12);
    m16(RBLK + 12, 10); m16(RBLK + 14, 0); m16(RBLK + 16, 0); m16(RBLK + 18, (uint32_t)h); m16(RBLK + 20, (uint32_t)w);
}
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
/* the program's half (A2CARD_PROG) */
static int prog_on, stalled, prog_fails;
static uint32_t pwx, pwy, pww, pwh;
static uint64_t beat_t;
static void poll(void);
/* ---- the pretend Mac side of the disks from the Mac (A2CARD_MAC) ---- */
static volatile struct a68a_mac *mach;
static volatile struct a68a_macc *macc;
static FILE *mfile[A68A_MEDIA];
static int mseen[A68A_MEDIA], mac_on, mac_fails;
static uint32_t mac_lat = 16000, mid_next = 1, mac_done, mac_rd, mac_wr, mac_turns;
static uint64_t mac_t;
static void mac_start(void)
{
    mach = (volatile struct a68a_mac *)(win + A68A_OFF_MAC);
    macc = (volatile struct a68a_macc *)(win + A68A_OFF_MACC);
    memset((void *)mach, 0, sizeof *mach);
    mach->session = 0x1234;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    mach->magic = A68A_MAC_MAGIC;
    mac_on = 1;
    if (getenv("A2CARD_MACLAT")) mac_lat = (uint32_t)atoi(getenv("A2CARD_MACLAT"));
}
static void mac_serve(void)
{
    if (!mac_on || host_us() < mac_t) return;
    mac_t = host_us() + mac_lat;
    if (macc->session != mach->session) return;
    mac_turns++;
    for (;;) {                                         /* the live requests, in the order of their seq */
        int best = -1;
        for (uint32_t i = 0; i < A68A_REQS; i++) {
            uint32_t q = macc->req[i].seq;
            if (q && q != mach->ans_seq[i] && (best < 0 || q - macc->req[best].seq > 0x80000000u)) best = (int)i;
        }
        if (best < 0) break;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        volatile struct a68a_mreq *r = &macc->req[best];
        uint32_t sl = r->slot, len = r->len;
        int32_t err = 0;
        uint8_t *d = win + A68A_OFF_XFER + (uint32_t)best * A68A_XFER_MAX;
        if (sl >= A68A_MEDIA || !mfile[sl] || mach->media[sl].id != r->id || len > A68A_XFER_MAX) err = -51;      /* rfNumErr */
        else if (fseek(mfile[sl], (long)r->off, SEEK_SET)) err = -40;
        else if (r->op == A68A_R_READ) { if (fread(d, 1, len, mfile[sl]) != len) err = -39; else mac_rd += len; }
        else if (r->op == A68A_R_WRITE) { if (fwrite(d, 1, len, mfile[sl]) != len) err = -36; else { mac_wr += len; fflush(mfile[sl]); } }
        else err = -50;
        mach->ans_err[best] = err;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        mach->ans_seq[best] = r->seq;
        mac_done++;
    }
    for (uint32_t s = 0; s < A68A_MEDIA; s++) {       /* the files the card has let go of: closed */
        if (!mfile[s] || macc->ms[s].id != mach->media[s].id) continue;
        uint32_t st = macc->ms[s].state;
        if (st != A68A_MS_NONE) mseen[s] = 1;
        if (st == A68A_MS_REFUSED || (st == A68A_MS_NONE && mseen[s])) {
            printf("a2card_test: the Mac closes %s (slot %u, %s)\n", (const char *)mach->media[s].name, s, st == A68A_MS_REFUSED ? "refused" : "let go");
            fclose(mfile[s]); mfile[s] = 0; mseen[s] = 0;
        }
    }
}
static void mac_insert(const char *arg)
{
    char path[400];
    snprintf(path, sizeof path, "%s", arg);
    char *u = strrchr(path, ':');
    uint32_t unit = A68A_UNIT_AUTO;
    if (u) { if (u[1] != '-') unit = (uint32_t)atoi(u + 1); *u = 0; }
    int ro = 0;
    size_t n = strlen(path);
    if (n > 3 && !strcmp(path + n - 3, "!ro")) { ro = 1; path[n - 3] = 0; }
    uint32_t sl = 0;
    while (sl < A68A_MEDIA && mfile[sl]) sl++;
    FILE *f = sl < A68A_MEDIA ? fopen(path, ro ? "rb" : "r+b") : 0;
    if (!f) { printf("a2card_test: FAIL: the Mac cannot open %s\n", path); mac_fails++; return; }
    fseek(f, 0, SEEK_END);
    volatile struct a68a_mmedia *md = &mach->media[sl];
    md->id = mid_next++; md->size = (uint32_t)ftell(f); md->flags = ro ? A68A_MF_RO : 0;
    const char *b = strrchr(path, '/');
    snprintf((char *)md->name, sizeof md->name, "%s", b ? b + 1 : path);
    mfile[sl] = f; mseen[sl] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    printf("a2card_test: the Mac offers %s (%u bytes) in slot %u\n", (const char *)md->name, md->size, sl);
    blk->cmd = A68A_C_MACINSERT; blk->arg0 = sl; blk->arg1 = unit;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    blk->cmd_seq = blk->cmd_seq + 1;
}
static int mac_open_files(void) { int n = 0; for (uint32_t s = 0; s < A68A_MEDIA; s++) n += mfile[s] != 0; return n; }
static void give_place(void)
{
    blk->prog = A68A_PROG_LIVE;
    blk->screen = 0; blk->left = (int32_t)pwx; blk->top = (int32_t)pwy; blk->right = (int32_t)(pwx + pww); blk->bottom = (int32_t)(pwy + pwh); blk->visible = 1;
    visrgn((int32_t)pww, (int32_t)pwh);
    blk->vis_rgn = HRGN; blk->vis_flags = 0; blk->clip_tag = A68A_CLIP_TAG + blk->geom_seq + 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    blk->geom_seq = blk->geom_seq + 1;
}
static void poll_for(uint32_t ms)
{
    uint64_t end = host_us() + (uint64_t)ms * 1000u;
    while (host_us() < end) {
        poll();
        if (prog_on && !stalled && host_us() >= beat_t) { blk->alive = blk->alive + 1; beat_t = host_us() + 100000; }
    }
}
static void desktop(void) { memset(scr, SD == 1 ? 0x55 : 0x80, (size_t)RB * SH); }
static uint32_t changed(int inside)
{
    uint32_t n = 0;
    for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) {
        int in = x >= pwx && x < pwx + pww && y >= pwy && y < pwy + pwh;
        if (in != inside) continue;
        if (SD == 1) n += ((scr[y * RB + x / 8] ^ 0x55) >> (7 - (x & 7))) & 1;
        else n += scr[y * RB + x * SD / 8] != 0x80;
    }
    return n;
}
static void prog_step(const char *what)
{
    if (!strcmp(what, "live")) {
        stalled = 0; blk->alive = blk->alive + 1; give_place();
        desktop(); poll_for(1500);
        uint32_t in = changed(1), out = changed(0);
        int ok = in > 0 && out == 0 && blk->nodraw == A68A_ND_DRAWS;
        printf("a2card_test: @live: %u pixels drawn in the window, %u outside it (nodraw %u): %s\n", in, out, blk->nodraw, ok ? "drawn again, in the window only" : "FAIL");
        if (!ok) prog_fails++;
        return;
    }
    if (!strcmp(what, "quit")) {                      /* as apple.c quits: gone(), then A68A_C_NOWINDOW, awaited */
        blk->prog = A68A_PROG_GONE; blk->focus = 0; blk->front = 0; blk->visible = 0; blk->clip_tag = 0; blk->vis_rgn = 0;
        blk->geom_seq = blk->geom_seq + 1;
        cmd(A68A_C_NOWINDOW, 0, 0);
        uint32_t seq = blk->cmd_seq;
        uint64_t t0 = host_us();
        while (blk->done_seq != seq && host_us() - t0 < 3000000u) poll();
        stalled = 1; m32(RBLK, 0); m32(HRGN, 0x12345678u);
    } else if (!strcmp(what, "stall")) stalled = 1;
    else if (!strcmp(what, "crash")) { m32(RBLK, 0); stalled = 1; }
    else if (!strcmp(what, "free")) m32(RBLK, 0);
    poll_for(!strcmp(what, "quit") ? 100 : 1200);
    desktop();
    cmd(A68A_C_REDRAW, 0, 0);                         /* a picture asked for (an update event): an idle Apple draws nothing else */
    poll_for(1500);
    uint32_t n = changed(0) + changed(1);
    int ok = n == 0 && blk->nodraw != A68A_ND_DRAWS;
    printf("a2card_test: @%s: %u pixels drawn in 1.5 s after it (nodraw %u, stalls %u): %s\n", what, n, blk->nodraw, blk->stalls, ok ? "nothing drawn" : "FAIL");
    if (!ok) prog_fails++;
}
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
        else if (!strncmp(a, "macins:", 7)) mac_insert(a + 7);
        else if (!strncmp(a, "maceject:", 9)) cmd(A68A_C_MACEJECT, (uint32_t)atoi(a + 9), 0);
        else if (!strcmp(a, "macquit")) {                 /* as apple.c quits: every disk out, served until the card let go */
            cmd(A68A_C_MACEJECT, A68A_UNIT_AUTO, 0);
            uint64_t t0 = host_us();
            while (mac_open_files() && host_us() - t0 < 15000000u) { poll(); mac_serve(); }
            int n = mac_open_files();
            printf("a2card_test: @macquit: every disk back on the Mac in %.1f s, %u requests answered (%u KB read, %u KB written): %s\n",
                   (double)(host_us() - t0) / 1e6, mac_done, mac_rd >> 10, mac_wr >> 10, n ? "FAIL" : "closed");
            if (n) mac_fails++;
            mach->magic = 0; mac_on = 0;
        } else if (!strcmp(a, "macdie")) { mac_on = 0; blk->prog = A68A_PROG_GONE; }
        else if (!strcmp(a, "quit") || !strcmp(a, "stall") || !strcmp(a, "crash") || !strcmp(a, "free") || !strcmp(a, "live")) {
            prog_step(a); pending += strlen(pending); next_key_t = host_us(); return;
        }
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
        if (!strncmp(t, "#ms", 3)) return macc && macc->ms[atoi(t + 3) % A68A_MEDIA].state == n;
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
static void poll(void) { psmac_guest_poll(2); mac_serve(); }   /* the kernel's call (psmac_guest.c), the card a guest of core 2; the pretend Mac's loop */

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
    prog_on = getenv("A2CARD_PROG") != 0;
    pwx = wx; pwy = wy; pww = ww; pwh = wh;
    if (prog_on) mram = calloc(MRAM / 4, 4);
    else snprintf(opts[nopts++], 200, "a2.testwin=0,%u,%u,%u,%u", wx, wy, wx + ww, wy + wh);
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
    if (prog_on) give_place();
    if (getenv("A2CARD_MAC")) mac_start();
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
        mac_serve();
        if (prog_on && !stalled && host_us() >= beat_t) { blk->alive = blk->alive + 1; beat_t = host_us() + 100000; }
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
    for (uint64_t tw = host_us(), t = tw + 20000000u; (blk->disk_dirty || host_us() < tw + 3000000u) && host_us() < t; ) { poll(); mac_serve(); }
    if (blk->disk_dirty) { ok = 0; printf("a2card_test: FAIL: %u units were not written back\n", blk->disk_dirty); }
    if (step < argc) { ok = 0; printf("a2card_test: FAIL: \"%s\" never appeared\n", argv[step]); }
    if (prog_fails || mac_fails) ok = 0;
    if (mach) printf("a2card_test: the pretend Mac answered %u requests in %u turns of its loop: %u KB read, %u KB written; %d files still open\n",
                     mac_done, mac_turns, mac_rd >> 10, mac_wr >> 10, mac_open_files());
    psmac_a2_report();
    printf("a2card_test: the speaker gave %llu samples, %llu of them loud; the window's natural size %ux%u; model %u; the card's message: %s\n",
           (unsigned long long)snd_samples, (unsigned long long)snd_loud, blk->nat_w, blk->nat_h, blk->model, (const char *)blk->message);
    write_picture(argv[2]);
    printf("a2card_test: %s: %ux%u at %u bit%s, the window at %u,%u %ux%u; %s\n", argv[2], SW, SH, SD, SD == 1 ? "" : "s", wx, wy, ww, wh, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
