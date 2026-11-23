/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_audio.c — the Macintosh's sound on the Pi's own outputs: HDMI and the 3.5 mm jack.
 *
 * Backstory (20 November 2026, docs/08 item 74). The user's SE shows its second screen on an HDMI
 * monitor, which has loudspeakers, and asked for the Mac's sound there, in stereo. Two sources feed
 * it. The first is the ASC, the IIci's sound chip, which the kernel answers itself on every board
 * (psmac_board_se.c): what the Sound Manager writes into its two FIFOs (channel A left, channel B
 * right, 8 bits at 22,257 Hz) is copied into two rings here as it is written, on core 0; in the
 * chip's wavetable mode (the startup chime) the four voices are synthesised here from the chip's
 * registers, as MAME's asc.cpp does, mono on both sides. On the SE board the SE's own speaker
 * keeps playing the same samples (psmac_board_se.c, "the SE's speaker"). The second is the sound
 * card, a ring of 16-bit stereo at 44.1 kHz in slot 9's window (psmb.h, "the sound card"), which
 * the Mac's PiSound extension fills as a Sound Manager 3 output device: better than the ASC's 8
 * bits, and the only stereo most programs ever use.
 *
 * Core 1 does all of it. It started the USB controller and then had nothing to do but wait for the
 * next enumeration (psmac_usb.c); now it waits in this file's loop, which takes the two sources,
 * converts each to the output's rate with a polyphase windowed-sinc filter (psmac_audio_fir.h,
 * written by tools/mkfir.py: 32 taps, 128 phases), mixes them and puts the result where the output
 * reads it. The output is chosen with mac.audio=:
 *   hdmi (the default): the HDMI transmitter's audio, IEC 958 subframes into the MAI FIFO, fed by a
 *        DMA channel that loops over a ring of blocks. The registers and their order are those of
 *        Circle's HDMISoundBaseDevice (github.com/rsta2/circle, read, not copied) and Linux's
 *        vc4_hdmi audio: the MAI's sample-rate divider from the HSM clock, N and CTS for the
 *        sink's audio clock recovery from the pixel clock, the audio InfoFrame in the packet RAM.
 *        It needs the firmware to run HDMI as HDMI and not DVI (hdmi_drive=2 in config.txt); the
 *        transmitter says which (the packet RAM's enable bit) and the log says so.
 *   jack: the 3B's, 3B+'s and 3A+'s headphone socket, the two PWM channels on GPIO 40 and 41 at
 *        250 MHz, one range per sample (Circle's PWMSoundBaseDevice is the reference), by DMA too.
 *   off: nothing; the Mac's extension then leaves the Sound Manager on the ASC.
 * In the testbed QEMU has neither (its DMA model also ignores DREQ, so a looping DMA would never
 * end): mac.audiodump=PATH writes the output, 16-bit little-endian stereo at the output's rate,
 * into a host file through semihosting, timed by the Pi's timer, for tools/audiocheck.py to look at.
 *
 * Rates. The card's samples are taken at the output's pace and the Mac refills what was taken, so
 * the card needs only the fixed ratio 44100 : out. The ASC's are paced by the kernel's model of
 * the chip (asc_drain, the Pi's timer at 22,257 Hz) and played by the HDMI's or the PWM's own
 * clock: the two drift apart by some parts per million, so the ASC's ratio follows the ring's
 * level, a few hundredths of a per cent either way. A sound that starts after silence starts with
 * whatever the ring holds; the level the ratio aims at is the one it had then.
 *
 * Since 21 November 2026 there is a fourth source (docs/08 item 82): the card's stream channel, for
 * PiPlay (mac/piplay.c). The Mac hands over a sound file's bytes as they are, MPEG audio (MP3) or the
 * PCM of an AIFF or WAV file, through a byte ring in the window (psmb.h, struct psmb_stream); core 1
 * takes them into a buffer of its own of 2 MiB, decodes MPEG audio with minimp3 (psmac_mp3.c), one
 * frame at a time and only while fewer than a few frames are decoded ahead, converts PCM, and plays the
 * result through a resampler of its own beside the other three, so a beep still sounds over the music.
 *
 * Not verified on hardware (20 November 2026): the HDMI and PWM output and their DMA; everything
 * from the rings to the samples is, in QEMU, through mac.audiodump=. */
#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "spinlock.h"
#include "psmac.h"
#include "psmb.h"
#include "psmac_audio_fir.h"
#include "psmac_video.h"                       /* source 5's calls, below */
#include "third_party/minimp3/minimp3.h"              /* the declarations; psmac_mp3.c compiles the decoder */
#include "psmac_codec.h"                               /* FLAC, Ogg Vorbis, ALAC (psmac_flac.c, psmac_ogg.c, psmac_alac.c) */

extern const char *psmac_option(const char *key);                /* psmac_bus.c */
extern int64_t psmac_sh_open(const char *name, uint64_t mode);
extern int psmac_sh_write(int64_t fd, const void *buf, uint64_t len);
extern spinlock_t vc_mbox_lock;                                  /* support_rpi.c: one mailbox caller at a time */
extern uintptr_t mmu_virt2phys(uintptr_t addr);
extern spinlock_t psmac_gpfsel_lock;                             /* support_rpi.c */
extern int psmac_board_se, psmac_board_solo;
extern const volatile uint8_t *psmac_asc_regs(void);             /* psmac_board_se.c: the ASC's registers and wave RAM */
extern uint32_t psmac_asc_wt_gen(void);                          /* ... bumped when the Mac writes a voice's phase */

static inline uint64_t now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
_Static_assert(sizeof(struct psmb_stream) == 384 && __builtin_offsetof(struct psmb_stream, cmd_seq) == 0x80, "psmb_stream");

/* the peripherals through the kernel's own mapping (psmac.h), little-endian registers */
#define IO(a) ((volatile uint32_t *)(uintptr_t)(PSMAC_PERIPH_HI + ((a) - PSMAC_PERIPH_LOW)))
static inline uint32_t rd(uint32_t a) { uint32_t v = *IO(a); __asm__ volatile("dmb oshld" ::: "memory"); return __builtin_bswap32(v); }
static inline void wr(uint32_t a, uint32_t v) { __asm__ volatile("dmb oshst" ::: "memory"); *IO(a) = __builtin_bswap32(v); __asm__ volatile("dsb oshst" ::: "memory"); }
static void wait_us(uint32_t us) { uint64_t t = now(), n = (uint64_t)us * hz() / 1000000u; while (now() - t < n) __asm__ volatile("yield"); }

/* One firmware property, asked from core 1. Not through alice_net.c's mailbox call: that one yields to
   the WiFi module's coroutine while it waits, which is core 3's to switch, never core 1's. Its own buffer
   in the kernel's image (low memory, which the VideoCore reaches on every Pi), the request handed over
   through the uncached alias, the reply matched by its address, under the lock every caller holds. */
static uint32_t vcbuf[8] __attribute__((aligned(16)));
static int vc_tag(uint32_t tag, uint32_t *val)
{
    vcbuf[0] = LE32(32); vcbuf[1] = 0; vcbuf[2] = LE32(tag); vcbuf[3] = LE32(8); vcbuf[4] = 0; vcbuf[5] = 0; vcbuf[6] = 0; vcbuf[7] = 0;
    arm_flush_cache((uintptr_t)vcbuf, sizeof vcbuf);
    uint32_t addr = ((uint32_t)mmu_virt2phys((uintptr_t)vcbuf) & 0x3fffffffu) | 0xC0000000u;
    uint64_t t0 = now(), lim = hz();
    int ok = 0;
    spinlock_acquire(&vc_mbox_lock);
    while ((rd(0x3F00B898u) & (1u << 31)) && now() - t0 < lim) ;
    wr(0x3F00B8A0u, (addr & ~15u) | 8u);
    while (now() - t0 < lim) {
        if (!(rd(0x3F00B898u) & (1u << 30)) && rd(0x3F00B880u) == ((addr & ~15u) | 8u)) { ok = 1; break; }
    }
    spinlock_release(&vc_mbox_lock);
    arm_dcache_invalidate((uintptr_t)vcbuf, sizeof vcbuf);
    if (!ok || LE32(vcbuf[1]) != 0x80000000u || !(LE32(vcbuf[4]) & 0x80000000u)) return -1;
    *val = LE32(vcbuf[5]);
    return 0;
}

#define ASC_RATE   22257u
#define CARD_RATE  44100u

static volatile int started;               /* core 1's output is running (psmac_audio_start) */
/* ---- options ---------------------------------------------------------------------------------- */
enum { OUT_OFF, OUT_HDMI, OUT_JACK, OUT_FILE };
static int opt_read, out_want = OUT_HDMI, out_kind = OUT_OFF, card_on = 1;
static uint32_t out_rate = 48000, test_secs;
static int test_card;
static int str_spk;                           /* mac.sestream=on: the SE's speaker plays the stream too (below, mix_frame) */
static const char *dump_path;
static void options(void)
{
    if (opt_read) return;
    opt_read = 1;
    const char *v = psmac_option("mac.audio");
    if (v) out_want = v[0] == 'o' ? OUT_OFF : v[0] == 'j' ? OUT_JACK : OUT_HDMI;
    if ((v = psmac_option("mac.audiorate")) && v[0] == '4' && v[1] == '4') out_rate = 44100;
    if ((v = psmac_option("mac.audiodump")) && v[0]) { dump_path = v; out_want = OUT_FILE; }
    if ((v = psmac_option("mac.sndcard")) && v[0] == 'o' && v[1] == 'f') card_on = 0;
    if ((v = psmac_option("mac.sestream")) && v[0] == 'o' && v[1] == 'n') str_spk = 1;
    if ((v = psmac_option("mac.audiotest"))) {
        if (v[0] == 'c') test_card = 1;
        else { uint32_t n = 0; while (*v >= '0' && *v <= '9') n = n * 10 + (uint32_t)(*v++ - '0'); test_secs = n ? n : 3; }
    }
}
/* for psmac_board_se.c (the chime's full length) and the log: whether anything plays the Mac's sound here */
int psmac_audio_wanted(void) { options(); return out_want != OUT_OFF; }

/* ---- the ASC's FIFO bytes, copied as the Mac writes them (core 0) ----------------------------- */
#define ASC_RING 16384u                       /* 0.74 s a channel: room for core 1's second of USB start */
static uint8_t asc_ring[2][ASC_RING];
static uint32_t asc_head[2], asc_tail[2];
static volatile uint32_t asc_flush_gen;
static volatile uint64_t asc_last_byte;       /* when the Mac last wrote a FIFO byte */
static uint32_t asc_dropped;
void psmac_audio_asc_byte(int ch, uint8_t v)
{
    if (out_kind == OUT_OFF) return;
    uint32_t h = asc_head[ch];
    if (h - __atomic_load_n(&asc_tail[ch], __ATOMIC_ACQUIRE) >= ASC_RING) { asc_dropped++; return; }
    asc_ring[ch][h % ASC_RING] = v;
    __atomic_store_n(&asc_head[ch], h + 1, __ATOMIC_RELEASE);
    asc_last_byte = now();
}
void psmac_audio_asc_flush(void) { asc_flush_gen++; }

/* ---- the resampler ---------------------------------------------------------------------------
   One per source. hist holds the last FIR_TAPS input frames twice over, so that the window ending
   at the newest is always contiguous; pos is the next write. The output lies frac of an input
   sample after the window's middle (tools/mkfir.py); step is input samples per output sample in
   32.32 fixed point. */
struct rs {
    int16_t h[2][2 * FIR_TAPS];
    uint32_t pos, frac, idle;
    uint64_t step;
};
static void rs_push(struct rs *r, int32_t l, int32_t rt)
{
    r->h[0][r->pos] = r->h[0][r->pos + FIR_TAPS] = (int16_t)l;
    r->h[1][r->pos] = r->h[1][r->pos + FIR_TAPS] = (int16_t)rt;
    r->pos = (r->pos + 1) % FIR_TAPS;
    r->idle = (l || rt) ? 0 : r->idle + 1;
}
static void rs_out(const struct rs *r, int32_t *l, int32_t *rt)
{
    if (r->idle >= FIR_TAPS) { *l = *rt = 0; return; }   /* a window of silence: nothing to compute */
    const int16_t *c = fir[r->frac >> FIR_PHASE_SHIFT], *a = &r->h[0][r->pos], *b = &r->h[1][r->pos];
    int32_t sl = 0, sr = 0;
    for (int k = 0; k < FIR_TAPS; k++) { sl += a[k] * c[k]; sr += b[k] * c[k]; }
    *l = sl >> 15; *rt = sr >> 15;
}
static uint64_t ratio(uint32_t in, uint32_t out) { return ((uint64_t)in << 32) / out; }

/* ---- source 1: the ASC ------------------------------------------------------------------------ */
static struct rs asc_rs;
static struct {
    int playing;                              /* FIFO mode: taking from the rings */
    uint32_t flush_seen, wt_seen, phase[4], target;
    int32_t level_avg;                        /* the rings' level, smoothed (x 16) */
    uint32_t starts, samples, underruns, wt_samples;
} asc;
static int asc_fifo_level(void)
{
    int a = (int)(__atomic_load_n(&asc_head[0], __ATOMIC_ACQUIRE) - asc_tail[0]);
    int b = (int)(__atomic_load_n(&asc_head[1], __ATOMIC_ACQUIRE) - asc_tail[1]);
    return a > b ? a : b;
}
static uint32_t be24(const volatile uint8_t *r, uint32_t off) { return (uint32_t)r[off] << 16 | (uint32_t)r[off + 1] << 8 | r[off + 2]; }
static int asc_wavetable(const volatile uint8_t *r)
{
    if (r[0x801] != 2) return 0;
    for (int v = 0; v < 4; v++) if (be24(r, 0x815 + 8 * v)) return 1;
    return 0;
}
/* one input frame of the ASC at 22,257 Hz, as signed 16-bit, the volume of $806 applied */
static void asc_next(int32_t *l, int32_t *rt)
{
    const volatile uint8_t *r = psmac_asc_regs();
    int32_t gain = (int32_t)((r[0x806] >> 5) & 7) * 256 / 7;         /* the Sound control panel's volume, 0-7 */
    if (asc.flush_seen != asc_flush_gen) {
        asc.flush_seen = asc_flush_gen;
        for (int ch = 0; ch < 2; ch++) asc_tail[ch] = __atomic_load_n(&asc_head[ch], __ATOMIC_ACQUIRE);
        asc.playing = 0;
    }
    if (asc_wavetable(r)) {                                          /* the chime: four voices, as MAME's asc.cpp */
        uint32_t g = psmac_asc_wt_gen();
        if (asc.wt_seen != g) { asc.wt_seen = g; for (int v = 0; v < 4; v++) asc.phase[v] = be24(r, 0x811 + 8 * v); }
        int sum = 0;
        for (int v = 0; v < 4; v++) {
            asc.phase[v] = (asc.phase[v] + be24(r, 0x815 + 8 * v)) & 0xffffffu;
            sum += (int8_t)(r[(v >> 1) * 0x400 + (v & 1) * 0x200 + ((asc.phase[v] >> 15) & 0x1ff)] ^ 0x80);
        }
        asc.wt_samples++;
        *l = *rt = (sum * 64) * gain / 256;                          /* sum/4 << 8 */
        return;
    }
    if (!asc.playing) {
        /* a sound starts: with what the rings hold once the first burst is in (the Sound Manager
           fills the chip's FIFO to the brim at once), or when the Mac has stopped writing */
        int lv = asc_fifo_level();
        if (lv <= 0 || (lv < 512 && now() - asc_last_byte < hz() / 100)) { *l = *rt = 0; return; }
        asc.playing = 1; asc.starts++; asc.target = (uint32_t)lv; asc.level_avg = lv * 16;
    }
    int32_t s[2] = { 0, 0 }, got = 0;
    for (int ch = 0; ch < 2; ch++) {
        if ((int)(__atomic_load_n(&asc_head[ch], __ATOMIC_ACQUIRE) - asc_tail[ch]) > 0) {
            s[ch] = (int8_t)(asc_ring[ch][asc_tail[ch] % ASC_RING] ^ 0x80) * 256;
            __atomic_store_n(&asc_tail[ch], asc_tail[ch] + 1, __ATOMIC_RELEASE);
            got = 1;
        }
    }
    if (!got) {
        if (now() - asc_last_byte < hz() / 20) asc.underruns++;    /* a gap inside a sound, not its end */
        else asc.playing = 0;
        *l = *rt = 0;
        return;
    }
    asc.samples++;
    *l = s[0] * gain / 256; *rt = s[1] * gain / 256;
}
/* the ratio follows the rings' level: within a sound the chip's model and the output's clock drift
   apart, and a ring that slowly fills or empties would end in a gap or a dropped burst */
static void asc_steer(void)
{
    uint64_t base = ratio(ASC_RATE, out_rate);
    if (!asc.playing) { asc_rs.step = base; return; }
    asc.level_avg += asc_fifo_level() - asc.level_avg / 16;
    int32_t err = asc.level_avg / 16 - (int32_t)asc.target;          /* samples too many (+) or too few (-) */
    if (err > 400) err = 400;
    if (err < -400) err = -400;
    asc_rs.step = base + (int64_t)base * err / 400 / 400;            /* at most a quarter per cent */
}

/* ---- source 2: the sound card ----------------------------------------------------------------- */
static volatile struct psmb_sound *card;
static const uint8_t *card_ring;
static struct rs card_rs;
static uint32_t card_frames, card_vol = 0x01000100u;
static int card_took;                         /* card_next took a frame (not silence for an empty ring) */
static void card_next(int32_t *l, int32_t *rt)
{
    uint32_t t = card->tail;
    if ((int32_t)(card->head - t) <= 0) {
        if (card->state) card->underruns++;
        *l = *rt = 0;
        return;
    }
    const uint8_t *f = card_ring + (t % PSMB_SND_FRAMES) * 4;
    int32_t a = (int16_t)(f[0] << 8 | f[1]), b = (int16_t)(f[2] << 8 | f[3]);
    __asm__ volatile("dmb ish" ::: "memory");
    card->tail = t + 1;
    card_frames++;
    card_took = 1;
    *l = a * (int32_t)(card_vol >> 16) / 256; *rt = b * (int32_t)(card_vol & 0xffff) / 256;
}

/* the SE's speaker plays the card and the stream too: a mono feed at the ASC's rate, taken by
   psmac_board_se.c's speaker beside the ASC's own bytes. Fed at the output's rate from what the card
   and the stream sound (mix_frame), the frames between two speaker samples averaged, which is a
   speaker of 8 bits and two inches. Only while either really plays: silence would keep the speaker
   writing its buffer for ever (docs/08 item 74). */
#define SPK_RING 4096u
static uint8_t spk_ring[SPK_RING];
static uint32_t spk_head, spk_tail, spk_frac, spk_n;
static int32_t spk_sum;
static void spk_feed(int32_t l, int32_t rt)
{
    spk_sum += (l + rt) / 2;
    spk_n++;
    spk_frac += ASC_RATE;
    if (spk_frac < out_rate) return;
    spk_frac -= out_rate;
    int32_t v = spk_sum / (int32_t)spk_n;
    spk_sum = 0; spk_n = 0;
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    if (spk_head - __atomic_load_n(&spk_tail, __ATOMIC_ACQUIRE) >= SPK_RING) return;
    spk_ring[spk_head % SPK_RING] = (uint8_t)((v >> 8) + 128);
    __atomic_store_n(&spk_head, spk_head + 1, __ATOMIC_RELEASE);
}
int psmac_audio_spk_level(void) { return (int)(__atomic_load_n(&spk_head, __ATOMIC_ACQUIRE) - spk_tail); }
uint8_t psmac_audio_spk_take(void)
{
    uint8_t v = spk_ring[spk_tail % SPK_RING];
    __atomic_store_n(&spk_tail, spk_tail + 1, __ATOMIC_RELEASE);
    return v;
}

/* ---- source 3: the test (mac.audiotest=SECONDS): 440 Hz on the left, 660 Hz on the right, at the
   ASC's rate through the ASC's resampler, so that tools/audiocheck.py can measure the channels'
   separation and the filter. A sine from a 64-step quarter table, in 16 bits. */
static struct rs test_rs;
static uint32_t test_n, test_ph[2];
static int32_t sine(uint32_t ph)              /* ph: a full turn is 2^32 */
{
    static const int16_t q[65] = { 0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
        12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170,
        23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571,
        30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767 };
    uint32_t i = ph >> 24, quad = i >> 6, k = i & 63;
    int32_t v = (quad & 1) ? q[64 - k] : q[k];
    return (quad & 2) ? -v : v;
}
static void test_next(int32_t *l, int32_t *rt)
{
    if (test_n >= test_secs * ASC_RATE) { *l = *rt = 0; return; }
    test_n++;
    test_ph[0] += (uint32_t)(((uint64_t)440 << 32) / ASC_RATE);
    test_ph[1] += (uint32_t)(((uint64_t)660 << 32) / ASC_RATE);
    *l = sine(test_ph[0]) / 2; *rt = sine(test_ph[1]) / 2;
}

/* ---- source 4: the stream channel (PiPlay; psmb.h struct psmb_stream) --------------------------
   Everything here runs on core 1, in psmac_audio_poll (the commands, the bytes, the decoding) and in
   mix_frame (the samples), so the queues between them need no atomics. The Mac's half of the block
   is read with care: a command is taken once its cmd_seq moves, and the ring's head is used only
   once it has been seen for STR_HEAD_LAG: the 68k's stores come from the JIT on core 0 with no
   barrier between the bytes and the head, and an ARM core may make them visible out of order. */
#define STR_PIBUF    (2u << 20)               /* the Pi's own buffer: 87 s of 192 kbit/s MP3, 12 s of CD-quality PCM */
#define STR_WIN      16384u                   /* the decoder's window: minimp3 checks a frame against the next ones in it */
#define STR_PCM      8192u                    /* decoded frames waiting for the resampler */
#define STR_PREFILL  2304u                    /* frames decoded before a stream sounds (two MP3 frames) */
#define STR_NOAUDIO  (256u << 10)             /* bytes looked through for a first MPEG frame */
#define STR_HEAD_LAG 50u                      /* us: how long a new head must have been seen */
static volatile struct psmb_stream *str;
static const volatile uint8_t *str_ring;
static struct rs str_rs;
static uint8_t *pibuf;
static uint32_t pb_head, pb_tail;             /* count for ever; the buffer is a power of two */
static uint32_t head_seen, head_ok;           /* the Mac's head as last seen, and the one that may be used */
static uint64_t head_seen_t;
static uint8_t dwin[STR_WIN];
static uint32_t dwin_n;
static mp3dec_t mp3d;
static int16_t mp3_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int16_t spcm[STR_PCM][2];
static uint32_t spcm_head, spcm_tail;
/* FLAC, Ogg Vorbis and ALAC (22 November 2026, docs/08 item 105): psmac_codec.h's decoders, their memory from
   the kernel's TLSF (which has a lock, so core 1 may allocate while core 0's JIT does) */
static struct pcodec codec;
static int16_t codec_pcm[2 * 1152];
static void *codec_alloc(size_t n) { return tlsf_malloc(tlsf, n); }
static void codec_free(void *p) { tlsf_free(tlsf, p); }
static const struct pc_alloc codec_mem = { codec_alloc, codec_free };
static int is_codec(uint32_t f) { return f == PSMB_STR_FLAC || f == PSMB_STR_OGG || f == PSMB_STR_ALAC; }
static volatile uint32_t str_reset_req;      /* the 68k was reset (psmac_audio_mac_reset, another core) */
static uint64_t str_busy, str_busy_t0;       /* the decoder's time over the last second */
static uint32_t str_max_us, str_used;
static struct {
    int on, playing, paused, stopping, src_done, ended, logged;
    uint32_t format, rate, channels, bits, flags, fbytes;
    int32_t gain, gain_target;                /* 0-256: a fade of 256 input frames on pause, resume and stop */
    uint64_t frames;                          /* input frames played */
    uint32_t skip;                            /* frames still to drop at the start: the encoder's delay (LAME tag) */
    int first;                                /* the first MPEG frame is still to come (Xing/Info, LAME tag) */
    uint32_t xing_frames, delay, padding, spf;
    uint64_t kbps_sum;
    uint32_t mpeg_frames, skipped, underruns;
    uint64_t stop_t;
    uint32_t start_ms;                        /* the Mac's: the file's time at the first byte (a seek) */
} st;

static uint32_t str_level(void) { return spcm_head - spcm_tail; }
static void str_clear(void)                   /* everything about the last stream forgotten */
{
    pb_head = pb_tail = 0; dwin_n = 0; spcm_head = spcm_tail = 0;
    for (uint32_t i = 0; i < sizeof st; i++) ((uint8_t *)&st)[i] = 0;
    for (uint32_t i = 0; i < sizeof str_rs; i++) ((uint8_t *)&str_rs)[i] = 0;
    str_rs.idle = FIR_TAPS;
    mp3dec_init(&mp3d);
    pc_close(&codec);
    str->rate = str->channels = str->bitrate = str->layer = 0;
    str->pos_ms = str->length_ms = str->buffered = str->mpeg_frames = str->skipped = str->underruns = 0;
    str->error = 0;
}
static uint32_t str_ms(uint64_t frames) { return st.rate ? (uint32_t)(frames * 1000u / st.rate) : 0; }
/* where the stream is in its file: the decoder's own frame number when it knows it (FLAC frames, Ogg granules),
   else the time the Mac said it starts at, plus what has been played */
static uint32_t str_pos_ms(void)
{
    if (is_codec(st.format) && codec.base >= 0 && st.rate) return (uint32_t)(((uint64_t)codec.base + st.frames) * 1000u / st.rate);
    return st.start_ms + str_ms(st.frames);
}
static void str_fail(uint32_t why, const char *say)
{
    kprintf("[AUDIO] stream: %s\n", say);
    st.on = st.playing = 0;
    str->error = why;
    str->state = PSMB_STR_S_ERROR;
}
static void str_start(void)
{
    str_clear();
    if (!pibuf) pibuf = tlsf_malloc(tlsf, STR_PIBUF);
    if (!pibuf) { str_fail(PSMB_STR_E_NOMEM, "no memory for the buffer"); return; }
    st.format = str->format;
    st.gain = st.gain_target = 256;
    st.first = 1;
    st.start_ms = str->start_ms;
    str_used++;
    if (st.format == PSMB_STR_PCM) {
        st.rate = str->pcm_rate; st.channels = str->pcm_channels; st.bits = str->pcm_bits; st.flags = str->pcm_flags;
        if (st.rate < 4000 || st.rate > 192000 || st.channels < 1 || st.channels > 8 || (st.bits != 8 && st.bits != 16 && st.bits != 24)) {
            kprintf("[AUDIO] stream: PCM at %u Hz, %u channels, %u bits: ", st.rate, st.channels, st.bits);
            str_fail(PSMB_STR_E_FORMAT, "not a layout this card plays");
            return;
        }
        st.fbytes = st.channels * st.bits / 8;
        str_rs.step = ratio(st.rate, out_rate);
        str->rate = st.rate; str->channels = st.channels;
        if (str->data_bytes) str->length_ms = (uint32_t)((uint64_t)str->data_bytes / st.fbytes * 1000u / st.rate);
        kprintf("[AUDIO] stream: PCM, %u Hz, %u channel%s, %u bits%s%s, %u bytes (%u.%u s)\n", st.rate, st.channels, st.channels > 1 ? "s" : "",
                st.bits, st.bits > 8 ? (st.flags & PSMB_STR_PCM_LE ? " little-endian" : " big-endian") : (st.flags & PSMB_STR_PCM_UNSIGNED ? " unsigned" : " signed"),
                st.channels > 2 ? " (the first two played)" : "", str->data_bytes, str->length_ms / 1000, str->length_ms / 100 % 10);
    } else if (st.format == PSMB_STR_MPEG) {
        kprintf("[AUDIO] stream: MPEG audio, %u bytes%s\n", str->data_bytes, st.start_ms ? " (from a seek)" : "");
    } else if (is_codec(st.format)) {
        if (!pc_open(&codec, st.format, st.start_ms != 0, &codec_mem)) { str_fail(PSMB_STR_E_NOMEM, codec.error ? codec.error : "no memory for the decoder"); return; }
        kprintf("[AUDIO] stream: %s, %u bytes, from %u.%u s\n", st.format == PSMB_STR_FLAC ? "FLAC" : st.format == PSMB_STR_OGG ? "Ogg Vorbis" : "Apple Lossless",
                str->data_bytes, st.start_ms / 1000, st.start_ms / 100 % 10);
    } else {
        str_fail(PSMB_STR_E_FORMAT, "a format number this kernel does not know");
        return;
    }
    st.on = 1;
    str->state = PSMB_STR_S_PLAYING;
}
static void str_finish_stop(void)
{
    if (st.on && !st.ended) kprintf("[AUDIO] stream: stopped at %u.%u s\n", str_pos_ms() / 1000, str_pos_ms() / 100 % 10);
    uint32_t h = str->head;
    str_clear();
    str->tail = h; head_seen = head_ok = h; head_seen_t = now();
    str->state = PSMB_STR_S_IDLE;
}
static void str_command(void)
{
    if (__atomic_load_n(&str_reset_req, __ATOMIC_ACQUIRE)) {
        str_reset_req = 0;
        if (st.on) kprintf("[AUDIO] stream: the 68k was reset; the stream stopped\n");
        st.on = 0;
        str_finish_stop();
        str->done_seq = str->cmd_seq;
        return;
    }
    if (st.stopping) {                         /* a STOP waits for its fade, at most 50 ms */
        if (st.gain && now() - st.stop_t < hz() / 20) return;
        st.stopping = 0;
        str_finish_stop();
        str->done_seq = str->cmd_seq;
    }
    uint32_t seq = str->cmd_seq;
    if (seq == str->done_seq) return;
    __asm__ volatile("dmb ish" ::: "memory");
    switch (str->cmd) {
    case PSMB_STR_C_START: str_start(); break;
    case PSMB_STR_C_PAUSE:
        if (st.on && !st.ended) { st.paused = 1; st.gain_target = 0; str->state = PSMB_STR_S_PAUSED; }
        break;
    case PSMB_STR_C_RESUME:
        if (st.on && st.paused) { st.paused = 0; st.gain_target = 256; str->state = PSMB_STR_S_PLAYING; }
        break;
    case PSMB_STR_C_STOP:
        if (st.playing && st.gain) { st.stopping = 1; st.gain_target = 0; st.stop_t = now(); return; }
        str_finish_stop();
        break;
    }
    str->done_seq = seq;
}
/* the ring's bytes into the Pi's buffer, as far as it has room */
static void str_take(void)
{
    uint32_t h = str->head;
    uint64_t t0 = now(), lag = hz() / 1000000u * STR_HEAD_LAG;
    if (h != head_seen) {                     /* a new head: the one before is safe if it stood long enough */
        if (t0 - head_seen_t >= lag) head_ok = head_seen;
        head_seen = h; head_seen_t = t0;
    } else if (head_ok != h && t0 - head_seen_t >= lag) head_ok = h;
    if (!st.on) return;
    uint32_t t = str->tail, n = head_ok - t, room = STR_PIBUF - (pb_head - pb_tail);
    if ((int32_t)n <= 0) return;
    if (n > room) n = room;
    for (uint32_t k = 0; k < n; ) {
        uint32_t ro = (t + k) % PSMB_STR_SIZE, po = (pb_head + k) & (STR_PIBUF - 1);
        uint32_t c = n - k;
        if (c > PSMB_STR_SIZE - ro) c = PSMB_STR_SIZE - ro;
        if (c > STR_PIBUF - po) c = STR_PIBUF - po;
        memcpy(pibuf + po, (const void *)(str_ring + ro), c);
        k += c;
    }
    pb_head += n;
    __asm__ volatile("dmb ish" ::: "memory");
    str->tail = t + n;
}
static int str_all_in(void) { return str->eof && head_ok == str->head && str->tail == str->head; }
static void str_put(int32_t l, int32_t r)
{
    if (st.skip) { st.skip--; return; }
    int16_t *f = spcm[spcm_head % STR_PCM];
    f[0] = (int16_t)l; f[1] = (int16_t)r;
    spcm_head++;
}
static uint8_t pb_byte(uint32_t k) { return pibuf[(pb_tail + k) & (STR_PIBUF - 1)]; }
/* a frame's first bytes after the side information: "Xing" or "Info", the LAME tag's encoder delay */
static void mpeg_first(const uint8_t *h, int len, const mp3dec_frame_info_t *info)
{
    int mpeg1 = (h[1] & 0x08) != 0, mono = (h[3] >> 6) == 3;
    int x = 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17)) + ((h[1] & 1) ? 0 : 2);
    st.spf = info->layer == 1 ? 384 : (info->layer == 3 && !mpeg1) ? 576 : 1152;
    if (x + 16 > len || !((h[x] == 'X' && h[x + 1] == 'i' && h[x + 2] == 'n' && h[x + 3] == 'g') ||
                          (h[x] == 'I' && h[x + 1] == 'n' && h[x + 2] == 'f' && h[x + 3] == 'o'))) return;
    uint32_t flags = (uint32_t)h[x + 4] << 24 | (uint32_t)h[x + 5] << 16 | (uint32_t)h[x + 6] << 8 | h[x + 7];
    if (flags & 1) st.xing_frames = (uint32_t)h[x + 8] << 24 | (uint32_t)h[x + 9] << 16 | (uint32_t)h[x + 10] << 8 | h[x + 11];
    if (flags == 15 && x + 144 <= len) {      /* frames, bytes, TOC and quality all there: the LAME tag follows (LAME's and ffmpeg's) */
        st.delay = (uint32_t)h[x + 141] << 4 | h[x + 142] >> 4;
        st.padding = (uint32_t)(h[x + 142] & 15) << 8 | h[x + 143];
        if (st.delay > 3000) st.delay = st.padding = 0;     /* not a LAME tag after all */
    }
    st.first = -1;                            /* this frame is the tag: its samples are dropped */
}
/* one slice of work: one MPEG frame decoded, or up to 1152 PCM frames converted */
static void str_decode(void)
{
    if (!st.on || st.src_done || STR_PCM - str_level() < MINIMP3_MAX_SAMPLES_PER_FRAME / 2) return;
    int all = str_all_in();
    uint64_t t0 = now();
    if (st.format == PSMB_STR_PCM) {
        uint32_t n = (pb_head - pb_tail) / st.fbytes, room = STR_PCM - str_level(), bps = st.bits / 8;
        if (!n) { if (all) st.src_done = 1; return; }
        if (n > room) n = room;
        if (n > 1152) n = 1152;
        int le = (st.flags & PSMB_STR_PCM_LE) != 0, uns = (st.flags & PSMB_STR_PCM_UNSIGNED) != 0;
        for (uint32_t i = 0; i < n; i++) {
            int32_t v[2] = { 0, 0 };
            for (uint32_t c = 0; c < 2; c++) {
                uint32_t k = i * st.fbytes + (c < st.channels ? c : 0) * bps;
                if (bps == 1) v[c] = uns ? ((int32_t)pb_byte(k) - 128) * 256 : (int8_t)pb_byte(k) * 256;
                if (bps == 2) v[c] = (int16_t)(le ? pb_byte(k) | pb_byte(k + 1) << 8 : pb_byte(k) << 8 | pb_byte(k + 1));
                if (bps == 3) v[c] = (int16_t)(le ? pb_byte(k + 1) | pb_byte(k + 2) << 8 : pb_byte(k) << 8 | pb_byte(k + 1));
            }
            str_put(v[0], v[1]);
        }
        pb_tail += n * st.fbytes;
    } else if (is_codec(st.format)) {
        struct pc_src src = { pibuf, STR_PIBUF - 1, pb_head, pb_tail, all };
        uint32_t room = STR_PCM - str_level();
        int n = pc_decode(&codec, &src, codec_pcm, (int)(room < 1152 ? room : 1152));
        pb_tail = src.tail;
        if (codec.rate && !st.rate) {          /* the header read: the stream's rate and channels */
            if (codec.rate < 4000 || codec.rate > 192000) { str_fail(PSMB_STR_E_FORMAT, "a sample rate this card does not play"); return; }
            st.rate = codec.rate; st.channels = codec.channels; st.bits = codec.bits;
            str_rs.step = ratio(st.rate, out_rate);
            str->rate = st.rate; str->channels = st.channels;
            if (codec.total) str->length_ms = (uint32_t)(codec.total * 1000u / st.rate);
            kprintf("[AUDIO] stream: %u Hz, %u channel%s, %u bits%s; %u.%u s\n", st.rate, st.channels, st.channels > 1 ? "s" : "", st.bits,
                    st.channels > 2 ? " (the first two played)" : "", str->length_ms / 1000, str->length_ms / 100 % 10);
        }
        if (n == PC_ERROR) { str_fail(PSMB_STR_E_DECODE, codec.error ? codec.error : "the decoder failed"); return; }
        if (n == PC_END) st.src_done = 1;
        static uint32_t idle_more;                /* a decoder that only ever wants more once everything is in: the end */
        idle_more = n == PC_MORE && all && !(pb_head - pb_tail) ? idle_more + 1 : 0;
        if (idle_more > 64) st.src_done = 1;
        if (n > 0) {
            if (codec.base >= 0 && st.first && st.start_ms) {   /* a seek: where the decoder found itself */
                uint32_t at = (uint32_t)((uint64_t)codec.base * 1000u / st.rate);
                kprintf("[AUDIO] stream: the decoder starts at %u.%03u s (the Mac asked for %u.%03u)\n", at / 1000, at % 1000, st.start_ms / 1000, st.start_ms % 1000);
                st.first = 0;
            }
            for (int i = 0; i < n; i++)
                str_put(codec_pcm[2 * i], codec_pcm[2 * i + 1]);
        }
    } else {
        uint32_t want = STR_WIN - dwin_n, have = pb_head - pb_tail;
        if (want > have) want = have;
        for (uint32_t k = 0; k < want; k++)
            dwin[dwin_n + k] = pb_byte(k);
        pb_tail += want; dwin_n += want;
        if (dwin_n < STR_WIN && !all) return;  /* minimp3 wants the frames that follow to be sure of one */
        if (!dwin_n) { st.src_done = 1; return; }
        mp3dec_frame_info_t info;
        int ns = mp3dec_decode_frame(&mp3d, dwin, (int)dwin_n, mp3_pcm, &info);
        uint32_t used = info.frame_bytes > 0 ? (uint32_t)info.frame_bytes : 0;
        if (!used) {
            if (all) st.src_done = 1;         /* the end: what is left is no whole frame */
            else used = dwin_n > 4 ? dwin_n - 4 : 0;   /* cannot happen with a full window; move on, keeping a header's worth */
        }
        if (ns > 0) {
            if (info.frame_offset > 0) st.skipped += (uint32_t)info.frame_offset;
            if (st.first > 0) {
                mpeg_first(dwin + info.frame_offset, (int)(used - (uint32_t)info.frame_offset), &info);
                st.rate = (uint32_t)info.hz; st.channels = (uint32_t)info.channels;
                str_rs.step = ratio(st.rate, out_rate);
                str->rate = st.rate; str->channels = st.channels; str->layer = (uint32_t)info.layer;
                if (st.delay) st.skip = st.delay + 529;     /* the encoder's delay and the decoder's own (529 samples) */
                uint64_t total = st.xing_frames ? (uint64_t)st.xing_frames * st.spf : 0;
                if (total > st.delay + st.padding) total -= st.delay + st.padding;
                if (total) str->length_ms = (uint32_t)(total * 1000u / st.rate);
                else if (str->data_bytes && info.bitrate_kbps) str->length_ms = (uint32_t)((uint64_t)str->data_bytes * 8u / (uint32_t)info.bitrate_kbps);
                kprintf("[AUDIO] stream: MPEG-%s layer %s, %u Hz %s, %u kbit/s; %u.%u s%s%s",
                        (dwin[info.frame_offset + 1] & 0x18) == 0x18 ? "1" : (dwin[info.frame_offset + 1] & 0x18) == 0x10 ? "2" : "2.5",
                        info.layer == 3 ? "III" : info.layer == 2 ? "II" : "I", st.rate, st.channels == 2 ? "stereo" : "mono",
                        (uint32_t)info.bitrate_kbps, str->length_ms / 1000, str->length_ms / 100 % 10,
                        st.xing_frames ? " (the Xing/Info frame's count)" : str->length_ms ? " (from the bit rate)" : " (length not known)",
                        st.delay ? "" : "\n");
                if (st.delay) kprintf("; the LAME tag's encoder delay %u and padding %u samples dropped\n", st.delay, st.padding);
                if (st.first > 0) st.first = 0;
            }
            if (st.first < 0) st.first = 0;    /* the Xing/Info frame: no audio */
            else {
                st.mpeg_frames++;
                st.kbps_sum += (uint32_t)info.bitrate_kbps;
                for (int i = 0; i < ns; i++) {
                    if (info.channels == 2) str_put(mp3_pcm[2 * i], mp3_pcm[2 * i + 1]);
                    else str_put(mp3_pcm[i], mp3_pcm[i]);
                }
            }
        } else if (used) {
            st.skipped += used;               /* a tag, or damage: no frame there */
            if (!st.mpeg_frames && st.skipped > STR_NOAUDIO) { str_fail(PSMB_STR_E_NOAUDIO, "no MPEG audio frame in the first 256 KB"); return; }
        }
        if (used > dwin_n) used = dwin_n;
        memmove(dwin, dwin + used, dwin_n - used);
        dwin_n -= used;
    }
    uint64_t d = now() - t0;
    str_busy += d;
    uint32_t us = (uint32_t)(d * 1000000u / hz());
    if (us > str_max_us) str_max_us = us;
}
/* enough decoded and buffered to start, or to go on after an underrun: two MPEG frames decoded and, behind
   them, half a second of PCM or 48 KB of MPEG in the Pi's buffer (two seconds at 192 kbit/s), unless the
   Mac has sent everything. The first testbed run started CD-quality PCM with 9 KB in hand, and the Mac,
   reading a disk image through semihosting, then fell behind by a fifth of the time in short gaps. */
static int str_ready(void)
{
    if (st.src_done) return str_level() > 0;
    if (str_level() < STR_PREFILL) return 0;
    uint32_t have = pb_head - pb_tail + dwin_n, want = st.format == PSMB_STR_PCM ? st.rate * st.fbytes / 2 :
                    st.format == PSMB_STR_FLAC || st.format == PSMB_STR_ALAC ? 256u << 10 : 48u << 10;   /* lossless: about two seconds */
    return have >= want || str_all_in();
}
/* one input frame of the stream, at its own rate */
static void str_next(int32_t *l, int32_t *rt)
{
    *l = *rt = 0;
    if (!st.on || !st.playing) {
        if (st.on && st.frames && !st.paused && !st.src_done && !st.ended) st.underruns++;    /* a gap after the start: an underrun */
        return;
    }
    if (st.gain != st.gain_target) st.gain += st.gain < st.gain_target ? 1 : -1;
    if (!st.gain && (st.paused || st.stopping)) return;
    if (!str_level()) {
        if (st.src_done) { st.ended = 1; st.playing = 0; }
        else { st.underruns++; st.playing = 0; }       /* starved: wait for another prefill rather than stutter */
        return;
    }
    const int16_t *f = spcm[spcm_tail % STR_PCM];
    spcm_tail++;
    st.frames++;
    uint32_t vol = str->volume;
    *l = (int32_t)f[0] * (int32_t)(vol >> 16) / 256 * st.gain / 256;
    *rt = (int32_t)f[1] * (int32_t)(vol & 0xffff) / 256 * st.gain / 256;
}
/* the stream's part of a poll: commands, bytes, one slice of decoding, the block's numbers */
static void str_poll(void)
{
    static uint32_t title_seen;
    if (!str) return;
    str_command();
    if (str->title_seq != title_seen) {       /* "now playing", as the Mac says it; only plain ASCII goes to the log */
        char t[129];
        title_seen = str->title_seq;
        __asm__ volatile("dmb ish" ::: "memory");
        for (int i = 0; i < 128; i++) {
            char c = str->title[i];
            t[i] = c;
            if (!c) break;
            if ((uint8_t)c < 32 || (uint8_t)c > 126) t[i] = '?';
        }
        t[128] = 0;
        kprintf("[AUDIO] stream: now playing \"%s\"\n", t);
    }
    str_take();
    str_decode();
    if (st.on && !st.playing && !st.ended && !st.paused && str_ready()) st.playing = 1;
    if (st.on && st.src_done && !str_level() && !st.playing) st.ended = 1;
    if (st.on && st.ended && !st.logged) {
        st.logged = 1;
        uint32_t ms = str_pos_ms();
        kprintf("[AUDIO] stream: played to the end, %u.%u s (%llu frames at %u Hz); %u MPEG frames, %u bytes skipped, %u underruns; "
                "the decoder at most %u us a slice, %u per mille of core 1 over the last second\n",
                ms / 1000, ms / 100 % 10, (unsigned long long)st.frames, st.rate, st.mpeg_frames, st.skipped, st.underruns, str_max_us, str->load);
        str->state = PSMB_STR_S_ENDED;
    }
    uint64_t t = now();
    if (t - str_busy_t0 >= hz()) {
        str->load = (uint32_t)(str_busy * 1000u / (t - str_busy_t0));
        str_busy = 0; str_busy_t0 = t;
    }
    str->pos_ms = str_pos_ms();
    str->buffered = pb_head - pb_tail + dwin_n;
    str->mpeg_frames = st.mpeg_frames;
    str->skipped = st.skipped;
    str->underruns = st.underruns;
    str->bitrate = st.mpeg_frames ? (uint32_t)(st.kbps_sum / st.mpeg_frames) : 0;
}
/* ---- source 5: the video card's sound (psmac_video.c, docs/16, 22 November 2026) -----------------
   The video card decodes its stream's MP2 on this same core and hands the samples over here, 16-bit
   stereo at the stream's rate (44.1 kHz from the helper), through a resampler of their own beside the
   other four. The card's clock is the number of frames played from this queue (psmac_video.c, "Timing"),
   so a pause stops taking from it rather than fading. Everything runs on core 1: no atomics. */
#define VID_PCM 32768u                        /* 0.74 s at 44.1 kHz; the card keeps 0.4 s ahead */
static int16_t vpcm[VID_PCM][2];
static uint32_t vpcm_head, vpcm_tail, vid_underruns;
static struct rs vid_rs;
static int vid_on, vid_paused;
static uint32_t vid_vol = 0x01000100u;
static uint64_t vid_played;
int psmac_audio_video_open(uint32_t rate)
{
    options();
    if (!__atomic_load_n(&started, __ATOMIC_ACQUIRE) || out_kind == OUT_OFF || rate < 8000 || rate > 48000) return 0;
    vpcm_head = vpcm_tail = 0; vid_played = 0; vid_paused = 0; vid_underruns = 0;
    for (uint32_t i = 0; i < sizeof vid_rs; i++) ((uint8_t *)&vid_rs)[i] = 0;
    vid_rs.idle = FIR_TAPS; vid_rs.step = ratio(rate, out_rate);
    vid_on = 1;
    return 1;
}
uint32_t psmac_audio_video_room(void) { return VID_PCM - (vpcm_head - vpcm_tail); }
uint32_t psmac_audio_video_queued(void) { return vid_on ? vpcm_head - vpcm_tail : 0; }
void psmac_audio_video_push(const int16_t *lr, uint32_t frames)
{
    for (uint32_t i = 0; i < frames && vpcm_head - vpcm_tail < VID_PCM; i++) {
        int16_t *f = vpcm[vpcm_head % VID_PCM];
        f[0] = lr[2 * i]; f[1] = lr[2 * i + 1];
        vpcm_head++;
    }
}
uint64_t psmac_audio_video_played(void) { return vid_played; }
void psmac_audio_video_pause(int on) { vid_paused = on; }
void psmac_audio_video_volume(uint32_t lr) { vid_vol = lr; }
void psmac_audio_video_close(void) { vid_on = 0; vpcm_head = vpcm_tail = 0; }
static void vid_next(int32_t *l, int32_t *rt)
{
    *l = *rt = 0;
    if (!vid_on || vid_paused) return;
    if (vpcm_head == vpcm_tail) { if (vid_played) vid_underruns++; return; }
    const int16_t *f = vpcm[vpcm_tail % VID_PCM];
    vpcm_tail++;
    vid_played++;
    *l = (int32_t)f[0] * (int32_t)(vid_vol >> 16) / 256;
    *rt = (int32_t)f[1] * (int32_t)(vid_vol & 0xffff) / 256;
}

#ifdef PSMAC_A2
/* ---- source 6: the guests' sound (psmac_a2.c: the Apple II's speaker, 23 November 2026) ----------------------
   Mono 16-bit samples at the guest's rate (44.1 kHz), made by the guest on its own core (core 2 on the solo board,
   core 1 elsewhere: then this same core) and handed over through a ring with one writer and one reader: the writer
   moves head, this core tail, both with release stores, so the ring needs no lock across the cores. Through a
   resampler of its own beside the other five; both channels. Only in a kernel with the Apple II card. */
#define GST_PCM 16384u                        /* 0.37 s at 44.1 kHz: the Apple's slices are 20 ms at most */
static int16_t gpcm[GST_PCM];
static uint32_t gst_head, gst_tail, gst_underruns;
static struct rs gst_rs;
static int gst_on;
int psmac_audio_guest_open(uint32_t rate)
{
    options();
    if (__atomic_load_n(&gst_on, __ATOMIC_ACQUIRE)) return 1;
    if (!__atomic_load_n(&started, __ATOMIC_ACQUIRE) || out_kind == OUT_OFF || rate < 8000 || rate > 48000) return 0;
    for (uint32_t i = 0; i < sizeof gst_rs; i++) ((uint8_t *)&gst_rs)[i] = 0;
    gst_rs.idle = FIR_TAPS; gst_rs.step = ratio(rate, out_rate);
    __atomic_store_n(&gst_on, 1, __ATOMIC_RELEASE);
    return 1;
}
uint32_t psmac_audio_guest_room(void)
{
    if (!__atomic_load_n(&gst_on, __ATOMIC_ACQUIRE)) return 0;
    return GST_PCM - (__atomic_load_n(&gst_head, __ATOMIC_RELAXED) - __atomic_load_n(&gst_tail, __ATOMIC_ACQUIRE));
}
void psmac_audio_guest_push(const int16_t *s, uint32_t n)
{
    uint32_t h = __atomic_load_n(&gst_head, __ATOMIC_RELAXED), t = __atomic_load_n(&gst_tail, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && h - t < GST_PCM; i++) gpcm[h++ % GST_PCM] = s[i];
    __atomic_store_n(&gst_head, h, __ATOMIC_RELEASE);
}
static void gst_next(int32_t *l, int32_t *rt)
{
    uint32_t t = __atomic_load_n(&gst_tail, __ATOMIC_RELAXED), h = __atomic_load_n(&gst_head, __ATOMIC_ACQUIRE);
    if (t == h) { *l = *rt = 0; gst_underruns++; return; }
    *l = *rt = gpcm[t % GST_PCM];
    __atomic_store_n(&gst_tail, t + 1, __ATOMIC_RELEASE);
}
#endif

/* psmac_bus.c's 68k RESET (through psmac_card_reset, another core): the stream stops at core 1's next poll */
void psmac_audio_mac_reset(void) { __atomic_store_n(&str_reset_req, 1, __ATOMIC_RELEASE); }

/* one output frame: each source stepped through its resampler, then mixed and clipped */
static uint32_t clips;
static void rs_step(struct rs *r, void (*next)(int32_t *, int32_t *), int32_t *l, int32_t *rt)
{
    uint64_t a = (uint64_t)r->frac + r->step;
    for (uint32_t n = (uint32_t)(a >> 32); n; n--) {
        int32_t x, y;
        next(&x, &y);
        rs_push(r, x, y);
    }
    r->frac = (uint32_t)a;
    rs_out(r, l, rt);
}
static void mix_frame(int16_t *lo, int16_t *ro)
{
    int32_t l = 0, r = 0, x, y;
    int32_t sl = 0, sr = 0;                                          /* what the card and the stream sound, for the SE's speaker */
    static uint32_t card_recent;                                     /* output frames since the card's ring last gave a frame */
    rs_step(&asc_rs, asc_next, &x, &y); l += x; r += y;
    card_took = 0;
    if (card) { rs_step(&card_rs, card_next, &x, &y); l += x; r += y; sl += x; sr += y; }
    if (card_took) card_recent = 0; else if (card_recent < 2 * FIR_TAPS) card_recent++;
    if (str) { rs_step(&str_rs, str_next, &x, &y); l += x; r += y; if (str_spk) { sl += x; sr += y; } }
    if (vid_on) { rs_step(&vid_rs, vid_next, &x, &y); l += x; r += y; if (str_spk) { sl += x; sr += y; } }   /* the video card's sound */
#ifdef PSMAC_A2
    if (gst_on) { rs_step(&gst_rs, gst_next, &x, &y); l += x; r += y; }    /* the Apple II's speaker (not the SE's own speaker yet) */
#endif
    /* The stream reaches the SE's speaker only with mac.sestream=on: in MAME's SE (21 November 2026,
       docs/08 item 82) a System beep while a stream fed the speaker left the Mac hung in the beep, the
       beep's sound going round in the card's ring; the same beep with the speaker kept for the card
       alone, or with mac.sesound=off, ended as it should. Not understood yet; the music plays on HDMI
       or the jack either way. */
    if (psmac_board_se && !psmac_board_solo && (card_recent < 2 * FIR_TAPS || (str_spk && st.playing && st.gain) || (str_spk && vid_on && !vid_paused))) spk_feed(sl, sr);
    if (test_secs && test_n < test_secs * ASC_RATE + FIR_TAPS) { rs_step(&test_rs, test_next, &x, &y); l += x; r += y; }
    if (l > 32767) { l = 32767; clips++; } else if (l < -32768) { l = -32768; clips++; }
    if (r > 32767) { r = 32767; clips++; } else if (r < -32768) { r = -32768; clips++; }
    *lo = (int16_t)l; *ro = (int16_t)r;
}

/* ---- the DMA ring both hardware outputs use -------------------------------------------------
   NBLK blocks of BLK frames, two 32-bit words a frame, a control block each, the last linked to the
   first: the channel loops for ever, paced by its peripheral's DREQ. The control block it is on says
   which block it plays; core 1 fills every block behind it. Memory from the kernel's heap, which is
   in the first gigabyte the DMA reaches, through the VideoCore's uncached alias. */
#define DMA_BASE    0x3F007000u
#define DMA_ENABLE  0x3F007FF0u
#define DMA_CS(c)   (DMA_BASE + (c) * 0x100u)
#define DMA_CONBLK(c) (DMA_BASE + (c) * 0x100u + 0x04u)
#define DMA_DEBUG(c) (DMA_BASE + (c) * 0x100u + 0x20u)
#define NBLK 6u
#define BLK  384u                                  /* two IEC 958 blocks of 192 frames: 8 ms at 48 kHz */
struct cb { uint32_t ti, src, dst, len, stride, next, pad[2]; };
static struct cb *cbs;
static uint32_t *blk[NBLK];
static uint32_t cb_bus[NBLK];
static int dma_ch = -1;
static uint32_t fill_next, blocks_filled, dma_errors, late_fills;
static uint32_t bus(const void *p) { return ((uint32_t)mmu_virt2phys((uintptr_t)p) & 0x3fffffffu) | 0xC0000000u; }
static int dma_setup(uint32_t dreq, uint32_t dest_bus)
{
    uint32_t m = 0;
    uint32_t mask = vc_tag(0x00060001u, &m) ? 0x7f35u : m & 0x7fffu;   /* GET_DMA_CHANNELS; the Pi 3 firmware's usual answer, if it gave none */
    for (int c = 14; c >= 0; c--) if (mask & (1u << c)) { dma_ch = c; break; }
    if (dma_ch < 0) { kprintf("[AUDIO] no DMA channel left for the ARM (firmware mask %04x)\n", mask); return 0; }
    cbs = tlsf_malloc_aligned(tlsf, NBLK * sizeof(struct cb), 64);
    for (uint32_t i = 0; i < NBLK; i++) blk[i] = tlsf_malloc_aligned(tlsf, BLK * 8, 64);
    if (!cbs || !blk[NBLK - 1]) { kprintf("[AUDIO] no memory for the DMA ring\n"); return 0; }
    for (uint32_t i = 0; i < NBLK; i++) { for (uint32_t k = 0; k < BLK * 2; k++) blk[i][k] = 0; cb_bus[i] = bus(&cbs[i]); }
    for (uint32_t i = 0; i < NBLK; i++) {
        cbs[i].ti = LE32(dreq << 16 | 1u << 8 /* SRC_INC */ | 1u << 6 /* DEST_DREQ */ | 1u << 3 /* WAIT_RESP */);
        cbs[i].src = LE32(bus(blk[i])); cbs[i].dst = LE32(dest_bus); cbs[i].len = LE32(BLK * 8);
        cbs[i].stride = 0; cbs[i].next = LE32(cb_bus[(i + 1) % NBLK]); cbs[i].pad[0] = cbs[i].pad[1] = 0;
        arm_flush_cache((uintptr_t)blk[i], BLK * 8);
    }
    arm_flush_cache((uintptr_t)cbs, NBLK * sizeof(struct cb));
    wr(DMA_ENABLE, rd(DMA_ENABLE) | 1u << dma_ch);
    wr(DMA_CS(dma_ch), 1u << 31);                                  /* reset */
    wait_us(10);
    fill_next = 1;                                                 /* block 0 plays first, silent */
    return 1;
}
static void dma_start(void)
{
    wr(DMA_CONBLK(dma_ch), cb_bus[0]);
    wr(DMA_CS(dma_ch), 1u << 28 /* WAIT_FOR_OUTSTANDING_WRITES */ | 15u << 20 /* panic priority */ | 8u << 16 /* priority */ | 1u /* ACTIVE */);
}
static int dma_playing(void)                       /* the block the channel is on */
{
    uint32_t a = rd(DMA_CONBLK(dma_ch));
    for (uint32_t i = 0; i < NBLK; i++) if (a == cb_bus[i]) return (int)i;
    return -1;
}

/* ---- output 1: HDMI ---------------------------------------------------------------------------- */
#define HDMI      0x3F902000u                      /* the HDMI core's registers */
#define HD        0x3F808000u                      /* the HD block: the MAI */
#define MAI_CHMAP (HDMI + 0x090u)
#define MAI_CFG   (HDMI + 0x094u)
#define AUD_PKT   (HDMI + 0x09Cu)
#define RAM_PKT   (HDMI + 0x0A0u)
#define RAM_STAT  (HDMI + 0x0A4u)
#define CRP_CFG   (HDMI + 0x0A8u)
#define CTS0      (HDMI + 0x0ACu)
#define CTS1      (HDMI + 0x0B0u)
#define PHY_CTL0  (HDMI + 0x2C4u)
#define PKT_AUDIO (HDMI + 0x490u)                  /* packet RAM, packet 4: the audio InfoFrame */
#define MAI_CTL   (HD + 0x14u)
#define MAI_THR   (HD + 0x18u)
#define MAI_FMT   (HD + 0x1Cu)
#define MAI_DATA  (HD + 0x20u)
#define MAI_SMP   (HD + 0x2Cu)
#define CM        0x3F101000u
#define DREQ_HDMI 17u
#define DREQ_PWM  5u
static uint32_t iec_frame;                         /* the frame's place in its 192-frame IEC 958 block */
static uint8_t iec_status[5];                      /* the channel status bits, one a frame (IEC 60958-3, consumer) */
static uint32_t hsm_rate, pix_rate, hdmi_n, hdmi_cts;
static uint32_t hsm_clock(void)                    /* CM_HSMDIV: PLLD (500 MHz) over a 4.8 fixed-point divisor */
{
    uint32_t d = (rd(CM + 0x08Cu) >> 4) & 0xfffu;
    return d ? (uint32_t)((500000000ull << 8) / d) : 0;
}
static uint32_t pixel_clock(void)                  /* PLLH from the 19.2 MHz crystal, over its fixed 10 for the pixel clock */
{
    uint32_t ctrl = rd(CM + 0x1960u), frac = rd(CM + 0x1A60u) & 0xfffffu;
    uint32_t ndiv = ctrl & 0x3ffu, pdiv = (ctrl >> 12) & 7u;
    if (rd(CM + 0x1074u) & (1u << 11)) { ndiv *= 2; frac *= 2; }
    if (!pdiv) return 0;
    return (uint32_t)((19200000ull * (((uint64_t)ndiv << 20) + frac) / pdiv >> 20) / 10u);
}
/* n / m nearest to a / b with n < 2^24 and m <= 256: the MAI's divider of the HSM clock (continued fractions) */
static void best_ratio(uint64_t a, uint64_t b, uint32_t *n_out, uint32_t *m_out)
{
    uint64_t n0 = 0, n1 = 1, d0 = 1, d1 = 0, x = a, y = b;
    while (y) {
        uint64_t q = x / y, t = x % y, n2 = n0 + q * n1, d2 = d0 + q * d1;
        if (n2 > 0xffffffu || d2 > 256u) break;
        n0 = n1; n1 = n2; d0 = d1; d1 = d2; x = y; y = t;
    }
    *n_out = (uint32_t)n1; *m_out = (uint32_t)(d1 ? d1 : 1);
}
static int wait_bit(uint32_t reg, uint32_t bit, int set)
{
    for (int i = 0; i < 100; i++) { if (!!(rd(reg) & bit) == set) return 1; wait_us(1000); }
    return 0;
}
static int hdmi_start(void)
{
    if (!(rd(RAM_PKT) & (1u << 16))) {
        kprintf("[AUDIO] HDMI: the transmitter sends no packets (packet RAM off): the firmware runs the monitor as DVI, which has no sound. "
                "hdmi_drive=2 in config.txt makes it HDMI; a monitor without loudspeakers stays silent either way\n");
        return 0;
    }
    hsm_rate = hsm_clock(); pix_rate = pixel_clock();
    if (!hsm_rate || !pix_rate) { kprintf("[AUDIO] HDMI: no HSM clock (%u) or pixel clock (%u) to time the samples by\n", hsm_rate, pix_rate); return 0; }
    uint32_t n, m;
    best_ratio(hsm_rate, out_rate, &n, &m);
    hdmi_n = out_rate * 128u / 1000u;                               /* N, and CTS so that 128 fs = pixel x N / CTS */
    hdmi_cts = (uint32_t)((uint64_t)pix_rate * hdmi_n / (out_rate * 128u));
    static const uint32_t rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 0 };
    uint32_t code = 0;
    for (uint32_t i = 0; rates[i]; i++) if (rates[i] == out_rate) code = 1 + i;
    /* the channel status: consumer, PCM, copying allowed; the rate; 24-bit words (the 16-bit samples
       sit in the top 16 of them) */
    iec_status[0] = 0x04; iec_status[1] = 0; iec_status[2] = 0;
    iec_status[3] = out_rate == 44100 ? 0x00 : 0x02;
    iec_status[4] = (uint8_t)(0x0b | (out_rate == 44100 ? 15 : 13) << 4);
    if (!dma_setup(DREQ_HDMI, 0x7E000000u + (MAI_DATA & 0xffffffu))) return 0;
    wr(MAI_CTL, 1u << 0 | 1u << 9 | 1u << 15 | 1u << 2 | 1u << 1);  /* reset, flush, delayed; clear the empty and full errors */
    wr(MAI_SMP, n << 8 | (m - 1));
    wr(MAI_FMT, code << 8 | 2u << 16);                              /* the rate's code, PCM */
    wr(MAI_THR, 16u << 24 | 16u << 16 | 16u << 8 | 16u);            /* panic and DREQ thresholds */
    wr(MAI_CFG, 1u << 27 | 1u << 26 | 3u);                          /* format and bit order reversed, channels 0 and 1 */
    wr(MAI_CHMAP, 0x08u);
    wr(AUD_PKT, 1u << 29 | 1u << 24 | 0x0fu << 10 | 3u);            /* zero data when flat or inactive; B frames marked 0xF; two channels */
    wr(CRP_CFG, 1u << 24 | hdmi_n);                                 /* N, CTS from the registers below */
    wr(CTS0, hdmi_cts); wr(CTS1, hdmi_cts);
    /* the audio InfoFrame: type $84, version 1, length 10, two channels, the rest "refer to the stream" */
    wr(RAM_PKT, rd(RAM_PKT) & ~(1u << 4));
    int ok = wait_bit(RAM_STAT, 1u << 4, 0);
    wr(PKT_AUDIO, 0x0A0184u); wr(PKT_AUDIO + 4, 0x0170u);
    for (uint32_t r = PKT_AUDIO + 8; r <= PKT_AUDIO + 0x20; r += 4) wr(r, 0);
    wr(RAM_PKT, rd(RAM_PKT) | 1u << 4);
    ok = wait_bit(RAM_STAT, 1u << 4, 1) && ok;
    dma_start();
    wr(PHY_CTL0, rd(PHY_CTL0) & ~(1u << 25));                       /* the PHY's random generator on (Circle: RNG power-down off) */
    wr(MAI_CTL, 2u << 4 | 1u << 12 | 1u << 13 | 1u << 3);           /* two channels, whole samples, aligned, enabled */
    kprintf("[AUDIO] HDMI: %u Hz stereo through DMA channel %d; HSM clock %u Hz (MAI divider %u/%u), pixel clock %u Hz, N %u CTS %u%s\n",
            out_rate, dma_ch, hsm_rate, n, m, pix_rate, hdmi_n, hdmi_cts, ok ? "" : "; the InfoFrame was not taken (packet RAM did not answer)");
    return 1;
}
static uint32_t parity(uint32_t v) { v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return v & 1; }
static uint32_t iec_word(int16_t s, uint32_t frame)
{
    uint32_t w = ((uint32_t)(uint16_t)s << 8 & 0xffffffu) << 4;      /* the sample in bits 4-27 as a 24-bit word */
    if (frame < 40 && (iec_status[frame >> 3] >> (frame & 7) & 1)) w |= 1u << 30;
    if (parity(w)) w |= 1u << 31;
    if (frame == 0) w |= 0x0fu;                                     /* the block's first frame (AUD_PKT's B-frame mark) */
    return w;
}

/* ---- output 2: the jack (PWM) ------------------------------------------------------------------ */
#define PWM       0x3F20C000u
#define CM_PWMCTL (CM + 0x0A0u)
#define CM_PWMDIV (CM + 0x0A4u)
#define CM_PASSWD 0x5A000000u
static uint32_t pwm_range;
static int jack_start(void)
{
    uint32_t rev = 0;
    int t = vc_tag(0x00010002u, &rev) || !(rev & 0x800000u) ? -1 : (int)((rev >> 4) & 0xffu);   /* GET_BOARD_REVISION: the type is bits 4-11 */
    if (t != 0x08 && t != 0x0d && t != 0x0e) {                       /* 3B, 3B+, 3A+: the boards with a jack on GPIO 40 and 41 */
        kprintf("[AUDIO] jack: this Pi (board type %02x) has no headphone socket on GPIO 40/41 (the Zero 2 W's GPIO 41 is its WiFi's power)\n", t);
        return 0;
    }
    pwm_range = (250000000u + out_rate / 2) / out_rate;
    /* the PWM clock: PLLD's 500 MHz over 2 */
    wr(CM_PWMCTL, CM_PASSWD | (rd(CM_PWMCTL) & 0xffu & ~0x10u));
    for (int i = 0; i < 1000 && (rd(CM_PWMCTL) & 0x80u); i++) wait_us(10);
    wr(CM_PWMDIV, CM_PASSWD | 2u << 12);
    wr(CM_PWMCTL, CM_PASSWD | 6u);
    wr(CM_PWMCTL, CM_PASSWD | 6u | 0x10u);
    wait_us(2000);
    spinlock_acquire(&psmac_gpfsel_lock);                          /* GPIO 40 and 41: ALT0, PWM0 and PWM1 */
    uint32_t f = rd(0x3F200010u);
    f = (f & ~0x3fu) | 4u | 4u << 3;
    wr(0x3F200010u, f);
    spinlock_release(&psmac_gpfsel_lock);
    wr(PWM + 0x10u, pwm_range); wr(PWM + 0x20u, pwm_range);
    wr(PWM + 0x00u, 1u << 0 | 1u << 5 | 1u << 8 | 1u << 13 | 1u << 6);   /* both channels on, from the FIFO; the FIFO cleared */
    wait_us(2000);
    if (rd(PWM + 0x10u) != pwm_range) { kprintf("[AUDIO] jack: the PWM block does not answer (no such hardware here)\n"); return 0; }
    if (!dma_setup(DREQ_PWM, 0x7E000000u + ((PWM + 0x18u) & 0xffffffu))) return 0;
    wr(PWM + 0x08u, 1u << 31 | 7u << 8 | 7u);                       /* DMA on, panic and DREQ thresholds 7 */
    dma_start();
    kprintf("[AUDIO] jack: %u Hz stereo through DMA channel %d, PWM range %u at 250 MHz (GPIO 40 right, 41 left)\n", out_rate, dma_ch, pwm_range);
    return 1;
}

/* ---- output 3: a file in the testbed ------------------------------------------------------------ */
static int64_t dump_fd = -1;
static uint64_t dump_t0;
static uint32_t dump_blocks;
static int16_t dump_buf[BLK * 2];
static int file_start(void)
{
    extern int psmac_sh_available(void);
    if (!psmac_sh_available()) { kprintf("[AUDIO] mac.audiodump: no semihosting here\n"); return 0; }
    dump_fd = psmac_sh_open(dump_path, 5);                          /* wb */
    if (dump_fd < 0) { kprintf("[AUDIO] mac.audiodump: cannot open %s\n", dump_path); return 0; }
    dump_t0 = now();
    kprintf("[AUDIO] testbed: %u Hz stereo, 16-bit little-endian, into %s through semihosting\n", out_rate, dump_path);
    return 1;
}

/* a block of output frames, in the output's format */
static void fill(uint32_t *dst, int16_t *pcm)
{
    for (uint32_t i = 0; i < BLK; i++) {
        int16_t l, r;
        mix_frame(&l, &r);
        if (pcm) { pcm[2 * i] = (int16_t)__builtin_bswap16((uint16_t)l); pcm[2 * i + 1] = (int16_t)__builtin_bswap16((uint16_t)r); continue; }
        if (out_kind == OUT_HDMI) {
            dst[2 * i] = LE32(iec_word(l, iec_frame)); dst[2 * i + 1] = LE32(iec_word(r, iec_frame));
            if (++iec_frame == 192) iec_frame = 0;
        } else {                                                    /* the jack: GPIO 40 (PWM0) is the right side on a Pi 3 */
            dst[2 * i] = LE32((uint32_t)(((int32_t)r + 32768) * (int32_t)pwm_range >> 16));
            dst[2 * i + 1] = LE32((uint32_t)(((int32_t)l + 32768) * (int32_t)pwm_range >> 16));
        }
    }
    blocks_filled++;
}
static void silence_all(void)                      /* before core 1 goes away for a USB enumeration */
{
    if (dma_ch < 0) return;
    for (uint32_t i = 0; i < NBLK; i++) {
        for (uint32_t k = 0; k < BLK * 2; k++) blk[i][k] = out_kind == OUT_HDMI ? LE32(iec_word(0, (k / 2) % 192)) : LE32(pwm_range / 2);
        arm_flush_cache((uintptr_t)blk[i], BLK * 8);
    }
}

/* ---- core 1 --------------------------------------------------------------------------------------- */
void psmac_audio_start(uint8_t *win)
{
    options();
    card = (volatile struct psmb_sound *)(win + PSMB_OFF_SOUND);
    card_ring = win + PSMB_OFF_SNDRING;
    for (uint32_t i = 0; i < sizeof(struct psmb_sound) / 4; i++) ((volatile uint32_t *)card)[i] = 0;
    asc_rs.step = ratio(ASC_RATE, out_rate); test_rs.step = asc_rs.step; card_rs.step = ratio(CARD_RATE, out_rate);
    int ok = 0, quiet = 0;
    extern int psmac_sh_available(void);
    if ((out_want == OUT_HDMI || out_want == OUT_JACK) && psmac_sh_available()) {
        /* the testbed: QEMU's raspi3b has no HDMI audio and no PWM block (a write there stopped core 1
           for good), and its DMA model ignores DREQ, so a looping channel would never end */
        kprintf("[AUDIO] testbed (semihosting): no HDMI audio or jack here; mac.audiodump=PATH writes the output to a host file\n");
        out_want = OUT_OFF;
        quiet = 1;
    }
    if (out_want == OUT_HDMI) ok = hdmi_start();
    else if (out_want == OUT_JACK) ok = jack_start();
    else if (out_want == OUT_FILE) ok = file_start();
    out_kind = ok ? out_want : OUT_OFF;
    card->rate = CARD_RATE << 16; card->frames = PSMB_SND_FRAMES; card->out_rate = out_kind ? out_rate : 0;
    card->out_kind = card_on ? (uint32_t)out_kind : PSMB_SND_OUT_NONE; card->volume = card_vol; card->test = (uint32_t)test_card;
    __asm__ volatile("dmb ish" ::: "memory");
    card->version = 1; card->magic = PSMB_SND_MAGIC;
    if (!card_on) card = 0;
    /* the stream channel (PiPlay): there whenever something plays, with or without the card's ring */
    str = (volatile struct psmb_stream *)(win + PSMB_OFF_STREAM);
    str_ring = win + PSMB_OFF_STRRING;
    for (uint32_t i = 0; i < sizeof(struct psmb_stream) / 4; i++) ((volatile uint32_t *)str)[i] = 0;
    str->size = PSMB_STR_SIZE; str->data_off = PSMB_OFF_STRRING; str->pibuf_size = STR_PIBUF;
    str->out_kind = (uint32_t)out_kind; str->out_rate = out_kind ? out_rate : 0;
    mp3dec_init(&mp3d); str_rs.idle = FIR_TAPS;
    __asm__ volatile("dmb ish" ::: "memory");
    str->version = PSMB_STR_VERSION; str->magic = PSMB_STR_MAGIC;
    if (out_want == OUT_OFF && !quiet) kprintf("[AUDIO] mac.audio=off: the Mac's sound stays on %s\n", psmac_board_se && !psmac_board_solo ? "the SE's speaker" : "nothing here");
    else if (out_kind) kprintf("[AUDIO] the ASC (22257 Hz, both FIFOs) %s mixed on core 1 at %u Hz%s\n",
                               card_on ? "and the sound card (44.1 kHz, the Aurora68 Sound extension)" : "alone (mac.sndcard=off)", out_rate,
                               psmac_board_se && !psmac_board_solo ? "; the SE's speaker plays as well" : "");
    __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
}

/* How long core 1 kept the sound waiting (23 November 2026, the owner's SE: noise on HDMI after the boot chime with the
   PC kernel, the PC running on core 1): the longest time between two calls here over the heartbeat's five seconds, and
   how many were longer than 32 ms (the ring is six 8 ms blocks: past four of them the block being filled is the one
   before the one playing, "filled late", and past six the DMA plays old sound). The heartbeat's audio line says both, in
   QEMU too, where mac.audiodump= has no DMA ring to be late for. */
static uint64_t poll_last_t, poll_gap_max;
static uint32_t poll_gaps_late;
/* from core 1's loop, and from its waits inside a USB enumeration (psmac_usb.c's delay_us): fill
   whatever the output has played. Returns 1 if it did something. */
int psmac_audio_poll(void)
{
    if (!__atomic_load_n(&started, __ATOMIC_ACQUIRE) || out_kind == OUT_OFF) return 0;
    {
        uint64_t t = now();
        if (poll_last_t) {
            uint64_t d = t - poll_last_t;
            if (d > poll_gap_max) poll_gap_max = d;
            if (d > hz() * 32u / 1000u) poll_gaps_late++;
        }
        poll_last_t = t;
    }
    if (card) card_vol = card->volume;
    str_poll();
    asc_steer();
    if (out_kind == OUT_FILE) {
        uint64_t due = (now() - dump_t0) * out_rate / hz() / BLK;
        if (dump_blocks >= due) return 0;
        if (due - dump_blocks > 50) dump_blocks = (uint32_t)due - 1;  /* the testbed lost time: no backlog */
        fill(0, dump_buf);
        psmac_sh_write(dump_fd, dump_buf, sizeof dump_buf);
        dump_blocks++;
        return 1;
    }
    if (rd(DMA_CS(dma_ch)) & (1u << 8)) {                           /* a DMA error: say so once, then start it again */
        if (!dma_errors++) kprintf("[AUDIO] DMA channel %d error, debug %08x: restarted\n", dma_ch, rd(DMA_DEBUG(dma_ch)));
        wr(DMA_CS(dma_ch), 1u << 31); wait_us(10); dma_start();
    }
    int playing = dma_playing();
    if (playing < 0) return 0;
    int did = 0;
    while (fill_next != (uint32_t)playing) {
        if ((fill_next + 1) % NBLK == (uint32_t)playing) late_fills++;   /* the last block before the one playing: nearly too late */
        fill(blk[fill_next], 0);
        arm_flush_cache((uintptr_t)blk[fill_next], BLK * 8);
        fill_next = (fill_next + 1) % NBLK;
        did = 1;
    }
    return did;
}
void psmac_audio_pause(void) { if (out_kind == OUT_HDMI || out_kind == OUT_JACK) silence_all(); }

/* the heartbeat's line */
void psmac_audio_report(void)
{
    if (!started || out_kind == OUT_OFF) return;
    uint32_t gap_ms = (uint32_t)(poll_gap_max * 1000u / hz());
    poll_gap_max = 0;
    kprintf("[PSMAC]   audio: %s %u Hz, %u blocks%s | ASC %u starts, %u samples, %u wavetable, %u underruns, %u dropped | card %u frames, %u underruns, queued %d, volume %08x | %u clipped | core 1's longest wait %u ms, %u over 32 ms\n",
            out_kind == OUT_HDMI ? "HDMI" : out_kind == OUT_JACK ? "jack" : "file", out_rate, out_kind == OUT_FILE ? dump_blocks : blocks_filled,
            dma_ch >= 0 ? (late_fills ? " (some filled late)" : "") : "", asc.starts, asc.samples, asc.wt_samples, asc.underruns, asc_dropped,
            card_frames, card ? card->underruns : 0, card ? (int)(card->head - card->tail) : 0, card_vol, clips, gap_ms, poll_gaps_late);
    if (str && str_used) {
        static const char *const sn[] = { "idle", "playing", "paused", "ended", "error" };
        uint32_t s = str->state < 5 ? str->state : 4;
        kprintf("[PSMAC]   stream: %s, %s %u Hz %u ch %u kbit/s, %u.%u of %u.%u s, %u MPEG frames, %u KB buffered, %d in the ring, "
                "%u underruns, %u bytes skipped, decoder %u per mille of core 1 (slice at most %u us), volume %08x\n",
                sn[s], st.format == PSMB_STR_PCM ? "PCM" : "MPEG", str->rate, str->channels, str->bitrate, str->pos_ms / 1000, str->pos_ms / 100 % 10,
                str->length_ms / 1000, str->length_ms / 100 % 10, str->mpeg_frames, str->buffered >> 10, (int)(str->head - str->tail),
                str->underruns, str->skipped, str->load, str_max_us, str->volume);
    }
}
