/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_video.c — the video card's core: an MPEG program stream from a byte ring, decoded, timed against
 * its sound, drawn into the computer's own frame buffer (docs/16-video-card.md, GitHub issue #23).
 *
 * Backstory (22 November 2026, docs/08 item 98). The owner asked for YouTube on the Mac, "elegant", through
 * a card that exists only in the Pi and that Alice68 (the Amiga side of the family) could share. The work
 * is split three ways: a helper on the owner's network does everything modern (search, yt-dlp, ffmpeg)
 * and serves MPEG-1 video with MP2 sound over plain HTTP; the Mac's program, Aurora68 Video, fetches it
 * over the Mac's own TCP/IP and copies the bytes into this card's ring; this file decodes them. Nothing
 * here knows about NuBus, the Mac's Slot Manager or the network: psmac_videocard.c is the Macintosh's
 * card around it (slot $C, its declaration ROM, its screens), and an Amiga build would put a Zorro board
 * or a library around the same file.
 *
 * Where it runs: core 1, the sound core (psmac_usb.c's loop), beside the mixer and PiPlay's MP3 decoder.
 * Core 0 is the 68k, core 2 the bus (the SE's wire), core 3 the services (network, WiFi, the screens'
 * conversion); core 1 only ever waited for USB enumerations and filled the sound output, which takes it
 * about a millisecond in every eight. psmac_video_poll does one slice at a time (a picture decoded, or a
 * few sound frames, or a picture drawn) and returns, so the sound output's DMA ring (48 ms) is looked at
 * between any two; a 320 x 240 picture is a few milliseconds of an A53's time.
 *
 * Timing. pl_mpeg gives every picture and every block of sound a time in the stream (frames counted at
 * the stream's rate from its start). When the stream has sound and the Pi has an output, the clock is the
 * sound: the stream time of the first sample given to the mixer plus the samples the mixer has played
 * since, so a picture is shown when the sound that goes with it is heard, and a sound output that falls
 * behind (an underrun) holds the pictures back with it. Without sound (no sound in the stream, or the
 * testbed with no output: QEMU has no HDMI audio) the Pi's timer is the clock, held while paused and while
 * the stream has run dry. A picture decoded later than its time is still drawn, unless it is more than two
 * frames late and the last one was drawn less than 80 ms ago: MPEG-1 without B-frames cannot skip
 * decoding (every picture is the next one's reference), only drawing, so "dropped" means "not drawn".
 *
 * Drawing. Into the screen's own frame buffer, inside the rectangle the Mac gives, scaled to fit it
 * (nearest pixel; the helper already makes the picture the size the Mac asked for, so this is normally
 * one to one), at the screen's depth:
 *   1 bit: the luminance against an 8 x 8 ordered (Bayer) threshold, black where it is darker: the SE's own
 *          screen, the owner's first wish ("it is going to look crap, but good proof of concept").
 *   2, 4, 8 bits: the colour, an ordered dither added, looked up in a 32,768-entry table of the nearest
 *          entry of the screen's own colour table (made again when the Mac changes the table).
 *   16 and 32 bits: the colour as it is.
 * The screens' own machinery then carries the pixels on: on the SE board the SE's screen rows that
 * changed go over the wire (psmac_board_se.c), and the HDMI screen is converted to the VideoCore 30 times
 * a second (psmac_display.c). So a screenshot of the Mac shows the video too, and a window moved over it
 * is the Mac's business: the Mac says "not visible" and redraws.
 */
#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "psmac_video.h"
#define PLM_NO_STDIO                          /* as psmac_plmpeg.c compiles it: no FILE in the kernel */
#include "third_party/pl_mpeg/pl_mpeg.h"     /* the declarations; psmac_plmpeg.c compiles the decoder */

static inline uint64_t now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static inline uint32_t us_of(uint64_t ticks) { return (uint32_t)(ticks * 1000000u / hz()); }

#define PREFILL     (64u << 10)               /* bytes in the ring before a stream starts: about a second at 320 x 240 */
#define LOAD_CHUNK  (64u << 10)               /* the most taken from the ring at a time */
#define HEAD_LAG    50u                       /* us a new head must have stood (psmac_audio.c, STR_HEAD_LAG) */
#define LEAD        0.4                       /* seconds of sound decoded ahead of the clock */
#define DROP_LATE   2.0                       /* frames late before a picture may go undrawn ... */
#define DROP_GAP_MS 80u                       /* ... if the last one was drawn less than this ago */

static volatile struct a68v_block *blk;
static uint8_t *ring;
static uint32_t nscreens;
static volatile uint32_t reset_req;
static uint32_t head_seen, head_ok;
static uint64_t head_seen_t;

static struct {
    int on, playing, paused, stalled, ended_v, ended_a, logged;
    plm_t *plm;
    plm_buffer_t *buf;
    int headers;
    int sound;                                /* the sound plays and is the clock */
    uint32_t rate, w, h;
    double fps, frame_s;
    plm_frame_t *pending;                     /* decoded, waiting for its time */
    double audio_t0, audio_next;              /* the stream time of the first sample given to the mixer; of the next */
    int audio_t0_set;
    uint64_t wall_t0, hold_t;                 /* the timer's clock: the start, and when it was held (paused, dry) */
    uint64_t last_draw;
    uint32_t fed;                             /* bytes given to pl_mpeg */
    uint32_t decoded, shown, dropped, underruns, streams;
} v;
static struct { uint32_t seq; uint32_t screen; int32_t t, l, b, r; uint32_t visible; } g;
static struct { uint64_t busy, t0, dec, dec_max, draw; uint32_t ndec, ndraw; } st;   /* the last second's numbers */

static void say(const char *s)
{
    int i = 0;
    for (; s[i] && i < 127; i++) blk->message[i] = s[i];
    blk->message[i] = 0;
}

/* ---- the ring ------------------------------------------------------------------------------------ */
static void head_update(void)
{
    uint32_t h = blk->head;
    uint64_t t = now(), lag = hz() / 1000000u * HEAD_LAG;
    if (h != head_seen) {                     /* a new head: the one before is safe if it stood long enough */
        if (t - head_seen_t >= lag) head_ok = head_seen;
        head_seen = h; head_seen_t = t;
    } else if (head_ok != h && t - head_seen_t >= lag) head_ok = h;
}
static uint32_t ring_level(void) { return head_ok - blk->tail; }
static int all_in(void) { return blk->eof && head_ok == blk->head && blk->tail == blk->head; }

/* pl_mpeg's load callback: called whenever its buffer has fewer bytes than a read needs. It takes what the
   ring has, up to LOAD_CHUNK; with the stream all in, it says so, and pl_mpeg then ends at the last byte. */
static void load_cb(plm_buffer_t *b, void *user)
{
    (void)user;
    head_update();
    uint32_t t = blk->tail, n = head_ok - t;
    if ((int32_t)n <= 0) {
        if (all_in()) plm_buffer_signal_end(b);
        return;
    }
    if (n > LOAD_CHUNK) n = LOAD_CHUNK;
    uint32_t o = t & (A68V_RING_SIZE - 1), first = n < A68V_RING_SIZE - o ? n : A68V_RING_SIZE - o;
    __asm__ volatile("dmb ish" ::: "memory");
    plm_buffer_write(b, ring + o, first);
    if (first < n) plm_buffer_write(b, ring, n - first);
    __asm__ volatile("dmb ish" ::: "memory");
    blk->tail = t + n;
    v.fed += n;
}

/* ---- the clock ---------------------------------------------------------------------------------- */
static double clock_now(void)
{
    if (v.sound) return v.audio_t0_set ? v.audio_t0 + (double)psmac_audio_video_played() / v.rate : 0.0;
    uint64_t t = (v.paused || v.stalled) ? v.hold_t : now();
    return (double)(t - v.wall_t0) / (double)hz();
}
static void hold(int on)                      /* the timer's clock held or let go (paused, or dry) */
{
    if (on) v.hold_t = now();
    else v.wall_t0 += now() - v.hold_t;
}
/* the sound ended before the pictures (or it stopped coming): the timer goes on from where the sound was */
static void clock_to_timer(void)
{
    double c = clock_now();
    v.sound = 0;
    v.wall_t0 = now() - (uint64_t)(c * (double)hz());
    blk->sound = 0;
}

/* ---- streams ------------------------------------------------------------------------------------ */
static void close_stream(void)
{
    if (v.plm) plm_destroy(v.plm);            /* the buffer goes with it (plm_create_with_buffer, destroy_when_done) */
    if (v.sound || v.rate) psmac_audio_video_close();
    uint32_t streams = v.streams;
    for (uint32_t i = 0; i < sizeof v; i++) ((uint8_t *)&v)[i] = 0;
    v.streams = streams;
}
static void fail(uint32_t why, const char *text)
{
    kprintf("[VIDEO] %s\n", text);
    close_stream();
    blk->error = why;
    say(text);
    blk->state = A68V_S_ERROR;
}
static void start_stream(void)
{
    close_stream();
    v.buf = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
    if (v.buf) v.plm = plm_create_with_buffer(v.buf, 1);
    if (!v.plm) { fail(A68V_E_NOMEM, "no memory for the decoder"); return; }
    plm_buffer_set_load_callback(v.buf, load_cb, 0);
    v.on = 1;
    v.streams++;
    blk->error = 0; blk->video_w = blk->video_h = blk->fps_x100 = blk->audio_rate = blk->audio_channels = 0;
    blk->pos_ms = blk->frames_decoded = blk->frames_shown = blk->frames_dropped = blk->underruns = 0;
    blk->sound = 0;
    say("buffering");
    blk->state = A68V_S_BUFFERING;
}
static void stop_stream(const char *why)
{
    if (v.on && why) kprintf("[VIDEO] %s at %u.%u s: %u pictures decoded, %u shown, %u not drawn, %u times dry\n", why,
                             blk->pos_ms / 1000, blk->pos_ms / 100 % 10, v.decoded, v.shown, v.dropped, v.underruns);
    close_stream();
    uint32_t h = blk->head;
    blk->tail = h; head_seen = head_ok = h; head_seen_t = now();
    say("idle");
    blk->state = A68V_S_IDLE;
}
/* the stream's first bytes are in: its headers, its sizes, whether its sound will play */
static int open_headers(void)
{
    if (!plm_has_headers(v.plm)) {
        if (v.fed >= A68V_PROBE || all_in()) { fail(A68V_E_NOVIDEO, "no MPEG-1 video found at the start of the stream"); return -1; }
        return 0;
    }
    if (plm_get_num_video_streams(v.plm) < 1) { fail(A68V_E_NOVIDEO, "the stream has no MPEG-1 video"); return -1; }
    v.w = (uint32_t)plm_get_width(v.plm); v.h = (uint32_t)plm_get_height(v.plm);
    if (!v.w || !v.h || v.w > A68V_MAX_W || v.h > A68V_MAX_H) { fail(A68V_E_SIZE, "a picture larger than the card decodes"); return -1; }
    v.fps = plm_get_framerate(v.plm);
    if (v.fps < 1.0 || v.fps > 120.0) v.fps = 25.0;
    v.frame_s = 1.0 / v.fps;
    if (plm_get_num_audio_streams(v.plm) > 0 && plm_get_samplerate(v.plm) > 0) {
        v.rate = (uint32_t)plm_get_samplerate(v.plm);
        v.sound = psmac_audio_video_open(v.rate);
        if (v.sound) psmac_audio_video_volume(blk->volume ? blk->volume : 0x01000100u);
        else plm_set_audio_enabled(v.plm, 0);   /* nothing would play it: pl_mpeg drops its packets */
    }
    blk->video_w = v.w; blk->video_h = v.h; blk->fps_x100 = (uint32_t)(v.fps * 100.0 + 0.5);
    blk->audio_rate = v.rate; blk->audio_channels = v.rate ? 2 : 0; blk->sound = (uint32_t)v.sound;
    kprintf("[VIDEO] stream %u: MPEG-1 %ux%u at %u.%02u frames a second, %s\n", v.streams, v.w, v.h,
            blk->fps_x100 / 100, blk->fps_x100 % 100,
            !v.rate ? "no sound (the Pi's timer is the clock)" : v.sound ? "MP2 sound through the mixer (the sound is the clock)"
                                                               : "its sound not played: no output here (the Pi's timer is the clock)");
    v.headers = 1;
    return 1;
}

/* ---- drawing ------------------------------------------------------------------------------------ */
static const uint8_t bayer[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 } };
static uint8_t lum[256];                      /* video luminance (16-235) to 0-255 */
static int32_t ty[256], tcr_r[256], tcr_g[256], tcb_g[256], tcb_b[256];   /* BT.601, x 256 */
static uint16_t xmap[4096];
static uint32_t xmap_w, xmap_fw;
static uint8_t *ctab;                         /* rgb555 -> the nearest colour table index */
static uint32_t ctab_seq = ~0u, ctab_depth;
static int ctab_gray;

static void tables(void)
{
    for (int i = 0; i < 256; i++) {
        int l = (i - 16) * 255 / 219;
        lum[i] = (uint8_t)(l < 0 ? 0 : l > 255 ? 255 : l);
        ty[i] = 298 * (i - 16) + 128;
        tcr_r[i] = 409 * (i - 128); tcr_g[i] = -208 * (i - 128);
        tcb_g[i] = -100 * (i - 128); tcb_b[i] = 516 * (i - 128);
    }
}
static inline int clamp8(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* the nearest entry of the screen's colour table for each 15-bit colour; 2^depth entries looked at */
static int make_ctab(const struct pv_surface *s)
{
    if (!ctab) ctab = tlsf_malloc(tlsf, 32768);
    if (!ctab) return 0;
    uint32_t n = 1u << s->depth, pal[256];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = s->clut[i] & 0xffffffu;
        if (s->gray) { uint32_t y = (((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 151 + (c & 255) * 28) >> 8; c = y << 16 | y << 8 | y; }
        pal[i] = c;
    }
    uint64_t t0 = now();
    for (uint32_t k = 0; k < 32768; k++) {
        int r = (int)((k >> 10) & 31) * 255 / 31, gg = (int)((k >> 5) & 31) * 255 / 31, b = (int)(k & 31) * 255 / 31;
        if (s->gray) r = gg = b = (r * 77 + gg * 151 + b * 28) >> 8;
        uint32_t best = 0, bd = ~0u;
        for (uint32_t i = 0; i < n; i++) {
            int dr = r - (int)((pal[i] >> 16) & 255), dg = gg - (int)((pal[i] >> 8) & 255), db = b - (int)(pal[i] & 255);
            uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
            if (d < bd) { bd = d; best = i; }
        }
        ctab[k] = (uint8_t)best;
    }
    ctab_seq = s->clut_seq; ctab_depth = s->depth; ctab_gray = s->gray;
    kprintf("[VIDEO] the screen's %u-colour table matched in %u ms\n", n, us_of(now() - t0) / 1000);
    return 1;
}

static void draw(const plm_frame_t *f)
{
    struct pv_surface s;
    if (!g.visible || !psmac_video_surface(g.screen, &s)) return;
    int32_t x0 = g.l, y0 = g.t, x1 = g.r, y1 = g.b;
    int32_t rw = x1 - x0, rh = y1 - y0;
    if (rw <= 0 || rh <= 0 || rw > 4096) return;
    /* the part of the rectangle on the screen */
    int32_t cx0 = x0 < 0 ? 0 : x0, cy0 = y0 < 0 ? 0 : y0;
    int32_t cx1 = x1 > (int32_t)s.width ? (int32_t)s.width : x1, cy1 = y1 > (int32_t)s.height ? (int32_t)s.height : y1;
    if (cx0 >= cx1 || cy0 >= cy1) return;
    uint32_t fw = f->width, fh = f->height;
    if (xmap_w != (uint32_t)rw || xmap_fw != fw) {
        for (int32_t x = 0; x < rw; x++) xmap[x] = (uint16_t)((uint32_t)x * fw / (uint32_t)rw);
        xmap_w = (uint32_t)rw; xmap_fw = fw;
    }
    if (s.depth >= 2 && s.depth <= 8 && (s.clut_seq != ctab_seq || s.depth != ctab_depth || s.gray != ctab_gray || !ctab))
        if (!make_ctab(&s)) return;
    int amp = s.depth == 8 ? 40 : s.depth == 4 ? 64 : 96;          /* the dither's reach, in 0-255 steps */
    for (int32_t y = cy0; y < cy1; y++) {
        uint32_t sy = (uint32_t)(y - y0) * fh / (uint32_t)rh;
        const uint8_t *yr = f->y.data + sy * f->y.width;
        const uint8_t *cbr = f->cb.data + (sy >> 1) * f->cb.width, *crr = f->cr.data + (sy >> 1) * f->cr.width;
        uint8_t *row = s.base + (uint32_t)y * s.rowbytes;
        const uint8_t *bay = bayer[y & 7];
        switch (s.depth) {
        case 1: {
            int cur = cx0 >> 3; uint8_t acc = row[cur];
            for (int32_t x = cx0; x < cx1; x++) {
                int bi = x >> 3;
                if (bi != cur) { row[cur] = acc; cur = bi; acc = row[bi]; }
                uint8_t bit = (uint8_t)(0x80u >> (x & 7));
                if (lum[yr[xmap[x - x0]]] < bay[x & 7] * 4 + 2) acc |= bit; else acc &= (uint8_t)~bit;
            }
            row[cur] = acc;
            break;
        }
        case 16: case 32:
            for (int32_t x = cx0; x < cx1; x++) {
                uint32_t sx = xmap[x - x0];
                int32_t c = ty[yr[sx]], cr = crr[sx >> 1], cb = cbr[sx >> 1];
                int r = clamp8((c + tcr_r[cr]) >> 8), gg = clamp8((c + tcr_g[cr] + tcb_g[cb]) >> 8), b = clamp8((c + tcb_b[cb]) >> 8);
                if (s.depth == 32) ((uint32_t *)row)[x] = (uint32_t)r << 16 | (uint32_t)gg << 8 | (uint32_t)b;
                else ((uint16_t *)row)[x] = (uint16_t)((r >> 3) << 10 | (gg >> 3) << 5 | (b >> 3));
            }
            break;
        default: {                                                   /* 2, 4, 8: dithered into the colour table */
            uint32_t d = s.depth, per = 8 / d, mask = (1u << d) - 1;
            for (int32_t x = cx0; x < cx1; x++) {
                uint32_t sx = xmap[x - x0];
                int32_t c = ty[yr[sx]], cr = crr[sx >> 1], cb = cbr[sx >> 1];
                int off = ((int)bay[x & 7] - 32) * amp / 64;
                int r = clamp8(((c + tcr_r[cr]) >> 8) + off), gg = clamp8(((c + tcr_g[cr] + tcb_g[cb]) >> 8) + off), b = clamp8(((c + tcb_b[cb]) >> 8) + off);
                uint32_t idx = ctab[(r >> 3) << 10 | (gg >> 3) << 5 | (b >> 3)];
                if (d == 8) { row[x] = (uint8_t)idx; continue; }
                uint32_t bi = (uint32_t)x / per, sh = 8 - d - ((uint32_t)x % per) * d;
                row[bi] = (uint8_t)((row[bi] & ~(mask << sh)) | (idx << sh));
            }
            break;
        }
        }
    }
    __asm__ volatile("dmb ish" ::: "memory");
}

static void geometry(void)
{
    uint32_t s1 = blk->geom_seq;
    if (s1 == g.seq) return;
    __asm__ volatile("dmb ish" ::: "memory");
    uint32_t screen = blk->screen, vis = blk->visible;
    int32_t t = blk->top, l = blk->left, b = blk->bottom, r = blk->right;
    __asm__ volatile("dmb ish" ::: "memory");
    if (blk->geom_seq != s1) return;          /* changing while read: next time */
    g.seq = s1; g.screen = screen; g.visible = vis; g.t = t; g.l = l; g.b = b; g.r = r;
}

/* ---- commands ----------------------------------------------------------------------------------- */
static void command(void)
{
    if (__atomic_load_n(&reset_req, __ATOMIC_ACQUIRE)) {
        reset_req = 0;
        stop_stream(v.on ? "the computer was reset: stopped" : 0);
        blk->done_seq = blk->cmd_seq;
        return;
    }
    uint32_t seq = blk->cmd_seq;
    if (seq == blk->done_seq) return;
    __asm__ volatile("dmb ish" ::: "memory");
    switch (blk->cmd) {
    case A68V_C_START: head_update(); start_stream(); break;
    case A68V_C_PAUSE:
        if (v.on && !v.paused && blk->state != A68V_S_ENDED) {
            v.paused = 1;
            if (v.sound) psmac_audio_video_pause(1); else if (!v.stalled) hold(1);
            blk->state = A68V_S_PAUSED; say("paused");
        }
        break;
    case A68V_C_RESUME:
        if (v.on && v.paused) {
            v.paused = 0;
            if (v.sound) psmac_audio_video_pause(0); else if (!v.stalled) hold(0);
            blk->state = v.playing ? A68V_S_PLAYING : A68V_S_BUFFERING; say(v.playing ? "playing" : "buffering");
        }
        break;
    case A68V_C_STOP: stop_stream("stopped"); break;
    }
    blk->done_seq = seq;
}

/* ---- the slice ---------------------------------------------------------------------------------- */
static void numbers(void)
{
    uint64_t t = now();
    if (t - st.t0 < hz()) return;
    blk->load = (uint32_t)(st.busy * 1000u / (t - st.t0));
    blk->decode_us = st.ndec ? us_of(st.dec / st.ndec) : 0;
    blk->decode_us_max = us_of(st.dec_max);
    blk->draw_us = st.ndraw ? us_of(st.draw / st.ndraw) : 0;
    st.busy = st.dec = st.dec_max = st.draw = 0; st.ndec = st.ndraw = 0; st.t0 = t;
}
/* sound: decode while the mixer has room and the sound is less than LEAD ahead of the clock */
static int sound_slice(void)
{
    if (!v.sound || v.ended_a || v.paused) return 0;
    if (psmac_audio_video_room() < PLM_AUDIO_SAMPLES_PER_FRAME) return 0;
    if (v.audio_t0_set && v.audio_next > clock_now() + LEAD) return 0;
    plm_samples_t *sm = plm_decode_audio(v.plm);
    if (!sm) {
        if (plm_has_ended(v.plm)) v.ended_a = 1;
        return 0;
    }
    static int16_t pcm[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
    for (uint32_t i = 0; i < sm->count * 2; i++) {
        float f = sm->interleaved[i] * 32767.0f;
        pcm[i] = (int16_t)(f > 32767.0f ? 32767 : f < -32768.0f ? -32768 : (int)f);
    }
    if (!v.audio_t0_set) { v.audio_t0 = sm->time; v.audio_t0_set = 1; }
    v.audio_next = sm->time + (double)sm->count / v.rate;
    psmac_audio_video_push(pcm, sm->count);
    return 1;
}
static int video_slice(void)
{
    if (v.pending || v.ended_v) return 0;
    uint64_t t0 = now();
    plm_frame_t *f = plm_decode_video(v.plm);
    uint64_t d = now() - t0;
    if (!f) {
        if (plm_has_ended(v.plm)) { v.ended_v = 1; return 1; }
        if (v.decoded && !all_in() && !v.stalled && !v.paused) {   /* dry: hold the timer's clock until bytes come */
            v.stalled = 1; v.underruns++; blk->underruns = v.underruns;
            if (!v.sound) hold(1);
            blk->state = A68V_S_BUFFERING; say("waiting for the stream");
        }
        return 0;
    }
    if (v.stalled) {
        v.stalled = 0;
        if (!v.sound && !v.paused) hold(0);
        if (!v.paused) { blk->state = A68V_S_PLAYING; say("playing"); }
    }
    st.dec += d; st.ndec++; if (d > st.dec_max) st.dec_max = d;
    v.decoded++; blk->frames_decoded = v.decoded;
    v.pending = f;
    if (!v.playing) {                         /* the first picture: the timer's clock starts at its time */
        v.playing = 1;
        v.wall_t0 = now() - (uint64_t)(f->time * (double)hz());
        if (!v.paused) { blk->state = A68V_S_PLAYING; say("playing"); }
    }
    return 1;
}
static int show_slice(void)
{
    if (!v.pending || v.paused) return 0;
    double c = clock_now(), late = c - v.pending->time;
    if (late < 0) return 0;
    uint64_t t = now();
    if (late > DROP_LATE * v.frame_s && t - v.last_draw < hz() / 1000u * DROP_GAP_MS) {
        v.dropped++; blk->frames_dropped = v.dropped;
    } else {
        draw(v.pending);
        uint64_t d = now() - t;
        st.draw += d; st.ndraw++;
        v.last_draw = now();
        v.shown++; blk->frames_shown = v.shown;
    }
    blk->pos_ms = (uint32_t)(v.pending->time * 1000.0);
    v.pending = 0;
    return 1;
}

int psmac_video_poll(void)
{
    if (!blk) return 0;
    uint64_t t0 = now();
    psmac_video_test_poll(blk, ring);
    command();
    geometry();
    if (v.sound && blk->volume) psmac_audio_video_volume(blk->volume);
    int did = 0;
    if (v.on) {
        head_update();
        if (!v.headers) {
            if (ring_level() >= PREFILL || all_in() || (blk->eof && head_ok == blk->head)) {
                int r = open_headers();
                did = r != 0;
            }
        } else {
            did |= sound_slice();
            if (!did) did |= video_slice();
            did |= show_slice();
            if (v.sound && v.ended_a && psmac_audio_video_queued() == 0 && !v.ended_v) clock_to_timer();
            if (v.ended_v && !v.pending && (!v.sound || (v.ended_a && psmac_audio_video_queued() == 0)) && !v.logged) {
                v.logged = 1;
                kprintf("[VIDEO] stream %u played to the end, %u.%u s: %u pictures decoded, %u shown, %u not drawn, %u times dry\n",
                        v.streams, blk->pos_ms / 1000, blk->pos_ms / 100 % 10, v.decoded, v.shown, v.dropped, v.underruns);
                uint32_t pos = blk->pos_ms;
                close_stream();
                blk->pos_ms = pos;
                say("ended");
                blk->state = A68V_S_ENDED;
            }
        }
        if (v.buf) blk->buffered = (uint32_t)plm_buffer_get_remaining(v.buf) + ring_level();
    }
    if (did) st.busy += now() - t0;
    numbers();
    return did;
}

void psmac_video_reset(void) { __atomic_store_n(&reset_req, 1, __ATOMIC_RELEASE); }

void psmac_video_init(uint8_t *win, uint32_t screens)
{
    tables();
    blk = (volatile struct a68v_block *)(win + A68V_OFF_BLOCK);
    ring = win + A68V_OFF_RING;
    nscreens = screens;
    for (uint32_t i = 0; i < sizeof(struct a68v_block) / 4; i++) ((volatile uint32_t *)blk)[i] = 0;
    blk->win_size = A68V_WIN_SIZE; blk->ring_off = A68V_OFF_RING; blk->ring_size = A68V_RING_SIZE;
    blk->screens = screens; blk->volume = 0x01000100u;
    say("idle");
    st.t0 = now();
    __asm__ volatile("dmb ish" ::: "memory");
    blk->version = A68V_VERSION; blk->magic = A68V_MAGIC;
}

void psmac_video_report(void)
{
    static uint32_t last_state = ~0u, last_streams;
    if (!blk || !v.streams) return;
    static const char *const sn[] = { "idle", "buffering", "playing", "paused", "ended", "error" };
    uint32_t s = blk->state < 6 ? blk->state : 5;
    int quiet = s == A68V_S_IDLE || s == A68V_S_ENDED || s == A68V_S_ERROR;   /* said once, not every five seconds */
    if (quiet && s == last_state && v.streams == last_streams) return;
    last_state = s; last_streams = v.streams;
    kprintf("[PSMAC]   video: %s, stream %u %ux%u, %u.%u s, %u decoded, %u shown, %u not drawn, %u dry; decode %u us (at most %u), draw %u us, "
            "%u per mille of core 1; %u KB waiting, screen %u at %d,%d-%d,%d%s, sound %s\n",
            sn[s], v.streams, blk->video_w, blk->video_h, blk->pos_ms / 1000, blk->pos_ms / 100 % 10, blk->frames_decoded,
            blk->frames_shown, blk->frames_dropped, blk->underruns, blk->decode_us, blk->decode_us_max, blk->draw_us, blk->load,
            blk->buffered >> 10, g.screen, g.l, g.t, g.r, g.b, g.visible ? "" : " (hidden)", blk->sound ? "on" : "off");
}
