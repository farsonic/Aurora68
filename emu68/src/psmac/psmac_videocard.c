/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_videocard.c — the video card as the Macintosh sees it: a NuBus card in slot $C around the
 * board-neutral core of psmac_video.c (docs/16-video-card.md, GitHub issue #23).
 *
 * Backstory (22 November 2026, docs/08 item 98). The core decodes an MPEG program stream from a byte ring
 * and draws it into a frame buffer; it knows nothing of the Mac. This file is everything Macintosh about
 * the card, kept thin so that Alice68 can put the same core behind a Zorro board or a library:
 *  - Its memory: 1 MiB of Pi RAM for the window (the block and the ring) and 16 KiB for its declaration
 *    ROM, carved by psmac_setup_memory beside the big frame buffer (psmac_videocard_size, _map).
 *  - Its place: slot $C (psmb.h, PSMB_VID_*). The window at the start of the slot's space ($FC000000) and
 *    again at $FCC00000, where a 24-bit Mac reaches the slot ($C00000); the ROM at the slot's top. The MMU
 *    shim maps all three like slot 9's window (psmac_mmu.c), so the 68k reads and writes them as memory,
 *    through the JIT, with no trap. Slot $B was the first plan (docs/16 until 22 November); the IIci's
 *    built-in video lives in slot $B's space, so the ROM's tables do not map it one to one.
 *  - Its ROM: board "Aurora68 Video Decoder" (renamed from "Aurora68 Video" the same day) and one sResource of
 *    the project's vendor category, type 3, carrying the card's driver ".A68Video" (mac/a68video.s), which the
 *    Slot Manager opens at start-up (psmac_video_declrom.h, made by tools/mac/mkvideodeclrom.py; docs/08 item
 *    110). Programs and the QuickTime components use the driver's calls; the window stays directly usable.
 *  - Its frame decoder: QuickTime's pictures one at a time (psmac_vframes.c), in the same window.
 *  - Its screens: the Mac's own frame buffers as psmac_display.c keeps them. Screen 0 is slot 9's (the
 *    SE's own 512 x 342 on the SE board; the HDMI screen on the solo board), screen 1 the SE board's HDMI
 *    screen in slot $A when there is one.
 *  - Its sound: psmac_audio.c's mixer, source 5.
 *  - A test producer for the testbed (mac.videotest=, below), which fills the ring from a host file as the
 *    Mac's program will from the network, so the decoder is tested before that program exists.
 * mac.video=off leaves slot $C empty, as before this card.
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"
#include "psmb.h"
#include "psmac_video.h"
#include "psmac_video_declrom.h"

#define KERNEL_ALIAS 0xffffff9000000000ull
extern const char *psmac_option(const char *key);
extern void psmac_mmu_set_video(uintptr_t win, uintptr_t rom);
extern int psmac_display_count(void);
extern int psmac_display_surface(int i, uint8_t **pix, uint32_t *w, uint32_t *h, uint32_t *rb, uint32_t *depth,
                                 const volatile uint32_t **clut, uint32_t *clut_seq, int *gray);
extern int64_t psmac_sh_open(const char *name, uint64_t mode);
extern int psmac_sh_read(int64_t fd, void *buf, uint64_t len);
extern int64_t psmac_sh_flen(int64_t fd);
extern int psmac_sh_available(void);

static inline uint64_t now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }

static uintptr_t card_phys;

static int video_wanted(void)
{
    const char *v = psmac_option("mac.video");
    return !(v && v[0] == 'o' && v[1] == 'f');
}
uint32_t psmac_videocard_size(void) { return video_wanted() ? A68V_WIN_SIZE + PSMB_VID_ROM_SIZE : 0; }
void psmac_videocard_map(uintptr_t phys)
{
    card_phys = phys;
    psmac_mmu_set_video(phys, phys + A68V_WIN_SIZE);
}

/* ---- mac.videotest=PATH,LEFT,TOP,WIDTH,HEIGHT,SCREEN[,DELAY[,PAUSE_AT[,SEEK_AT]]] (testbed only) ----------
   An MPEG program stream from the host (semihosting) fed into the ring as the Mac's program would feed it:
   32 KiB at a time while there is room, eof at the end. It starts DELAY seconds after the kernel (0: as soon
   as the screen is on), drawing at LEFT,TOP, WIDTH x HEIGHT on SCREEN (0 or 1). PAUSE_AT: a pause of two
   seconds once the picture is that many seconds in; SEEK_AT: at that many seconds a seek back to the start
   (STOP, the file again from its first byte, START), the way the program will seek.
   mac.videorate=KBPS[,ON_MS,OFF_MS] (testbed only, 23 November 2026, docs/08 item 125) paces the feed like a network:
   KBPS kilobits a second at most, and with ON_MS,OFF_MS only during ON_MS of every ON_MS + OFF_MS (a WiFi that
   stops for a while), what the pause could have carried lost, as a slow network loses it. */
static struct {
    int on, started, paused, seeked;
    char path[128];                          /* psmac_option gives at most 127 characters */
    int32_t x, y, w, h;
    uint32_t screen, delay, pause_at, seek_at;
    int64_t fd, len, off;
    uint64_t t0, pause_t;
    uint32_t kbps, on_ms, off_ms;            /* mac.videorate= */
    uint64_t rate_t, rate_t0, credit;
} tst;
static uint32_t num(const char **p)
{
    uint32_t n = 0;
    while (**p >= '0' && **p <= '9') n = n * 10 + (uint32_t)(*(*p)++ - '0');
    if (**p == ',') (*p)++;
    return n;
}
static void test_options(void)
{
    const char *v = psmac_option("mac.videotest");
    if (!v || !v[0]) return;
    int i = 0;
    while (v[i] && v[i] != ',' && i < (int)sizeof tst.path - 1) { tst.path[i] = v[i]; i++; }
    tst.path[i] = 0;
    const char *p = v + i + (v[i] == ',');
    tst.x = (int32_t)num(&p); tst.y = (int32_t)num(&p); tst.w = (int32_t)num(&p); tst.h = (int32_t)num(&p);
    tst.screen = num(&p); tst.delay = num(&p); tst.pause_at = num(&p); tst.seek_at = num(&p);
    if (!tst.w || !tst.h) { tst.w = 320; tst.h = 240; }
    tst.on = 1; tst.fd = -1; tst.t0 = now();
    if ((v = psmac_option("mac.videorate")) && v[0] >= '0' && v[0] <= '9') {
        p = v; tst.kbps = num(&p); tst.on_ms = num(&p); tst.off_ms = num(&p);
        kprintf("[VIDEO] mac.videotest: fed at %u kbit/s at most", tst.kbps);
        if (tst.on_ms && tst.off_ms) kprintf(", %u ms on and %u ms off", tst.on_ms, tst.off_ms);
        kprintf("\n");
    }
    kprintf("[VIDEO] mac.videotest: %s at %d,%d %dx%d on screen %u, %u s after start%s\n", tst.path, tst.x, tst.y, tst.w, tst.h,
            tst.screen, tst.delay, psmac_sh_available() ? "" : " (no semihosting: nothing will play)");
}
static void cmd(volatile struct a68v_block *b, uint32_t c)
{
    b->cmd = c;
    __asm__ volatile("dmb ish" ::: "memory");
    b->cmd_seq = b->cmd_seq + 1;
}
static int test_open(void)
{
    tst.fd = psmac_sh_open(tst.path, 1);
    if (tst.fd < 0) { kprintf("[VIDEO] mac.videotest: cannot open %s\n", tst.path); tst.on = 0; return 0; }
    tst.len = psmac_sh_flen(tst.fd); tst.off = 0;
    return 1;
}
/* mac.videorate='s pace: bytes the "network" may carry now (no limit without it) */
static uint32_t test_credit(void)
{
    if (!tst.kbps) return ~0u;
    uint64_t t = now();
    if (!tst.rate_t) { tst.rate_t = tst.rate_t0 = t; tst.credit = 0; }
    uint64_t dt = t - tst.rate_t;
    tst.rate_t = t;
    uint32_t cyc = tst.on_ms + tst.off_ms;
    int on = !tst.on_ms || !tst.off_ms || (uint32_t)((t - tst.rate_t0) * 1000u / hz() % cyc) < tst.on_ms;
    if (on) tst.credit += (uint64_t)tst.kbps * 125u * dt / hz();
    if (tst.credit > 65536u) tst.credit = 65536u;      /* what a TCP window could have in flight */
    return (uint32_t)tst.credit;
}
static void test_fill(volatile struct a68v_block *b, uint8_t *ring)
{
    uint32_t credit = test_credit();
    for (int k = 0; k < 4 && tst.off < tst.len; k++) {
        uint32_t h = b->head, room = A68V_RING_SIZE - (h - b->tail), o = h & (A68V_RING_SIZE - 1);
        uint32_t n = 32768u;
        if (n > credit) n = credit;
        if (n > room) n = room;
        if (n > A68V_RING_SIZE - o) n = A68V_RING_SIZE - o;
        if ((int64_t)n > tst.len - tst.off) n = (uint32_t)(tst.len - tst.off);
        if (!n) return;
        if (!psmac_sh_read(tst.fd, ring + o, n)) { kprintf("[VIDEO] mac.videotest: read failed at %lld\n", (long long)tst.off); tst.off = tst.len; break; }
        tst.off += n;
        if (tst.kbps) { credit -= n; tst.credit -= n; }
        __asm__ volatile("dmb ish" ::: "memory");
        b->head = h + n;
    }
    if (tst.off >= tst.len && !b->eof) b->eof = 1;
}
void psmac_video_test_poll(volatile struct a68v_block *b, uint8_t *ring)
{
    if (!tst.on) return;
    if (!tst.started) {
        uint8_t *pix; uint32_t w, h, rb, d, seq; const volatile uint32_t *clut; int gray;
        if (now() - tst.t0 < (uint64_t)tst.delay * hz()) return;
        if (!psmac_display_surface((int)tst.screen, &pix, &w, &h, &rb, &d, &clut, &seq, &gray)) return;
        if (!test_open()) return;
        b->eof = 0;
        test_fill(b, ring);
        b->screen = tst.screen; b->left = tst.x; b->top = tst.y; b->right = tst.x + tst.w; b->bottom = tst.y + tst.h; b->visible = 1;
        __asm__ volatile("dmb ish" ::: "memory");
        b->geom_seq = b->geom_seq + 1;
        cmd(b, A68V_C_START);
        tst.started = 1;
        kprintf("[VIDEO] mac.videotest: %lld bytes, started on a %ux%u screen at %u bits\n", (long long)tst.len, w, h, d);
        return;
    }
    if (b->done_seq != b->cmd_seq) return;    /* the last command not taken yet */
    uint32_t pos = b->pos_ms / 1000;
    if (tst.pause_at && !tst.paused && b->state == A68V_S_PLAYING && pos >= tst.pause_at) {
        cmd(b, A68V_C_PAUSE); tst.paused = 1; tst.pause_t = now();
        kprintf("[VIDEO] mac.videotest: pause at %u ms\n", b->pos_ms);
        return;
    }
    if (tst.paused == 1 && now() - tst.pause_t >= 2 * hz()) {
        cmd(b, A68V_C_RESUME); tst.paused = 2;
        kprintf("[VIDEO] mac.videotest: resume at %u ms\n", b->pos_ms);
        return;
    }
    if (tst.seek_at && !tst.seeked && b->state == A68V_S_PLAYING && pos >= tst.seek_at) {
        tst.seeked = 1;
        kprintf("[VIDEO] mac.videotest: seek back to the start at %u ms (stop, the file again, start)\n", b->pos_ms);
        cmd(b, A68V_C_STOP);
        tst.started = 2;
        return;
    }
    if (tst.started == 2) {                   /* the STOP taken: the stream again from its first byte */
        tst.off = 0; b->eof = 0;
        tst.fd = psmac_sh_open(tst.path, 1);
        if (tst.fd < 0) { tst.on = 0; return; }
        test_fill(b, ring);
        cmd(b, A68V_C_START);
        tst.started = 1;
        return;
    }
    test_fill(b, ring);
}

/* ---- mac.vbuffer=MB, mac.vprefill=START[,REBUFFER] (23 November 2026, docs/08 item 125) ---------------------
   The Pi's buffer behind the ring (8 MB by default; 0: the ring alone, as before) and the seconds a stream holds
   before it starts (3) and before it goes on after running dry (2; two-thirds of START when only START is given;
   0: at once, as before). */
static void buffer_options(void)
{
    uint32_t mb = 8, start = 3, re = 2;
    const char *v = psmac_option("mac.vbuffer"), *p;
    if (v && v[0] >= '0' && v[0] <= '9') { p = v; mb = num(&p); if (mb > 32) mb = 32; }
    if ((v = psmac_option("mac.vprefill")) && v[0] >= '0' && v[0] <= '9') {
        p = v; start = num(&p);
        re = *p >= '0' && *p <= '9' ? num(&p) : start * 2 / 3;
        if (start > 30) start = 30;
        if (re > 30) re = 30;
    }
    psmac_video_config(mb << 20, start * 1000u, re * 1000u);
    kprintf("[VIDEO] buffering: %u MB behind the ring, %u s before a stream starts, %u s after it runs dry\n", mb, start, re);
}

/* ---- the screens and the start --------------------------------------------------------------------- */
int psmac_video_surface(uint32_t screen, struct pv_surface *s)
{
    uint32_t seq; int gray;
    if (!psmac_display_surface((int)screen, &s->base, &s->width, &s->height, &s->rowbytes, &s->depth, &s->clut, &seq, &gray)) return 0;
    s->clut_seq = seq; s->gray = gray;
    return 1;
}
/* a menu down: TheMenu ($A26, the ID of the menu whose title is highlighted, 0 when none) is not 0 while the Menu
   Manager tracks a menu and until the program unhighlights its title (HiliteMenu(0)) after the command; the same for
   every program (23 November 2026, docs/08 item 124). A long at $A24 (MenuFlash, TheMenu), read as the crash record
   reads the Mac's memory */
uint32_t psmac_mac_rd32(uint32_t logical, int *ok);
uint32_t psmac_video_rd32(uint32_t addr, int *ok) { return psmac_mac_rd32(addr, ok); }
int psmac_video_menu_down(void)
{
    int ok;
    uint32_t v = psmac_mac_rd32(0xA24, &ok);
    return ok && (v & 0xFFFFu) != 0;
}
void psmac_videocard_init(void)
{
    uint8_t *rom = (uint8_t *)(KERNEL_ALIAS + card_phys + A68V_WIN_SIZE);
    for (uint32_t i = 0; i < PSMB_VID_ROM_SIZE; i++) rom[i] = 0;
    memcpy(rom + PSMB_VID_ROM_SIZE - sizeof psmac_video_declrom, psmac_video_declrom, sizeof psmac_video_declrom);   /* right-aligned: the format block at the slot's top */
    arm_flush_cache((uintptr_t)rom, PSMB_VID_ROM_SIZE);
    uint8_t *win = (uint8_t *)(KERNEL_ALIAS + card_phys);
    uint32_t screens = (uint32_t)psmac_display_count();
    buffer_options();
    psmac_video_init(win, screens);
    arm_flush_cache((uintptr_t)win, 4096);
    test_options();
    kprintf("[VIDEO] the video decoder card in slot $%X: window $%08X (24-bit $%06X), ROM %u bytes at the slot's top; %u screen%s to draw on\n",
            PSMB_VID_SLOT, PSMB_VID_WIN_PHYS, PSMB_VID_SLOT << 20, (uint32_t)sizeof psmac_video_declrom, screens, screens == 1 ? "" : "s");
}
