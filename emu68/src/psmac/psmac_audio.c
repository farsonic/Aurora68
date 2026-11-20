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
 * Not verified on hardware (20 November 2026): the HDMI and PWM output and their DMA; everything
 * from the rings to the samples is, in QEMU, through mac.audiodump=. */
#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "spinlock.h"
#include "psmac.h"
#include "psmb.h"
#include "psmac_audio_fir.h"

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

/* ---- options ---------------------------------------------------------------------------------- */
enum { OUT_OFF, OUT_HDMI, OUT_JACK, OUT_FILE };
static int opt_read, out_want = OUT_HDMI, out_kind = OUT_OFF, card_on = 1;
static uint32_t out_rate = 48000, test_secs;
static int test_card;
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

/* the SE's speaker plays the card too: a mono feed at the ASC's rate, taken by psmac_board_se.c's
   speaker beside the ASC's own bytes. Two card frames averaged, every 44100/22257th one, which is
   a speaker of 8 bits and two inches. */
#define SPK_RING 4096u
static uint8_t spk_ring[SPK_RING];
static uint32_t spk_head, spk_tail, spk_frac;
static int32_t spk_prev;
static void spk_feed(int32_t l, int32_t rt)
{
    int32_t m = (l + rt) / 2, v = (m + spk_prev) / 2;
    spk_prev = m;
    spk_frac += ASC_RATE;
    if (spk_frac < CARD_RATE) return;
    spk_frac -= CARD_RATE;
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

/* one output frame: each source stepped through its resampler, then mixed and clipped */
static uint32_t clips;
static void rs_step(struct rs *r, void (*next)(int32_t *, int32_t *), int32_t *l, int32_t *rt)
{
    uint64_t a = (uint64_t)r->frac + r->step;
    for (uint32_t n = (uint32_t)(a >> 32); n; n--) {
        int32_t x, y;
        card_took = 0;
        next(&x, &y);
        rs_push(r, x, y);
        if (card_took && psmac_board_se && !psmac_board_solo) spk_feed(x, y);   /* only real frames: silence would keep the speaker writing */
    }
    r->frac = (uint32_t)a;
    rs_out(r, l, rt);
}
static void mix_frame(int16_t *lo, int16_t *ro)
{
    int32_t l = 0, r = 0, x, y;
    rs_step(&asc_rs, asc_next, &x, &y); l += x; r += y;
    if (card) { rs_step(&card_rs, card_next, &x, &y); l += x; r += y; }
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
static volatile int started;
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
    if (out_want == OUT_OFF && !quiet) kprintf("[AUDIO] mac.audio=off: the Mac's sound stays on %s\n", psmac_board_se && !psmac_board_solo ? "the SE's speaker" : "nothing here");
    else if (out_kind) kprintf("[AUDIO] the ASC (22257 Hz, both FIFOs) %s mixed on core 1 at %u Hz%s\n",
                               card_on ? "and the sound card (44.1 kHz, the PiSound extension)" : "alone (mac.sndcard=off)", out_rate,
                               psmac_board_se && !psmac_board_solo ? "; the SE's speaker plays as well" : "");
    __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
}

/* from core 1's loop, and from its waits inside a USB enumeration (psmac_usb.c's delay_us): fill
   whatever the output has played. Returns 1 if it did something. */
int psmac_audio_poll(void)
{
    if (!__atomic_load_n(&started, __ATOMIC_ACQUIRE) || out_kind == OUT_OFF) return 0;
    if (card) card_vol = card->volume;
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
    kprintf("[PSMAC]   audio: %s %u Hz, %u blocks%s | ASC %u starts, %u samples, %u wavetable, %u underruns, %u dropped | card %u frames, %u underruns, queued %d, volume %08x | %u clipped\n",
            out_kind == OUT_HDMI ? "HDMI" : out_kind == OUT_JACK ? "jack" : "file", out_rate, out_kind == OUT_FILE ? dump_blocks : blocks_filled,
            dma_ch >= 0 ? (late_fills ? " (some filled late)" : "") : "", asc.starts, asc.samples, asc.wt_samples, asc.underruns, asc_dropped,
            card_frames, card ? card->underruns : 0, card ? (int)(card->head - card->tail) : 0, card_vol, clips);
}
