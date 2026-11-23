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
 * Buffering (23 November 2026, docs/08 item 125). On the owner's SE over WiFi the classic Play's music videos
 * stuttered whenever the network slowed for a moment ("might need a bit more buffering also"): the ring is
 * 512 KiB, 3.5 s at 512 x 342 and less at the sizes Play asks for HDMI, a stream started after only 64 KiB,
 * and with sound a stream that ran dry stopped and started with every few packets that came. Three changes,
 * none of them in the window the Mac sees:
 *  - A buffer in the Pi's memory behind the ring (8 MiB by default, mac.vbuffer=MB, taken from the kernel's
 *    heap at the first stream and kept; 0: the ring alone, as before). Every slice moves what the Mac has
 *    written from the ring into it (drain), so the ring is nearly always empty and the Mac, and through TCP
 *    the server, can send at the network's full rate for as long as the buffer has room; pl_mpeg's load_cb
 *    reads the buffer. A STOP empties both.
 *  - Buffering by time, not bytes. The stream's own clock is in its pack headers (the SCR, 90 kHz), so the
 *    bytes from one pack to the next against the SCR between them give its rate (scan, as the bytes are
 *    moved), and bytes held / rate the seconds held. A stream starts when that is mac.vprefill= seconds (3 by
 *    default), or the whole stream is in, or the buffer is full.
 *  - Rebuffering. When the decoder finds nothing more to decode while the stream is not all in (the pictures,
 *    or the sound with less than 100 ms of it left in the mixer), the clock stops (the sound paused, the timer
 *    held) and the card buffers again, to the second number of mac.vprefill= (2 s by default), before going
 *    on: one pause and then smooth play, rather than a stutter at every few packets.
 * The block (version 2) says how much is held (buffer_ms, buffer_pct of buffer_target_ms), the buffer's size,
 * the stream's rate and the time spent rebuffering; each stream's end logs the high-water mark too.
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
 * Since 23 November 2026 (Aurora68 Play 1.8, docs/08 item 124) a program may also hand over its window's visible
 * region (the block's vis_rgn): the card then reads the region at every picture and draws only where the window
 * shows, so a video behind another program's window, or partly under one, goes on where it can be seen; and no
 * picture is drawn while a menu is down (the menu bar's saved bits, MBSaveLoc), whichever program pulled it down.
 */
#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "psmac_video.h"
#define PLM_NO_STDIO                          /* as psmac_plmpeg.c compiles it: no FILE in the kernel */
#include "third_party/pl_mpeg/pl_mpeg.h"     /* the declarations; psmac_plmpeg.c compiles the decoder */

#ifdef PSMAC_VIDEO_HOST                       /* tools/vbuf_test.c: this file on the host, against a simulated clock */
uint64_t pv_host_now(void);
static inline uint64_t now(void) { return pv_host_now(); }
static inline uint64_t hz(void)  { return 1000000u; }
#define DMB() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#else
static inline uint64_t now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
#define DMB() __asm__ volatile("dmb ish" ::: "memory")
#endif
static inline uint32_t us_of(uint64_t ticks) { return (uint32_t)(ticks * 1000000u / hz()); }
static inline uint32_t ms_of(uint64_t ticks) { return (uint32_t)(ticks * 1000u / hz()); }

#define PREFILL     (64u << 10)               /* bytes held before the headers are looked for (the start waits for time) */
#define LOAD_CHUNK  (64u << 10)               /* the most given to pl_mpeg at a time */
#define DRAIN_MAX   (256u << 10)              /* the most moved from the ring to the Pi's buffer in one slice (~0.2 ms) */
#define HEAP_KEEP   (8u << 20)                /* the kernel's heap always left to the rest (psmac_card.c, psmac_usbdisk.c) */
#define RATE_GUESS  330000u                   /* bytes a second assumed before the packs say: the server's most, 2.5 Mbit/s of
                                                 video and its sound (assumed high, a start waits longer, never shorter) */
#define SOUND_LOW_MS 100u                     /* the mixer's sound left when a starved decoder rebuffers */
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

/* the amounts (psmac_video_config: mac.vbuffer=, mac.vprefill=) */
static struct { uint32_t fifo, start_ms, rebuf_ms; } cfg = { 8u << 20, 3000u, 2000u };
/* the Pi's buffer behind the ring: written by drain, read by load_cb, both on this core */
static struct { uint8_t *buf; uint32_t size, rd, wr, level; } fq;
/* the stream's pack headers as the bytes go by: its rate from the bytes between two packs and their SCRs */
static struct {
    uint32_t sh; int got; uint8_t b[5];
    uint64_t pos, pack_pos;                   /* bytes seen this stream; where the pack being read began */
    int have; uint64_t scr, ppos;             /* the last pack's SCR (90 kHz) and place */
    uint64_t bytes, ticks;                    /* summed over plausible steps (0 < SCR step <= 2 s) */
} sc;

static struct {
    int on, playing, paused, stalled, ended_v, ended_a, logged;
    int primed;                               /* the start's buffering done (mac.vprefill=) */
    int held;                                 /* the clock held: paused or stalled */
    int starving;                             /* the sound's decoder found nothing more: rebuffer when the mixer is low */
    uint64_t t_start, t_primed, rebuf_t0;     /* START, the first picture's go, the rebuffering begun */
    uint64_t rebuf_ticks;                     /* the time spent rebuffering */
    uint32_t rebuffers, primed_ms, primed_kb;
    uint32_t hw_bytes, hw_ms;                 /* the high-water mark: the most held, in bytes and in time */
    uint32_t pct_said;
    plm_t *plm;
    plm_buffer_t *buf;
    int headers;
    int sound;                                /* the sound plays and is the clock */
    uint32_t rate, w, h;
    double fps, frame_s;
    plm_frame_t *pending;                     /* decoded, waiting for its time */
    plm_frame_t last;                         /* the picture last drawn (its planes stay while at most one more is decoded) */
    int have_last;
    double audio_t0, audio_next;              /* the stream time of the first sample given to the mixer; of the next */
    int audio_t0_set;
    uint64_t wall_t0, hold_t;                 /* the timer's clock: the start, and when it was held (paused, dry) */
    uint64_t last_draw;
    uint32_t fed;                             /* bytes given to pl_mpeg */
    uint32_t decoded, shown, dropped, underruns, streams;
} v;
static struct { uint32_t seq; uint32_t screen; int32_t t, l, b, r; uint32_t visible; uint32_t rgn, vflags; } g;
static uint32_t menu_skips, rgn_skips, rgn_parts;   /* pictures not drawn under a menu, or for a region that could not
                                                       be read; the parts of the last region drawn (the heartbeat's) */
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
/* the whole stream has left the ring (it may still be in the Pi's buffer) */
static int all_in(void) { return blk->eof && head_ok == blk->head && blk->tail == blk->head; }
/* ... and the Pi's buffer is empty too: pl_mpeg has every byte */
static int input_done(void) { return all_in() && fq.level == 0; }
static uint32_t input_level(void) { return fq.level + ring_level(); }

/* ---- the stream's rate, from its pack headers ----------------------------------------------------- */
static void pack(void)
{
    const uint8_t *b = sc.b;
    if ((b[0] & 0xf1) != 0x21 || !(b[2] & 1) || !(b[4] & 1)) return;     /* not an MPEG-1 pack's '0010 xxx1' */
    uint64_t scr = (uint64_t)((b[0] >> 1) & 7) << 30 | (uint64_t)b[1] << 22 | (uint64_t)(b[2] >> 1) << 15
                 | (uint64_t)b[3] << 7 | (uint64_t)(b[4] >> 1);
    if (sc.have) {                            /* a jump (a seek's new place, a new film) is left out */
        int64_t d = (int64_t)(scr - sc.scr);
        if (d > 0 && d <= 180000) { sc.bytes += sc.pack_pos - sc.ppos; sc.ticks += (uint64_t)d; }
    }
    sc.have = 1; sc.scr = scr; sc.ppos = sc.pack_pos;
}
static void scan(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (sc.got >= 0) {
            sc.b[sc.got++] = c;
            if (sc.got == 5) { pack(); sc.got = -1; }
            continue;
        }
        sc.sh = sc.sh << 8 | c;
        if (sc.sh == 0x000001bau) { sc.got = 0; sc.pack_pos = sc.pos + i - 3; sc.sh = ~0u; }
    }
    sc.pos += n;
}
static double clock_now(void);
/* bytes a second: the packs' own rate once a quarter of a second of SCR has gone by; else what the decoder has
   used against the stream's time; 0 if neither is known yet. The bytes after the last pack count with the time up
   to it, so the rate is never less than the stream's: FFmpeg's first pack is a few KB followed by half a second of
   SCR (its -muxdelay), then 250 KB in the next step at 1.3 Mbit/s, and a rate of the closed steps alone said 360
   kbit/s there and started a stream with less than a second held (tools/vbuf_test.c's outage, 23 November 2026) */
static uint32_t byte_rate(void)
{
    uint64_t r = 0;
    if (sc.ticks >= 22500u) r = (sc.bytes + (sc.pos - sc.ppos)) * 90000u / sc.ticks;
    else if (v.playing && v.buf) {
        double c = clock_now();
        uint32_t rem = (uint32_t)plm_buffer_get_remaining(v.buf);
        if (c > 1.0 && v.fed > rem) r = (uint64_t)((double)(v.fed - rem) / c);
    }
    return r >= 2000u && r <= (20u << 20) ? (uint32_t)r : 0;
}
/* what the card holds, as stream time: the bytes not yet decoded at the stream's rate, and the mixer's sound */
static uint32_t held_bytes(void) { return input_level() + (v.buf ? (uint32_t)plm_buffer_get_remaining(v.buf) : 0); }
static uint32_t held_ms(void)
{
    uint32_t r = byte_rate();
    uint64_t ms = (uint64_t)held_bytes() * 1000u / (r ? r : RATE_GUESS);
    if (v.sound && v.rate) ms += (uint64_t)psmac_audio_video_queued() * 1000u / v.rate;
    return ms > 0xffffffu ? 0xffffffu : (uint32_t)ms;
}
/* the card can take no more: the Mac is held back by a full ring with the Pi's buffer full behind it */
static int input_full(void)
{
    return fq.buf ? fq.size - fq.level < 2 * LOAD_CHUNK : ring_level() + LOAD_CHUNK >= A68V_RING_SIZE;
}
/* enough to (re)start: the target's time held, or everything in, or no room for more */
static int ready(uint32_t target_ms) { return !target_ms || all_in() || input_full() || held_ms() >= target_ms; }

/* ---- the Pi's buffer ------------------------------------------------------------------------------- */
static void fq_put(const uint8_t *p, uint32_t n)       /* n at most the room */
{
    scan(p, n);
    uint32_t first = n < fq.size - fq.wr ? n : fq.size - fq.wr;
    memcpy(fq.buf + fq.wr, p, first);
    if (first < n) memcpy(fq.buf, p + first, n - first);
    fq.wr = (fq.wr + n) % fq.size; fq.level += n;
}
static void fq_get(plm_buffer_t *b, uint32_t n)         /* n at most the level */
{
    uint32_t first = n < fq.size - fq.rd ? n : fq.size - fq.rd;
    plm_buffer_write(b, fq.buf + fq.rd, first);
    if (first < n) plm_buffer_write(b, fq.buf, n - first);
    fq.rd = (fq.rd + n) % fq.size; fq.level -= n;
}
static void fq_reset(void)
{
    fq.rd = fq.wr = fq.level = 0;
    for (uint32_t i = 0; i < sizeof sc; i++) ((uint8_t *)&sc)[i] = 0;
    sc.got = -1; sc.sh = ~0u;
}
/* at a stream's start: the buffer, once, from the kernel's heap (halved until it fits, down to 1 MiB) */
static void fq_alloc(void)
{
    if (fq.buf || !cfg.fifo) return;
    for (uint32_t want = cfg.fifo; want >= (1u << 20); want >>= 1) {
        if (tlsf_get_free_size(tlsf) < (uintptr_t)want + HEAP_KEEP) continue;
        if ((fq.buf = tlsf_malloc(tlsf, want))) { fq.size = want; break; }
    }
    if (fq.buf) kprintf("[VIDEO] a buffer of %u KB behind the card's ring (mac.vbuffer=%u MB asked; %u KB of the kernel's heap left)\n",
                        fq.size >> 10, cfg.fifo >> 20, (uint32_t)(tlsf_get_free_size(tlsf) >> 10));
    else kprintf("[VIDEO] no room in the kernel's heap for a buffer behind the ring (%u MB asked): the ring alone\n", cfg.fifo >> 20);
    blk->buffer_size = fq.size;
}
/* the ring emptied into the Pi's buffer as far as there is room: the Mac's bytes, then the tail it waits for */
static int drain(void)
{
    if (!fq.buf) return 0;
    head_update();
    uint32_t t = blk->tail, n = head_ok - t, room = fq.size - fq.level;
    if ((int32_t)n <= 0 || !room) return 0;
    if (n > room) n = room;
    if (n > DRAIN_MAX) n = DRAIN_MAX;
    uint32_t o = t & (A68V_RING_SIZE - 1), first = n < A68V_RING_SIZE - o ? n : A68V_RING_SIZE - o;
    DMB();                                    /* the bytes read after the head that covers them */
    fq_put(ring + o, first);
    if (first < n) fq_put(ring, n - first);
    DMB();                                    /* and copied before the Mac may write over them */
    blk->tail = t + n;
    return 1;
}

/* pl_mpeg's load callback: called whenever its buffer has fewer bytes than a read needs. It takes what the
   Pi's buffer has (or, without one, the ring), up to LOAD_CHUNK; with the stream all in, it says so, and
   pl_mpeg then ends at the last byte. */
static void load_cb(plm_buffer_t *b, void *user)
{
    (void)user;
    if (fq.buf) {
        drain();
        uint32_t n = fq.level;
        if (!n) {
            if (input_done()) plm_buffer_signal_end(b);
            return;
        }
        if (n > LOAD_CHUNK) n = LOAD_CHUNK;
        fq_get(b, n);
        v.fed += n;
        return;
    }
    head_update();
    uint32_t t = blk->tail, n = head_ok - t;
    if ((int32_t)n <= 0) {
        if (all_in()) plm_buffer_signal_end(b);
        return;
    }
    if (n > LOAD_CHUNK) n = LOAD_CHUNK;
    uint32_t o = t & (A68V_RING_SIZE - 1), first = n < A68V_RING_SIZE - o ? n : A68V_RING_SIZE - o;
    DMB();
    scan(ring + o, first);
    plm_buffer_write(b, ring + o, first);
    if (first < n) { scan(ring, n - first); plm_buffer_write(b, ring, n - first); }
    DMB();
    blk->tail = t + n;
    v.fed += n;
}

/* ---- the clock ---------------------------------------------------------------------------------- */
static double clock_now(void)
{
    if (v.sound) return v.audio_t0_set ? v.audio_t0 + (double)psmac_audio_video_played() / v.rate : 0.0;
    uint64_t t = v.held ? v.hold_t : now();
    return (double)(t - v.wall_t0) / (double)hz();
}
static void hold(int on)                      /* the timer's clock held or let go (paused, or dry) */
{
    if (on) v.hold_t = now();
    else v.wall_t0 += now() - v.hold_t;
}
/* the clock stands while the stream is paused or rebuffering, and goes on when neither: the sound paused in the
   mixer (what it holds is kept) or the timer held */
static void clock_sync(void)
{
    int h = v.paused || v.stalled;
    if (h == v.held) return;
    v.held = h;
    if (v.sound) psmac_audio_video_pause(h);
    else hold(h);
}
/* the sound ended before the pictures (or it stopped coming): the timer goes on from where the sound was */
static void clock_to_timer(void)
{
    double c = clock_now();
    if (v.held) psmac_audio_video_pause(0);
    v.sound = 0;
    v.wall_t0 = now() - (uint64_t)(c * (double)hz());
    v.hold_t = now();
    blk->sound = 0;
}

/* ---- streams ------------------------------------------------------------------------------------ */
/* the buffering's numbers for the stream's last log line */
static void buffer_report(void)
{
    if (!v.on) return;
    if (v.stalled) v.rebuf_ticks += now() - v.rebuf_t0;
    uint32_t r = byte_rate(), rb = ms_of(v.rebuf_ticks);
    kprintf("[VIDEO] stream %u's buffer: %u KB (%u.%u s) at most, of %u KB%s; started after %u.%u s with %u.%u s (%u KB) held; "
            "rebuffered %u time%s, %u.%u s in all; %u kbit/s\n", v.streams, v.hw_bytes >> 10, v.hw_ms / 1000, v.hw_ms / 100 % 10,
            (fq.size ? fq.size : A68V_RING_SIZE) >> 10, fq.size ? " behind the ring" : " (the ring alone)",
            v.t_primed ? ms_of(v.t_primed - v.t_start) / 1000 : 0, v.t_primed ? ms_of(v.t_primed - v.t_start) / 100 % 10 : 0,
            v.primed_ms / 1000, v.primed_ms / 100 % 10, v.primed_kb, v.rebuffers, v.rebuffers == 1 ? "" : "s",
            rb / 1000, rb / 100 % 10, r * 8u / 1000u);
}
static void close_stream(void)
{
    if (v.plm) plm_destroy(v.plm);            /* the buffer goes with it (plm_create_with_buffer, destroy_when_done) */
    if (v.sound || v.rate) psmac_audio_video_close();
    uint32_t streams = v.streams;
    for (uint32_t i = 0; i < sizeof v; i++) ((uint8_t *)&v)[i] = 0;
    v.streams = streams;
    fq_reset();                               /* what the Pi's buffer held goes with the stream */
}
static void fail(uint32_t why, const char *text)
{
    kprintf("[VIDEO] %s\n", text);
    buffer_report();
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
    fq_alloc();
    v.on = 1;
    v.streams++;
    v.t_start = now();
    v.pct_said = ~0u;
    blk->error = 0; blk->video_w = blk->video_h = blk->fps_x100 = blk->audio_rate = blk->audio_channels = 0;
    blk->pos_ms = blk->frames_decoded = blk->frames_shown = blk->frames_dropped = blk->underruns = 0;
    blk->sound = 0;
    blk->buffer_ms = blk->buffer_pct = blk->rebuffer_ms = blk->byte_rate = 0;
    blk->buffer_target_ms = cfg.start_ms;
    say("buffering");
    blk->state = A68V_S_BUFFERING;
}
static void stop_stream(const char *why)
{
    if (v.on && why) {
        kprintf("[VIDEO] %s at %u.%u s: %u pictures decoded, %u shown, %u not drawn, %u times dry\n", why,
                blk->pos_ms / 1000, blk->pos_ms / 100 % 10, v.decoded, v.shown, v.dropped, v.underruns);
        buffer_report();
    }
    close_stream();
    uint32_t h = blk->head;
    blk->tail = h; head_seen = head_ok = h; head_seen_t = now();
    blk->buffered = blk->buffer_ms = blk->buffer_pct = 0;
    say("idle");
    blk->state = A68V_S_IDLE;
}
/* the stream's first bytes are in: its headers, its sizes, whether its sound will play */
static int open_headers(void)
{
    if (!plm_has_headers(v.plm)) {
        if (v.fed >= A68V_PROBE || input_done()) { fail(A68V_E_NOVIDEO, "no MPEG-1 video found at the start of the stream"); return -1; }
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

/* the window's visible region as rectangles (23 November 2026, docs/08 item 124). A QuickDraw region: rgnSize, rgnBBox,
   then (unless rgnSize is 10, a rectangle) inversion lines: a y, the x's where the region's inside changes from that
   line down, $7FFF; the whole ends with another $7FFF. Read through the Mac's memory map at every picture (the Window
   Manager changes it whenever a window moves, also another program's), each band of lines with the same x's a set of
   rectangles, a band like the one above it merged into it; in the window's coordinates, the picture's rectangle at
   0,0, moved to the screen's by x0, y0 and cut to the part on the screen. Anything that does not read as a region (a
   block moving in the heap as it is read) is -1: that picture is not drawn */
#define MAX_PARTS 16
static int rd16(uint32_t a, uint32_t *v)
{
    int ok;
    uint32_t w = psmac_video_rd32(a & ~3u, &ok);
    *v = a & 2 ? w & 0xFFFFu : w >> 16;
    return ok;
}
static int vis_parts(int32_t out[MAX_PARTS][4], int32_t x0, int32_t y0, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1)
{
    int ok, n = 0, nx = 0, k;
    uint32_t mp = psmac_video_rd32(g.rgn, &ok), size, w, a;
    int32_t xs[32], bb[4];
    if (!ok || !mp) return -1;
    if (g.vflags & A68V_VF_24BIT) mp &= 0x00FFFFFFu;
    if ((mp & 1) || !rd16(mp, &size) || size < 10 || size > 8192) return -1;
    for (k = 0; k < 4; k++) { if (!rd16(mp + 2 + 2 * (uint32_t)k, &w)) return -1; bb[k] = (int16_t)w; }
    if (size == 10) {                         /* a rectangle (or empty) */
        int32_t t = bb[0] + y0, l = bb[1] + x0, b = bb[2] + y0, r = bb[3] + x0;
        if (t < cy0) t = cy0;
        if (l < cx0) l = cx0;
        if (b > cy1) b = cy1;
        if (r > cx1) r = cx1;
        if (t >= b || l >= r) return 0;
        out[0][0] = t; out[0][1] = l; out[0][2] = b; out[0][3] = r;
        return 1;
    }
    a = mp + 10;
    uint32_t end = mp + size;
    if (a + 2 > end || !rd16(a, &w)) return -1;
    a += 2;
    while (w != 0x7FFF) {
        int32_t y = (int16_t)w, ny;
        for (;;) {                            /* this line's x's toggle the inside */
            if (a + 2 > end || !rd16(a, &w)) return -1;
            a += 2;
            if (w == 0x7FFF) break;
            int32_t x = (int16_t)w;
            for (k = 0; k < nx && xs[k] < x; k++) { }
            if (k < nx && xs[k] == x) { for (; k + 1 < nx; k++) xs[k] = xs[k + 1]; nx--; }
            else {
                if (nx >= 32) return -1;
                for (int j = nx; j > k; j--) xs[j] = xs[j - 1];
                xs[k] = x; nx++;
            }
        }
        if (a + 2 > end || !rd16(a, &w)) return -1;
        a += 2;
        if (w == 0x7FFF) break;
        ny = (int16_t)w;
        if (ny <= y) return -1;               /* not a region: lines go down */
        for (k = 0; k + 1 < nx; k += 2) {     /* the band y..ny */
            int32_t t = y + y0, l = xs[k] + x0, b = ny + y0, r = xs[k + 1] + x0, j;
            if (t < cy0) t = cy0;
            if (l < cx0) l = cx0;
            if (b > cy1) b = cy1;
            if (r > cx1) r = cx1;
            if (t >= b || l >= r) continue;
            for (j = 0; j < n; j++)           /* the same columns just above: one rectangle */
                if (out[j][2] == t && out[j][1] == l && out[j][3] == r) { out[j][2] = b; break; }
            if (j < n) continue;
            if (n == MAX_PARTS) return -1;    /* more pieces than this: not drawn rather than drawn over a window */
            out[n][0] = t; out[n][1] = l; out[n][2] = b; out[n][3] = r; n++;
        }
    }
    return n;
}

/* the part cx0..cx1, cy0..cy1 of the picture's rectangle (x0, y0, rw x rh, on the screen s) */
static void draw_area(const plm_frame_t *f, const struct pv_surface *ps, int32_t x0, int32_t y0, int32_t rw, int32_t rh,
                      int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1)
{
    struct pv_surface s = *ps;
    uint32_t fh = f->height;
    int amp = s.depth == 8 ? 40 : s.depth == 4 ? 64 : 96;          /* the dither's reach, in 0-255 steps */
    (void)rw;
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
}
static void draw(const plm_frame_t *f)
{
    struct pv_surface s;
    if (!g.visible || !psmac_video_surface(g.screen, &s)) return;
    if (psmac_video_menu_down()) { menu_skips++; return; }   /* a menu over the screen: whichever program's */
    int32_t x0 = g.l, y0 = g.t, x1 = g.r, y1 = g.b;
    int32_t rw = x1 - x0, rh = y1 - y0;
    if (rw <= 0 || rh <= 0 || rw > 4096) return;
    /* the part of the rectangle on the screen */
    int32_t cx0 = x0 < 0 ? 0 : x0, cy0 = y0 < 0 ? 0 : y0;
    int32_t cx1 = x1 > (int32_t)s.width ? (int32_t)s.width : x1, cy1 = y1 > (int32_t)s.height ? (int32_t)s.height : y1;
    if (cx0 >= cx1 || cy0 >= cy1) return;
    uint32_t fw = f->width;
    if (xmap_w != (uint32_t)rw || xmap_fw != fw) {
        for (int32_t x = 0; x < rw; x++) xmap[x] = (uint16_t)((uint32_t)x * fw / (uint32_t)rw);
        xmap_w = (uint32_t)rw; xmap_fw = fw;
    }
    if (s.depth >= 2 && s.depth <= 8 && (s.clut_seq != ctab_seq || s.depth != ctab_depth || s.gray != ctab_gray || !ctab))
        if (!make_ctab(&s)) return;
    if (!g.rgn) draw_area(f, &s, x0, y0, rw, rh, cx0, cy0, cx1, cy1);
    else {                                    /* only the parts of the window that show (its visRgn, read now) */
        int32_t rr[MAX_PARTS][4];
        int n = vis_parts(rr, x0, y0, cx0, cy0, cx1, cy1);
        if (n < 0) { rgn_skips++; return; }   /* the region could not be read (moving in the Mac's heap): not this picture */
        rgn_parts = (uint32_t)n;
        for (int k = 0; k < n; k++)
            draw_area(f, &s, x0, y0, rw, rh, rr[k][1], rr[k][0], rr[k][3], rr[k][2]);
    }
    DMB();
}

static void geometry(void)
{
    uint32_t s1 = blk->geom_seq;
    if (s1 == g.seq) return;
    DMB();
    uint32_t screen = blk->screen, vis = blk->visible;
    int32_t t = blk->top, l = blk->left, b = blk->bottom, r = blk->right;
    int tagged = blk->clip_tag == A68V_CLIP_TAG + s1;   /* a stale tag: another program's place, the whole rectangle */
    uint32_t rgn = tagged ? blk->vis_rgn : 0, vflags = blk->vis_flags;
    DMB();
    if (blk->geom_seq != s1) return;          /* changing while read: next time */
    g.seq = s1; g.screen = screen; g.visible = vis; g.t = t; g.l = l; g.b = b; g.r = r;
    g.rgn = rgn; g.vflags = vflags;
    /* the window moved, was uncovered or brought back while no new picture is coming (paused, ended, dry): the
       last picture drawn again, so the program never has to paint the picture area itself (22 November 2026) */
    if (vis && v.on && v.have_last && (v.paused || v.stalled || v.ended_v)) draw(&v.last);
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
    DMB();
    switch (blk->cmd) {
    case A68V_C_START: head_update(); start_stream(); break;
    case A68V_C_PAUSE:
        if (v.on && !v.paused && blk->state != A68V_S_ENDED) {
            v.paused = 1;
            clock_sync();
            blk->state = A68V_S_PAUSED; say("paused");
        }
        break;
    case A68V_C_RESUME:
        if (v.on && v.paused) {
            v.paused = 0;
            clock_sync();
            int on = v.playing && !v.stalled;
            blk->state = on ? A68V_S_PLAYING : A68V_S_BUFFERING; say(on ? "playing" : "buffering");
            v.pct_said = ~0u;
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
/* ---- rebuffering ------------------------------------------------------------------------------- */
/* the decoder starved while the stream is not all in: the clock stops and the card buffers to the rebuffer target */
static void rebuffer(void)
{
    if (v.stalled || v.paused || !v.primed || all_in()) return;
    v.stalled = 1; v.starving = 0;
    v.underruns++; blk->underruns = v.underruns;
    v.rebuffers++;
    v.rebuf_t0 = now();
    clock_sync();
    blk->buffer_target_ms = cfg.rebuf_ms;
    blk->state = A68V_S_BUFFERING; say("buffering"); v.pct_said = ~0u;
    if (cfg.rebuf_ms && v.rebuffers <= 10)   /* with mac.vprefill=0 it is every few packets: the end's line counts them */
        kprintf("[VIDEO] stream %u ran dry at %u.%u s: buffering %u.%u s before going on\n", v.streams, blk->pos_ms / 1000,
                blk->pos_ms / 100 % 10, cfg.rebuf_ms / 1000, cfg.rebuf_ms / 100 % 10);
}
/* enough again (or paused meanwhile: the resume finds the clock as the pause left it) */
static void rebuffered(void)
{
    if (!v.stalled || !ready(cfg.rebuf_ms)) return;
    v.stalled = 0; v.starving = 0;
    v.rebuf_ticks += now() - v.rebuf_t0;
    blk->rebuffer_ms = ms_of(v.rebuf_ticks);
    clock_sync();
    if (!v.paused && v.playing) { blk->state = A68V_S_PLAYING; say("playing"); }
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
        else if (v.audio_t0_set && !all_in()) v.starving = 1;   /* rebuffers when the mixer is nearly out (the poll) */
        return 0;
    }
    v.starving = 0;
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
        if (v.decoded) rebuffer();            /* dry: the clock stops until the rebuffer target is in */
        return 0;
    }
    st.dec += d; st.ndec++; if (d > st.dec_max) st.dec_max = d;
    v.decoded++; blk->frames_decoded = v.decoded;
    v.pending = f;
    if (!v.playing) {                         /* the first picture: the timer's clock starts at its time */
        v.playing = 1;
        v.wall_t0 = now() - (uint64_t)(f->time * (double)hz());
        v.hold_t = now();
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
        v.last = *v.pending; v.have_last = 1;
        uint64_t d = now() - t;
        st.draw += d; st.ndraw++;
        v.last_draw = now();
        v.shown++; blk->frames_shown = v.shown;
    }
    blk->pos_ms = (uint32_t)(v.pending->time * 1000.0);
    v.pending = 0;
    return 1;
}

/* the block's buffering numbers, and the high-water mark */
static void buffer_numbers(void)
{
    uint32_t b = held_bytes(), ms = held_ms(), tgt = blk->buffer_target_ms;
    uint32_t pct = !tgt || ms >= tgt ? 100u : ms * 100u / tgt;
    if (all_in() && blk->state == A68V_S_BUFFERING) pct = 100;
    blk->buffered = b; blk->buffer_ms = ms; blk->buffer_pct = pct; blk->byte_rate = byte_rate();
    blk->rebuffer_ms = ms_of(v.rebuf_ticks + (v.stalled ? now() - v.rebuf_t0 : 0));
    if (b > v.hw_bytes) v.hw_bytes = b;
    if (ms > v.hw_ms) v.hw_ms = ms;
    if (blk->state == A68V_S_BUFFERING && pct != v.pct_said) {   /* "buffering 40%": what a program may show as it is */
        char t[24] = "buffering ";
        int i = 10;
        if (pct >= 100) t[i++] = '1';
        if (pct >= 10) t[i++] = (char)('0' + pct / 10 % 10);
        t[i++] = (char)('0' + pct % 10); t[i++] = '%'; t[i] = 0;
        say(t);
        v.pct_said = pct;
    }
}

int psmac_video_poll(void)
{
    if (!blk) return 0;
    uint64_t t0 = now();
    int fdid = psmac_vframes_poll();          /* QuickTime's pictures first: the 68k waits for them */
    psmac_video_test_poll(blk, ring);
    command();
    geometry();
    if (v.sound && blk->volume) psmac_audio_video_volume(blk->volume);
    int did = 0;
    if (v.on) {
        head_update();
        int moved = drain();                  /* not counted in did: the slices below go on all the same */
        if (!v.headers) {
            if (input_level() >= PREFILL || all_in() || (blk->eof && head_ok == blk->head)) {
                int r = open_headers();
                did = r != 0;
            }
        } else if (!v.primed) {               /* the start waits for mac.vprefill='s seconds (or all of a short stream) */
            if (ready(cfg.start_ms)) {
                v.primed = 1; v.t_primed = now(); v.primed_ms = held_ms(); v.primed_kb = held_bytes() >> 10;
                blk->buffer_target_ms = cfg.rebuf_ms;
                uint32_t w = ms_of(v.t_primed - v.t_start);
                kprintf("[VIDEO] stream %u starts after %u.%u s with %u.%u s held (%u KB at %u kbit/s%s)\n", v.streams, w / 1000,
                        w / 100 % 10, v.primed_ms / 1000, v.primed_ms / 100 % 10, v.primed_kb, byte_rate() * 8u / 1000u,
                        all_in() ? "; all of it" : input_full() ? "; the buffer full" : "");
                did = 1;
            }
        } else {
            rebuffered();
            if (v.starving && !v.stalled && psmac_audio_video_queued() < v.rate * SOUND_LOW_MS / 1000u) rebuffer();
            did |= sound_slice();
            if (!did) did |= video_slice();
            did |= show_slice();
            if (v.sound && v.ended_a && psmac_audio_video_queued() == 0 && !v.ended_v) clock_to_timer();
            if (v.ended_v && !v.pending && (!v.sound || (v.ended_a && psmac_audio_video_queued() == 0)) && !v.logged) {
                v.logged = 1;
                kprintf("[VIDEO] stream %u played to the end, %u.%u s: %u pictures decoded, %u shown, %u not drawn, %u times dry\n",
                        v.streams, blk->pos_ms / 1000, blk->pos_ms / 100 % 10, v.decoded, v.shown, v.dropped, v.underruns);
                buffer_report();
                uint32_t pos = blk->pos_ms;
                close_stream();
                blk->pos_ms = pos;
                say("ended");
                blk->state = A68V_S_ENDED;
            }
        }
        if (v.on) buffer_numbers();
        did |= moved;
    }
    if (did) st.busy += now() - t0;
    numbers();
    return did | fdid;
}

void psmac_video_reset(void) { __atomic_store_n(&reset_req, 1, __ATOMIC_RELEASE); psmac_vframes_reset(); }

void psmac_video_config(uint32_t buffer_bytes, uint32_t start_ms, uint32_t rebuffer_ms)
{
    if (fq.buf && buffer_bytes != cfg.fifo && !v.on) { tlsf_free(tlsf, fq.buf); fq.buf = 0; fq.size = 0; }
    cfg.fifo = buffer_bytes; cfg.start_ms = start_ms; cfg.rebuf_ms = rebuffer_ms;
}

void psmac_video_init(uint8_t *win, uint32_t screens)
{
    tables();
    fq_reset();
    blk = (volatile struct a68v_block *)(win + A68V_OFF_BLOCK);
    ring = win + A68V_OFF_RING;
    nscreens = screens;
    for (uint32_t i = 0; i < sizeof(struct a68v_block) / 4; i++) ((volatile uint32_t *)blk)[i] = 0;
    blk->win_size = A68V_WIN_SIZE; blk->ring_off = A68V_OFF_RING; blk->ring_size = A68V_RING_SIZE;
    blk->screens = screens; blk->volume = 0x01000100u;
    blk->buffer_size = fq.size; blk->buffer_target_ms = cfg.start_ms;
    head_seen = head_ok = 0; head_seen_t = now(); g.seq = 0;
    say("idle");
    st.t0 = now();
    psmac_vframes_init(win);
    DMB();
    blk->version = A68V_VERSION; blk->magic = A68V_MAGIC;
}

void psmac_video_report(void)
{
    static uint32_t last_state = ~0u, last_streams;
    psmac_vframes_report();
    if (!blk || !v.streams) return;
    static const char *const sn[] = { "idle", "buffering", "playing", "paused", "ended", "error" };
    uint32_t s = blk->state < 6 ? blk->state : 5;
    int quiet = s == A68V_S_IDLE || s == A68V_S_ENDED || s == A68V_S_ERROR;   /* said once, not every five seconds */
    if (quiet && s == last_state && v.streams == last_streams) return;
    last_state = s; last_streams = v.streams;
    kprintf("[PSMAC]   video: %s, stream %u %ux%u, %u.%u s, %u decoded, %u shown, %u not drawn, %u dry; decode %u us (at most %u), draw %u us, "
            "%u per mille of core 1; %u KB waiting (%u.%u s, %u%% of %u.%u s), rebuffered %u.%u s, screen %u at %d,%d-%d,%d%s, "
            "%s %u, %u under a menu, %u unread, sound %s\n",
            sn[s], v.streams, blk->video_w, blk->video_h, blk->pos_ms / 1000, blk->pos_ms / 100 % 10, blk->frames_decoded,
            blk->frames_shown, blk->frames_dropped, blk->underruns, blk->decode_us, blk->decode_us_max, blk->draw_us, blk->load,
            blk->buffered >> 10, blk->buffer_ms / 1000, blk->buffer_ms / 100 % 10, blk->buffer_pct, blk->buffer_target_ms / 1000,
            blk->buffer_target_ms / 100 % 10, blk->rebuffer_ms / 1000, blk->rebuffer_ms / 100 % 10, g.screen, g.l, g.t, g.r, g.b, g.visible ? "" : " (hidden)",
            g.rgn ? "region parts" : "no region", rgn_parts, menu_skips, rgn_skips, blk->sound ? "on" : "off");
}
