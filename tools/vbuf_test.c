/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/vbuf_test.c - the video card's stream channel on the host: psmac_video.c itself (its ring, the Pi's buffer
 * behind it, buffering by time, rebuffering, pl_mpeg, the drawing) against a simulated network and mixer
 * (tools/vbuf_test.sh, docs/08 item 125, docs/16-video-card.md "Buffering").
 *
 *     vbuf_test FILM.mpg FRAMES SCENARIO
 *
 * Backstory (23 November 2026). The owner's music videos stuttered on the SE whenever its WiFi slowed for a
 * moment. The card gained a buffer in the Pi's memory behind its 512 KiB ring, a start that waits for seconds of
 * the stream rather than 64 KiB, and a rebuffer that stops the clock once and waits for two seconds instead of
 * stuttering at every few packets. This program runs the card's own file (compiled with PSMAC_VIDEO_HOST: the
 * clock is this program's, in microseconds, advanced a millisecond a step) with the Mac's side played as the
 * classic Play plays it: the film poured into the ring as a network delivers it, at most what the ring has room
 * for (TCP holds the server back), eof at the end. The mixer is simulated too: it plays 44.1 kHz from what the card
 * gave it, and a moment when it has nothing while not paused is a gap the listener hears.
 *
 * Scenarios: steady (twice the film's rate), bursty (1.5 s at six times its rate, then 4.5 s of nothing: more than the ring holds),
 * outage (1.25 times its rate with six seconds of nothing from 8 s, through a 1 MiB buffer so that it wraps),
 * outage-timer (the same without sound: the Pi's timer is the clock, as in QEMU), pause (paused 3 s at 4 s while
 * the network goes on), stop (a STOP at 5 s, the ring and the buffer emptied, the film again from its start), and
 * bursty-old / outage-old with mac.vbuffer=0 mac.vprefill=0 (the card as it was). Each prints one line of numbers;
 * the exit status says whether the scenario's checks held (the -old ones are only measured).
 *
 * Drawn over another window (25 November 2026, docs/16 "Drawn over another window", docs/08 item 146): the cover
 * scenarios give the card a pretend Mac memory holding the window's visRgn (the picture 320 x 200 at the screen's top,
 * the region 30 lines taller, the strip below the picture as Play's and Video's windows have one) and, in the middle of
 * a picture, do what the Mac did on the owner's SE when a Finder window was dropped over the Apple II card's window:
 * paint that window over the picture's right half below its first 40 lines and give the card's window its smaller,
 * L-shaped region. cover1: before the second band's look at the region (the card must see it and give the picture up);
 * cover2: just after the look (the band lands on the Finder's window, and the card must ask for it to be painted
 * again: the pretend program does, as the Finder would after PaintBehind); cover-repair: video.guard=repair, the
 * repairs alone; cover-paused: a paused film's last picture drawn again for a geometry change, given up, and drawn
 * again with the new region at the next turn; cover-old: video.guard=off, the region read once a picture: the leftover.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "psmac_video.h"

void *tlsf;                                   /* the kernel's heap; the C library's here (tools/mpg1_host/tlsf.h) */
static uint64_t T;                            /* the simulated time, microseconds */
uint64_t pv_host_now(void) { return T; }

/* ---- what the kernel around the card would provide ---------------------------------------------------- */
void psmac_vframes_init(uint8_t *win) { (void)win; }
int  psmac_vframes_poll(void) { return 0; }
void psmac_vframes_reset(void) {}
void psmac_vframes_report(void) {}
void psmac_video_test_poll(volatile struct a68v_block *b, uint8_t *r) { (void)b; (void)r; }

#define SW 320
#define SH 240
static uint32_t screen[SW * SH];
int psmac_video_surface(uint32_t s, struct pv_surface *o)
{
    if (s) return 0;
    o->base = (uint8_t *)screen; o->width = SW; o->height = SH; o->rowbytes = SW * 4; o->depth = 32;
    o->clut = 0; o->clut_seq = 0; o->gray = 0;
    return 1;
}
/* the Mac's memory (the window's region, the menu bar's data; docs/08 item 124), read by psmac_wclip.c: none but in the
   cover scenarios (aligned longs in the host's order, as the kernel reads the Mac's; tools/a2card_test.c's), so no
   pointer and no menu down */
#define MRAM (1u << 20)
static uint32_t *mram;
uint32_t psmac_ram_size(void) { return mram ? MRAM : 0; }
uint8_t *psmac_mac_ram_ptr(void) { return (uint8_t *)mram; }
int psmac_display_info(int i, uint32_t *w, uint32_t *h, uint32_t *depth, uint32_t *base) { (void)i; (void)w; (void)h; (void)depth; (void)base; return 0; }
static void m32(uint32_t a, uint32_t v) { mram[a / 4] = v; }
static void m16(uint32_t a, uint32_t v) { uint32_t *p = &mram[(a & ~3u) / 4]; *p = a & 2 ? (*p & 0xFFFF0000u) | (v & 0xFFFFu) : (*p & 0xFFFFu) | v << 16; }
#define ZONE 0x80000u
#define HRGN (ZONE + 0x100u)
#define RBLK (ZONE + 0x400u)
static void visrgn(int32_t w, int32_t h)            /* a 32-bit zone's relocatable block holding the rectangle 0,0,h,w */
{
    m32(ZONE, ZONE + 0x10000u);
    m32(RBLK, 0x80000000u); m32(RBLK + 4, 12 + 64); m32(RBLK + 8, HRGN - ZONE);
    m32(HRGN, RBLK + 12);
    m16(RBLK + 12, 10); m16(RBLK + 14, 0); m16(RBLK + 16, 0); m16(RBLK + 18, (uint32_t)h); m16(RBLK + 20, (uint32_t)w);
}
/* the region the window has once another window covers its content from x = cl, y = ct to its bottom right (window
   coordinates): an L of inversion lines, rewritten in place in the same block, as the Window Manager does */
static void visrgn_l(int32_t w, int32_t h, int32_t cl, int32_t ct)
{
    static const int n = 10 + 3 * 8 + 2;
    uint32_t a = RBLK + 12;
    uint32_t v[] = { (uint32_t)n, 0, 0, (uint32_t)h, (uint32_t)w,
                     0, 0, (uint32_t)w, 0x7FFF, (uint32_t)ct, (uint32_t)cl, (uint32_t)w, 0x7FFF, (uint32_t)h, 0, (uint32_t)cl, 0x7FFF, 0x7FFF };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++, a += 2) m16(a, v[i]);
}

/* ---- the cover scenarios ------------------------------------------------------------------------------- */
#define PW 320                                /* the picture's place: the screen's top, 320 x 200; the window's region 30 taller */
#define PH 200
#define STRIP 30
#define CX0 160                               /* the Finder's window over the picture from here to its bottom right */
#define CY0 40
#define FINDER 0x00123456u
static int cover_mode, cover_armed, cover_calls, cover_done, cover_seen;
static uint32_t cov_px_at_end, rep_done_n, rep_rects, rep_outside;
static int covered(uint32_t x, uint32_t y) { return x >= CX0 && x < PW && y >= CY0 && y < PH; }
static uint32_t not_finder(void)
{
    uint32_t n = 0;
    for (uint32_t y = CY0; y < PH; y++) for (uint32_t x = CX0; x < PW; x++) n += screen[y * SW + x] != FINDER;
    return n;
}
/* psmac_video.c's BAND_HOOK: 0 before a band's look at the region, 1 after it, 2 at the picture's end, 3 after the one
   reading of video.guard=off */
void pv_host_band(int after_look)
{
    if (!cover_armed || cover_done) {
        if (after_look == 2 && cover_done && !cover_seen) { cover_seen = 1; cov_px_at_end = not_finder(); }   /* that picture's end */
        return;
    }
    if (after_look == 3 && cover_mode == 3) cover_calls = 1;    /* the region read once, the picture drawn */
    else if (after_look != (cover_mode == 2 ? 1 : 0)) return;
    if (++cover_calls < 2) return;                              /* the second band */
    cover_done = 1;
    for (uint32_t y = CY0; y < PH; y++) for (uint32_t x = CX0; x < PW; x++) screen[y * SW + x] = FINDER;
    visrgn_l(PW, PH + STRIP, CX0, CY0);
}
/* the program's turn: a repair request painted (the Finder's window where it was asked, as PaintBehind would) */
static uint8_t *win;
static volatile struct a68v_block *b;
static void prog_repairs(void)
{
    volatile struct a68_repair *rp = (volatile struct a68_repair *)(win + A68V_OFF_REPAIR);
    uint32_t s1 = rp->seq;
    if ((s1 & 1) || s1 == b->repair_done) return;
    struct a68_repair_rect r[A68_REPAIR_MAX];
    uint32_t n = rp->n > A68_REPAIR_MAX ? A68_REPAIR_MAX : rp->n;
    for (uint32_t i = 0; i < n; i++) r[i] = rp->r[i];
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (rp->seq != s1) return;
    for (uint32_t i = 0; i < n; i++) {
        rep_rects++;
        for (int32_t y = r[i].top; y < r[i].bottom; y++) for (int32_t x = r[i].left; x < r[i].right; x++) {
            if (x < 0 || y < 0 || x >= SW || y >= SH) continue;
            if (!covered((uint32_t)x, (uint32_t)y)) rep_outside++;  /* more than was drawn outside the region */
            else screen[y * SW + x] = FINDER;
        }
    }
    b->repair_done = s1;
    rep_done_n++;
}

/* the mixer: psmac_audio.c's video source, 32,768 frames of room, played at its rate unless paused */
static struct { int want, open, paused, in_gap; uint32_t rate; uint64_t queued, played, gap, gaps; double acc; } mx;
int psmac_audio_video_open(uint32_t rate)
{
    if (!mx.want) return 0;
    mx.open = 1; mx.rate = rate; mx.queued = mx.played = 0; mx.paused = 0; mx.acc = 0;
    return 1;
}
uint32_t psmac_audio_video_room(void) { return (uint32_t)(32768u - mx.queued); }
uint32_t psmac_audio_video_queued(void) { return mx.open ? (uint32_t)mx.queued : 0; }
void psmac_audio_video_push(const int16_t *lr, uint32_t frames) { (void)lr; mx.queued += frames; }
uint64_t psmac_audio_video_played(void) { return mx.played; }
void psmac_audio_video_pause(int on) { mx.paused = on; }
void psmac_audio_video_volume(uint32_t lr) { (void)lr; }
void psmac_audio_video_close(void) { mx.open = 0; mx.queued = 0; }
static void mixer_step(double dt, int heard)   /* heard: a gap now would be one (not after the film's end) */
{
    if (!mx.open || mx.paused) { mx.in_gap = 0; return; }
    mx.acc += mx.rate * dt;
    uint64_t want = (uint64_t)mx.acc, take = want < mx.queued ? want : mx.queued;
    mx.acc -= (double)want;
    mx.queued -= take; mx.played += take;
    if (take < want && mx.played && heard) { mx.gap += want - take; if (!mx.in_gap) mx.gaps++; mx.in_gap = 1; }
    else if (want) mx.in_gap = 0;
}

/* ---- the Mac's side ------------------------------------------------------------------------------------ */
static uint8_t *ring, *film;
static long film_len, fed;
static double credit;
static void cmd(uint32_t c)
{
    b->cmd = c;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    b->cmd_seq = b->cmd_seq + 1;
}
/* the network delivers rate bytes a second into the ring, at most what it has room for */
static void deliver(double rate, double dt)
{
    credit += rate * dt;
    if (credit > 65536.0) credit = 65536.0;  /* a TCP window in flight; what a pause could have carried is lost */
    while (fed < film_len && credit >= 1.0) {
        uint32_t h = b->head, room = A68V_RING_SIZE - (h - b->tail), o = h & (A68V_RING_SIZE - 1);
        uint32_t n = (uint32_t)credit;
        if (n > room) n = room;
        if (n > A68V_RING_SIZE - o) n = A68V_RING_SIZE - o;
        if ((long)n > film_len - fed) n = (uint32_t)(film_len - fed);
        if (!n) break;
        memcpy(ring + o, film + fed, n);
        fed += n; credit -= n;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        b->head = h + n;
    }
    if (fed >= film_len) b->eof = 1;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: vbuf_test FILM.mpg FRAMES SCENARIO\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); film_len = ftell(f); fseek(f, 0, SEEK_SET);
    film = malloc((size_t)film_len);
    if (fread(film, 1, (size_t)film_len, f) != (size_t)film_len) { perror(argv[1]); return 2; }
    fclose(f);
    uint32_t frames = (uint32_t)atoi(argv[2]);
    const char *sc = argv[3];
    double secs = frames / 25.0, rate = film_len / secs;     /* the film's own bytes a second */
    int old = strstr(sc, "-old") != 0 && strncmp(sc, "cover", 5) != 0, sound = strcmp(sc, "outage-timer") != 0;
    int steady = !strcmp(sc, "steady"), bursty = !strncmp(sc, "bursty", 6), outage = !strncmp(sc, "outage", 6);
    int pause = !strcmp(sc, "pause"), stop = !strcmp(sc, "stop");
    int cover = !strncmp(sc, "cover", 5), cpaused = !strcmp(sc, "cover-paused");
    cover_mode = !strcmp(sc, "cover1") || cpaused || !strcmp(sc, "cover-repair") ? 1 : !strcmp(sc, "cover2") ? 2 : !strcmp(sc, "cover-old") ? 3 : 0;
    if (cover && !cover_mode) { fprintf(stderr, "vbuf_test: no scenario %s\n", sc); return 2; }
    if (!steady && !bursty && !outage && !pause && !stop && !cover) { fprintf(stderr, "vbuf_test: no scenario %s\n", sc); return 2; }
    if (cover) {
        mram = calloc(1, MRAM);
        visrgn(PW, PH + STRIP);
        psmac_video_guard_config(cover_mode == 3 ? 1 : !strcmp(sc, "cover-repair") ? 2 : 0, 0);
    }
    uint32_t start_ms = old ? 0 : 3000, rebuf_ms = old ? 0 : 2000;
    psmac_video_config(old ? 0 : outage ? 1u << 20 : 8u << 20, start_ms, rebuf_ms);
    mx.want = sound;

    win = calloc(1, A68V_WIN_SIZE);
    ring = win + A68V_OFF_RING;
    b = (volatile struct a68v_block *)win;
    psmac_video_init(win, 1);
    b->screen = 0; b->left = 0; b->top = 0; b->right = SW; b->bottom = SH; b->visible = 1;
    if (cover) {                              /* the picture 320 x 200, the window's region handed over (Play 1.8's way) */
        b->bottom = PH;
        b->vis_rgn = HRGN; b->vis_flags = 0; b->clip_tag = A68V_CLIP_TAG + b->geom_seq + 1;
    }
    b->geom_seq = b->geom_seq + 1;
    cmd(A68V_C_START);

    const double dt = 0.001;
    uint32_t shown = 0, max_ring = 0, max_buffered = 0, start_held = 0, paused_at = 0, stopped = 0, restarted = 0;
    double t_play = -1, t_end = -1;
    uint64_t hash = 1469598103934665603ull;
    int ok = 1;
    int cover_step = -1;                      /* when the cover came */
    uint32_t cpause_at = 0, cov_vis = 0, cov_lost = 0, cov_left = ~0u;
    static uint32_t snap[SW * SH];
    for (int step = 0; step < 300000; step++) {
        double t = step * dt;
        T = (uint64_t)step * 1000u;
        double net = steady || stop || cover ? 2.0 * rate : pause ? 1.25 * rate
                   : bursty ? (t - 6.0 * (int)(t / 6.0) < 1.5 ? 6.0 * rate : 0.0)
                   : (t >= 8.0 && t < 14.0 ? 0.0 : 1.25 * rate);
        deliver(net, dt);
        mixer_step(dt, fed < film_len || b->pos_ms + 1000 < (uint32_t)(secs * 1000));
        psmac_video_poll();
        if (cover) {
            prog_repairs();
            if (!cpaused && !cover_armed && b->pos_ms >= 3000) cover_armed = 1;   /* the next picture's second band */
            if (cpaused && !cpause_at && b->pos_ms >= 3000) { cmd(A68V_C_PAUSE); cpause_at = (uint32_t)step; }
            if (cpaused && cpause_at && !cover_armed && (uint32_t)step - cpause_at == 300) {
                /* paused, the last picture whole on the screen: kept, the screen cleared, and the window "moved" (a geometry
                   change): the card draws the last picture again, and the Finder's window comes in its second band */
                memcpy(snap, screen, sizeof snap);
                memset(screen, 0, sizeof screen);
                cover_armed = 1;
                b->clip_tag = A68V_CLIP_TAG + b->geom_seq + 1;
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                b->geom_seq = b->geom_seq + 1;
            }
            if (cover_done && cover_step < 0) cover_step = step;
            if (cover_step >= 0 && step - cover_step == 1500) {   /* 1.5 s on: what the screen holds */
                cov_left = not_finder();
                for (uint32_t y = 0; y < PH; y++) for (uint32_t x = 0; x < PW; x++) {
                    if (covered(x, y)) continue;
                    if (screen[y * SW + x]) cov_vis++;
                    if (cpaused && screen[y * SW + x] != snap[y * SW + x]) cov_lost++;   /* the last picture, as it was */
                }
                if (cpaused) cmd(A68V_C_RESUME);
            }
        }
        uint32_t lv = b->head - b->tail;
        if (lv > max_ring) max_ring = lv;
        if (b->buffered > max_buffered) max_buffered = b->buffered;
        if (t_play < 0 && b->state == A68V_S_PLAYING) t_play = t;
        if (t_play < 0 && b->buffer_ms > start_held) start_held = b->buffer_ms;   /* what the start waited for */
        if (b->frames_shown != shown) {     /* a picture drawn: into the hash of what the viewer saw */
            shown = b->frames_shown;
            for (uint32_t i = 0; i < SW * SH; i++) hash = (hash ^ screen[i]) * 1099511628211ull;
        }
        if (pause && !paused_at && b->pos_ms >= 4000) { cmd(A68V_C_PAUSE); paused_at = step; }
        if (pause && paused_at && paused_at != ~0u && step - paused_at == 3000) { cmd(A68V_C_RESUME); paused_at = ~0u; }
        if (stop && !stopped && b->pos_ms >= 5000) {
            cmd(A68V_C_STOP); psmac_video_poll();
            stopped = 1;
            if (b->tail != b->head || b->buffered || b->state != A68V_S_IDLE) {
                printf("vbuf_test: %s: after STOP the ring holds %u bytes, the card %u, state %u\n", sc, b->head - b->tail,
                       b->buffered, b->state);
                ok = 0;
            }
            fed = 0; b->eof = 0; credit = 0;  /* the film again from its first byte, as a seek to 0 */
            cmd(A68V_C_START); restarted = 1; t_play = -1; shown = 0; start_held = 0;
            continue;
        }
        if (b->state == A68V_S_ENDED || b->state == A68V_S_ERROR) { t_end = t; break; }
    }
    printf("vbuf_test: %s: %s; %u of %u pictures decoded, %u shown, %u not drawn; started at %.2f s with %u ms held; "
           "rebuffered %u times for %u ms; sound gaps %llu (%llu ms); ended at %.2f s of a %.1f s film; ring at most %u KB, "
           "the card %u KB; %u kbit/s measured, %u actual; pictures %016llx\n",
           sc, b->state == A68V_S_ENDED ? "ended" : (const char *)b->message, b->frames_decoded, frames, b->frames_shown,
           b->frames_dropped, t_play, start_held, b->underruns, b->rebuffer_ms, (unsigned long long)mx.gaps,
           (unsigned long long)(mx.gap * 1000 / 44100), t_end, secs, max_ring >> 10, max_buffered >> 10,
           b->byte_rate * 8u / 1000u, (uint32_t)(rate * 8 / 1000), (unsigned long long)hash);
    if (cover) {
        int bug = cover_mode == 3, given = cover_mode == 1 && strcmp(sc, "cover-repair");
        int good = cover_done && cov_left != ~0u && cov_vis > 0 && !cov_lost &&
                   (bug ? cov_left > 0 : cov_left == 0 && rep_outside == 0 && (given ? cov_px_at_end == 0 : cov_px_at_end > 0));
        printf("vbuf_test: %s: the region changed %s; at that picture's end %u pixels of the Finder's window drawn over, %u repair "
               "request%s carried out (%u rectangles, %u pixels asked for outside the drawn-over part), afterwards %u left; %u pixels "
               "of picture where the window shows%s: %s\n",
               sc, bug ? "after the one reading of the region" : cover_mode == 2 ? "just after a band's look" : "before a band's look",
               cov_px_at_end, rep_done_n, rep_done_n == 1 ? "" : "s", rep_rects, rep_outside, cov_left, cov_vis,
               cpaused ? (cov_lost ? ", NOT the paused picture" : ", the paused picture drawn again whole there") : "",
               !good ? "FAIL" : bug ? "the leftover, as on the owner's SE (video.guard=off)" :
               given ? "the picture given up, nothing drawn over" : "drawn over, then painted again");
        if (!good) ok = 0;
    }
    if (b->state != A68V_S_ENDED) { printf("vbuf_test: %s: did not end\n", sc); ok = 0; }
    if (b->frames_decoded != frames) { printf("vbuf_test: %s: %u pictures decoded, %u in the film\n", sc, b->frames_decoded, frames); ok = 0; }
    if (old || cover_mode == 3) return ok ? 0 : 1;   /* the card as it was: measured, not judged (cover-old's leftover is) */
    if (b->frames_shown != frames) { printf("vbuf_test: %s: %u pictures shown\n", sc, b->frames_shown); ok = 0; }
    if (mx.gaps) { printf("vbuf_test: %s: the sound had gaps\n", sc); ok = 0; }
    if (start_held < start_ms * 9 / 10) { printf("vbuf_test: %s: started with %u ms held, not %u\n", sc, start_held, start_ms); ok = 0; }
    uint32_t want = outage ? 1 : 0;
    if (b->underruns != want) { printf("vbuf_test: %s: rebuffered %u times, not %u\n", sc, b->underruns, want); ok = 0; }
    if (outage && (b->rebuffer_ms < rebuf_ms / 2 || b->rebuffer_ms > 6000 + rebuf_ms)) {
        printf("vbuf_test: %s: %u ms rebuffering\n", sc, b->rebuffer_ms); ok = 0;
    }
    if (stop && !restarted) { printf("vbuf_test: stop: never stopped\n"); ok = 0; }
    if (!old && (bursty || steady || pause) && max_ring > (256u << 10)) { printf("vbuf_test: %s: the ring was not emptied\n", sc); ok = 0; }
    return ok ? 0 : 1;
}
