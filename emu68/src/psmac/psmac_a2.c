/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_a2.c — the Aurora68 Apple II Card: an 8-bit Apple II (Aurora65) on a Pi core, shown in a window on the Mac's
 * desktop (docs/20-apple-ii-card.md; Aurora65's docs/DESIGN.md, "The card in Aurora68" and "The guest scheduler").
 *
 * Backstory (23 November 2026). The owner asked for the Apple II family beside the PC card, "8-bit first": the II, II+,
 * IIe, enhanced IIe and IIc in a Mac window, 1:1 on the SE's screen, the front window's machine first on a shared core.
 * Aurora65, a project of its own beside this repository, made the machine: its own 6502/65C02, memory and switches,
 * the Disk II, a block-device card with its own firmware and the picture, built freestanding and big-endian with this
 * kernel's own flags as libaurora65.a, driven through aurora65.h, whose only outside names are a65_host_alloc,
 * a65_host_free and a65_host_log. This file is everything Macintosh about it, as psmac_pc.c is for the PC:
 *  - Its place: slot $E (psmb.h, PSMB_A2_*): a 1 MiB window at $FE000000 and $FEE00000 (the slot's 24-bit megabyte)
 *    holding the block of psmac_a2.h, and the declaration ROM at the slot's top (psmac_a2_declrom.h,
 *    tools/mac/mka2declrom.py: board "Aurora68 Apple II Card", the driver ".A68A2", mac/a68apple.s). The MMU shim maps
 *    the three as it maps slot $D's.
 *  - Its memory: the window, the ROM page and a pool of A2_POOL_MB for the machine (about 0.7 MB), its 5.25" images'
 *    bit streams (220 KB each) and the write-back overlay, carved from the Pi's RAM by psmac_bus.c beside the other
 *    cards': the kernel's heap is shared by everything else.
 *  - Its core: the PC's rule (core 2 on the solo board, where it is light; core 1 in slices elsewhere), a2.core=1|2 to
 *    choose; with the PC card on the same core, psmac_guest.c shares it, the front window's machine first. The Apple
 *    is paced: a slice runs the Apple's time that has passed in real time (a65_apple_run takes microseconds of the
 *    Apple's own 1.02 MHz clock), at most 20 ms of it, and more than 50 ms behind it drops the rest (it slows down
 *    rather than bursting). Waiting for a key the library calls it idle and skips 7 frames of 8; the card then asks
 *    for the core a sixteenth as often.
 *  - Its files: the ROM (the owner's own dump: a2.rom=, or by the model's name, identified by the chips' CRC-32s that
 *    Aurora65's tools/romcheck.py lists, its SHA-256 logged for pinning; refused clearly when missing), the Disk II's
 *    boot PROM if the owner has it (a2.p5=), disk images (a2.d1=, a2.d2= 5.25"; a2.hd1= ... a2.hd4= block images), on
 *    the SD card (psmac_card.c's FAT reader, the PC card's calls) or, in the testbed, host files through semihosting.
 *    The library reads and writes an image at any offset through a65_io; the card puts a sector overlay in front of
 *    the file (the PC card's way): writes go into the Pi's memory by 512-byte sector and from there back into the file
 *    in place, 2 s after the last write, 10 s at most, and at once at power off, reset, eject and a change of model.
 *  - Its screen: drawn by this core into the Mac's own frame buffer inside the window's content and only where its
 *    visRgn shows (psmac_wclip.c), never in the box around the Mac's pointer, never while a menu is down, at most
 *    30 times a second and only the rows that changed. On a one-bit screen at 1:1 (the owner's choice): 40 columns,
 *    lores and hires as 280 x 192, one Mac pixel an Apple pixel (hires as a monochrome monitor shows it, lores
 *    ordered-dithered from its colours), 80 columns from the text cells in Aurora65's own font at 6 x 8 (480 x 192),
 *    double hires 560 x 192; black on white unless A68A_O_APPLESENSE; scaled to the window as the alternative. On a
 *    deeper screen: the 560 x 192 colour picture with its lines doubled, 1:1 or by the largest whole number that fits.
 *  - Its input: the Mac's key downs (character and virtual key code, a65_apple_mac_key: Command is Open-Apple, Option
 *    Solid-Apple), key ups, modifier changes, the mouse as the joystick, from the program through the block's ring.
 *  - Its sound: the speaker's samples (a65_apple_audio) to the Mac's sound output's sixth source (psmac_audio.c).
 * Built only with -DPSMAC_A2=ON (emu68/CMakeLists.txt, which links libaurora65.a): the normal kernel has none of this.
 * a2=on (default off) puts the card in slot $E and starts the Apple with the Mac.
 */
#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "psmac.h"
#include "psmb.h"
#include "psmac_a2.h"
#include "psmac_wclip.h"
#include "psmac_guest.h"
#include "psmac_a2_declrom.h"
#include "aurora65.h"
#include "a65_font.h"                        /* Aurora65's own 5 x 7 font (its tools/mkfont.py), for 80 columns on one bit */

#ifdef PSMAC_A2_HOST
#define KERNEL_ALIAS 0
#else
#define KERNEL_ALIAS 0xffffff9000000000ull
#endif
extern const char *psmac_option(const char *key);
extern void psmac_mmu_set_a2(uintptr_t win, uintptr_t rom);
extern int psmac_display_surface(int i, uint8_t **pix, uint32_t *w, uint32_t *h, uint32_t *rb, uint32_t *depth,
                                 const volatile uint32_t **clut, uint32_t *clut_seq, int *gray);
extern int64_t psmac_sh_open(const char *name, uint64_t mode);
extern int psmac_sh_read(int64_t fd, void *buf, uint64_t len);
extern int psmac_sh_write(int64_t fd, const void *buf, uint64_t len);
extern int64_t psmac_sh_seek(int64_t fd, uint64_t pos);
extern int64_t psmac_sh_flen(int64_t fd);
extern int psmac_sh_available(void);
extern int psmac_card_pc_open(const char *name, uint32_t *size);                 /* psmac_card.c, the guests' files */
extern int psmac_card_pc_read(int file, uint32_t off, void *buf, uint32_t n);
extern int psmac_card_pc_write(int file, uint32_t off, const void *buf, uint32_t n);
extern int psmac_card_pc_writable(void);
extern int psmac_card_pc_list(void (*cb)(void *ctx, const char *name, uint32_t size), void *ctx);
extern int psmac_audio_guest_open(uint32_t rate);                                /* psmac_audio.c, source 6 */
extern uint32_t psmac_audio_guest_room(void);
extern void psmac_audio_guest_push(const int16_t *s, uint32_t n);
extern int psmac_board_se, psmac_board_solo;

#ifdef PSMAC_A2_HOST                         /* tools/a2card_test.c: this file on the host, with the host's Aurora65 library */
uint64_t a2_host_now(void);
static inline uint64_t now(void) { return a2_host_now(); }
static inline uint64_t hz(void)  { return 1000000u; }
#define DMB() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#else
static inline uint64_t now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
#define DMB() __asm__ volatile("dmb ish" ::: "memory")
#endif
static inline uint32_t us_since(uint64_t t) { return (uint32_t)((now() - t) * 1000000u / hz()); }
static inline uint64_t ms_ticks(uint32_t ms) { return (uint64_t)ms * hz() / 1000u; }

#define A2_POOL_MB     16u           /* the machine (0.7 MB), two 5.25" images' bits (220 KB each), the ROMs, the overlay */
#define DRAW_HZ        30u
#define INSET          2             /* one bit at 1:1: pixels of paper between the window's frame and the picture, as the
                                        PC card's TEXT_INSET (a glyph's first column touched the frame and read as a cut-off
                                        letter there): 284 x 192 for 280, 484 x 192 for 80 columns */
#define AGAIN_MS       100u          /* the pointer's old box drawn again this long after the first time (psmac_pc.c) */
#define RUN_MAX_US     20000u        /* the most of the Apple's time one slice runs: a catch-up is spread out */
#define BEHIND_MAX_US  50000u        /* further behind than this, the rest is dropped (Aurora65's DESIGN) */
#define KEY_WAIT_MS    250u          /* a key not taken by the Apple in this time is replaced by the next, as the latch is */
#define AUDIO_RATE     44100u
#define FLUSH_IDLE_MS  2000u         /* the PC card's write-back schedule (docs/19-pc-card.md "Disks") */
#define FLUSH_MAX_MS   10000u
#define FLUSH_SLICE_US 4000u
#define FLUSH_RUN      64u           /* sectors per write at most: 32 KB */
#define OV_BUCKETS     1024u

static struct {
    int on;                          /* a2=on */
    int model;                       /* a2.model= (-1: not given) */
    uint32_t core;                   /* a2.core= (0: by the board) */
    char rom[128], p5[128];
} opt;
static uintptr_t card_phys;
static volatile struct a68a_block *blk;
static uint8_t *win;
static void *pool;
static uintptr_t pool_lo, pool_hi;
static volatile int ready;           /* psmac_a2card_init done */
static int chosen = -1;              /* the core it runs on, decided at the first poll */
static a65_apple *M;                 /* the machine, once built */

static const char *const model_name[A68A_MODELS] = { "Apple ][", "Apple ][+", "Apple //e", "Apple //e (enhanced)", "Apple //c" };
static const char *const model_key[A68A_MODELS] = { "ii", "iiplus", "iie", "iie-enhanced", "iic" };
/* the ROM's file in the card's root when a2.rom= does not name one, by model */
static const char *const model_rom[A68A_MODELS] = { "apple2.rom", "apple2plus.rom", "apple2e.rom", "apple2e-enhanced.rom", "apple2c.rom" };

/* ---- options --------------------------------------------------------------------------------------- */
static uint32_t num(const char *v, uint32_t dflt)
{
    if (!v || v[0] < '0' || v[0] > '9') return dflt;
    uint32_t n = 0;
    while (*v >= '0' && *v <= '9') n = n * 10 + (uint32_t)(*v++ - '0');
    return n;
}
static int word_is(const char *v, const char *w)
{
    while (*w && *v == *w) { v++; w++; }
    return !*w && (!*v || *v == ' ');
}
static int is_on(const char *key) { const char *v = psmac_option(key); return v && v[0] == 'o' && v[1] == 'n'; }
static void str_opt(char *out, uint32_t max, const char *key, const char *dflt)
{
    const char *v = psmac_option(key);
    if (!v || !v[0]) v = dflt;
    uint32_t i = 0;
    while (v && v[i] && v[i] != ' ' && i < max - 1) { out[i] = v[i]; i++; }
    out[i] = 0;
}
static void options(void)
{
    static int done;
    if (done) return;
    done = 1;
    opt.on = is_on("a2");
    opt.model = -1;
    const char *v = psmac_option("a2.model");
    if (v) for (int i = 0; i < (int)A68A_MODELS; i++) if (word_is(v, model_key[i])) opt.model = i;
    if (v && opt.model < 0 && word_is(v, "iiee")) opt.model = A68A_MODEL_IIEE;
    opt.core = num(psmac_option("a2.core"), 0);
    if (opt.core != 1 && opt.core != 2) opt.core = 0;
    str_opt(opt.rom, sizeof opt.rom, "a2.rom", "");
    str_opt(opt.p5, sizeof opt.p5, "a2.p5", "disk2-p5.rom");
}
static uint32_t pool_bytes(void) { return A2_POOL_MB << 20; }

uint32_t psmac_a2card_size(void)
{
    options();
    return opt.on ? A68A_WIN_SIZE + PSMB_A2_ROM_SIZE + pool_bytes() : 0;
}
void psmac_a2card_map(uintptr_t phys)
{
    card_phys = phys;
    psmac_mmu_set_a2(phys, phys + A68A_WIN_SIZE);
}

/* ---- what libaurora65 asks of the host (a65_host.h) -------------------------------------------------- */
void *a65_host_alloc(size_t n)
{
    void *p = pool ? tlsf_malloc_aligned(pool, n ? n : 1, 16) : 0;
    if (p) memset(p, 0, n);                       /* the library relies on it */
    return p;
}
void a65_host_free(void *p)
{
    if (p && (uintptr_t)p >= pool_lo && (uintptr_t)p < pool_hi) tlsf_free(pool, p);
}
void a65_host_log(const char *line) { kprintf("[A2] %s\n", line); }

/* ---- the block ------------------------------------------------------------------------------------- */
static void say(const char *s)
{
    uint32_t i = 0;
    while (s[i] && i < sizeof blk->message - 1) { blk->message[i] = s[i]; i++; }
    blk->message[i] = 0;
}
static void tell(const char *s) { say(s); kprintf("[A2] %s\n", s); }
static void set_state(uint32_t st, uint32_t err, const char *msg)
{
    blk->state = st; blk->error = err;
    if (msg) tell(msg);
}
#define MSG_MAX 400           /* a message's buffer: its pieces are a name (63), a path (127), a reason and words, under 400 */
static void put(char **p, const char *s) { while (*s) *(*p)++ = *s++; }

void psmac_a2card_init(void)
{
    if (!card_phys) return;
    uint8_t *rom = (uint8_t *)(KERNEL_ALIAS + card_phys + A68A_WIN_SIZE);
    for (uint32_t i = 0; i < PSMB_A2_ROM_SIZE; i++) rom[i] = 0;
    memcpy(rom + PSMB_A2_ROM_SIZE - sizeof psmac_a2_declrom, psmac_a2_declrom, sizeof psmac_a2_declrom);   /* the format block at the slot's top */
    arm_flush_cache((uintptr_t)rom, PSMB_A2_ROM_SIZE);
    win = (uint8_t *)(KERNEL_ALIAS + card_phys);
    for (uint32_t i = 0; i < A68A_WIN_SIZE; i += 8) *(uint64_t *)(win + i) = 0;
    blk = (volatile struct a68a_block *)win;
    struct a68a_disks *dk = (struct a68a_disks *)(win + A68A_OFF_DISKS);
    for (uint32_t i = 0; i < A68A_UNITS; i++) dk->unit[i] = -1;
    uint8_t *heap = (uint8_t *)(KERNEL_ALIAS + card_phys + A68A_WIN_SIZE + PSMB_A2_ROM_SIZE);
    pool = tlsf_init_with_memory(heap, pool_bytes());
    pool_lo = (uintptr_t)heap; pool_hi = pool_lo + pool_bytes();
    blk->win_size = A68A_WIN_SIZE;
    blk->model = (uint32_t)(opt.model >= 0 ? opt.model : (int)A68A_MODEL_IIEE);
    blk->magic = A68A_MAGIC;
    set_state(A68A_S_OFF, 0, "The Apple II is off.");
    DMB();
    blk->version = A68A_VERSION;
    arm_flush_cache((uintptr_t)win, 4096);
    ready = 1;
    kprintf("[A2] the Apple II card in slot $%X: window $%08X (24-bit $%06X), ROM %u bytes at the slot's top; %u MiB for the machine; model %s, ROM %s\n",
            PSMB_A2_SLOT, PSMB_A2_WIN_PHYS, PSMB_A2_SLOT << 20, (uint32_t)sizeof psmac_a2_declrom, pool_bytes() >> 20,
            model_name[blk->model], opt.rom[0] ? opt.rom : model_rom[blk->model]);
}

/* ---- files: the SD card, or the testbed's host through semihosting (psmac_pc.c's) ---------------------- */
struct pfile { int sh; int64_t fd; int card; uint32_t size; int rw; };
static int pf_open(struct pfile *f, const char *name, int rw)
{
    f->sh = 0; f->fd = -1; f->card = -1; f->size = 0; f->rw = 0;
    if (!name[0]) return -1;
    if (psmac_sh_available()) {
        int64_t fd = rw ? psmac_sh_open(name, 3) : -1;   /* semihosting's SYS_OPEN modes: 1 "rb", 3 "r+b" */
        if (fd >= 0) f->rw = 1;
        else fd = psmac_sh_open(name, 1);
        if (fd >= 0) { f->sh = 1; f->fd = fd; f->size = (uint32_t)psmac_sh_flen(fd); return 0; }
    }
    int c = psmac_card_pc_open(name, &f->size);
    if (c < 0) return c;
    f->card = c;
    f->rw = rw && psmac_card_pc_writable();
    return 0;
}
/* n bytes at off, cut at the file's end (the rest of the buffer zero: an image's last sector may be short) */
static int pf_read(struct pfile *f, uint32_t off, void *buf, uint32_t n)
{
    uint32_t k = off >= f->size ? 0 : (n < f->size - off ? n : f->size - off);
    if (k < n) memset((uint8_t *)buf + k, 0, n - k);
    if (!k) return 0;
    if (f->sh) {
        if (psmac_sh_seek(f->fd, off) < 0) return -1;
        return psmac_sh_read(f->fd, buf, k) ? 0 : -1;
    }
    return psmac_card_pc_read(f->card, off, buf, k);
}
/* whole sectors at a sector's offset; the part past the file's end is not written into a host file (it would grow),
   and on the card it falls in the last cluster's own unused bytes (psmac_card_pc_write allows the last short sector) */
static int pf_write(struct pfile *f, uint32_t off, const void *buf, uint32_t n)
{
    if (!f->rw || off >= f->size) return -1;
    if (f->sh) {
        uint32_t k = n < f->size - off ? n : f->size - off;
        if (psmac_sh_seek(f->fd, off) < 0) return -1;
        return psmac_sh_write(f->fd, buf, k) ? 0 : -1;
    }
    return psmac_card_pc_write(f->card, off, buf, n);
}

/* ---- ROMs: which one is it? ----------------------------------------------------------------------------
   The owner's own dumps (never Apple's files from anywhere else: Aurora65's CLAUDE.md). Identified as Aurora65's
   tools/romcheck.py does, by the file's size and each ROM chip's CRC-32 as MAME's drivers publish them (romcheck.py also
   checks SHA-1; the CRC-32s of every chip together are what the kernel checks), and the file's SHA-256 is logged so that
   the owner can pin a dump. A file of the right size that matches no chip is used all the same, with a word in the log
   (another revision, a dump of an EPROM'd machine); Aurora65's own test ROM says so in its first bytes. */
struct romid { const char *name; int model; uint32_t size; uint32_t n; struct { uint32_t off, len, crc; } chip[6]; };
static const struct romid known[] = {
    { "Apple II (Integer BASIC; the $F800 monitor is not in MAME's list)", A68A_MODEL_II, 12288, 3,
      { { 0x1000, 0x800, 0xc0a4ad3bu }, { 0x1800, 0x800, 0xa99c2cf6u }, { 0x2000, 0x800, 0x62230d38u } } },
    { "Apple II+ (Applesoft, autostart monitor)", A68A_MODEL_IIPLUS, 12288, 6,
      { { 0x0000, 0x800, 0x6f05f949u }, { 0x0800, 0x800, 0x1f08087cu }, { 0x1000, 0x800, 0x2b8d9a89u },
        { 0x1800, 0x800, 0x5719871au }, { 0x2000, 0x800, 0x9a04eecfu }, { 0x2800, 0x800, 0x079589c4u } } },
    { "Apple IIe (unenhanced)", A68A_MODEL_IIE, 16384, 2, { { 0x0000, 0x2000, 0xe248835eu }, { 0x2000, 0x2000, 0xfc3d59d8u } } },
    { "Apple IIe enhanced", A68A_MODEL_IIEE, 16384, 2, { { 0x0000, 0x2000, 0x443aa7c4u }, { 0x2000, 0x2000, 0x95e10034u } } },
    { "Apple IIc (original, ROM 255)", A68A_MODEL_IIC, 16384, 1, { { 0, 0x4000, 0xf0edaa1bu } } },
    { "Apple IIc (UniDisk 3.5 ROM, 0)", -1, 32768, 1, { { 0, 0x8000, 0xc8b979b3u } } },
    { "Apple IIc (ROM 3, memory expansion)", -1, 32768, 1, { { 0, 0x8000, 0xbc5a79ffu } } },
    { "Apple IIc (ROM 4)", -1, 32768, 1, { { 0, 0x8000, 0x06f53328u } } },
    { "Apple IIc Plus", -1, 32768, 1, { { 0, 0x8000, 0x0b996420u } } },
};
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
/* SHA-256 (FIPS 180-4), for the log only */
static void sha256(const uint8_t *p, uint32_t n, uint8_t out[32])
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint64_t bits = (uint64_t)n * 8;
    uint32_t total = (n + 9 + 63) & ~63u;
    for (uint32_t blkoff = 0; blkoff < total; blkoff += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            uint32_t v = 0;
            for (int k = 0; k < 4; k++) {
                uint32_t at = blkoff + (uint32_t)i * 4 + (uint32_t)k;
                uint8_t b = at < n ? p[at] : at == n ? 0x80 : at >= total - 8 ? (uint8_t)(bits >> (8 * (total - 1 - at))) : 0;
                v = v << 8 | b;
            }
            w[i] = v;
        }
        for (int i = 16; i < 64; i++) {
            uint32_t a = w[i - 15], b = w[i - 2];
            uint32_t s0 = (a >> 7 | a << 25) ^ (a >> 18 | a << 14) ^ (a >> 3), s1 = (b >> 17 | b << 15) ^ (b >> 19 | b << 13) ^ (b >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = (e >> 6 | e << 26) ^ (e >> 11 | e << 21) ^ (e >> 25 | e << 7), ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = (a >> 2 | a << 30) ^ (a >> 13 | a << 19) ^ (a >> 22 | a << 10), mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int i = 0; i < 8; i++) for (int k = 0; k < 4; k++) out[i * 4 + k] = (uint8_t)(h[i] >> (24 - 8 * k));
}
static int has_text(const uint8_t *p, uint32_t n, uint32_t off, const char *t)
{
    for (uint32_t i = 0; t[i]; i++) if (off + i >= n || p[off + i] != (uint8_t)t[i]) return 0;
    return 1;
}
/* the known ROM it is (an index in known[]), -2 for Aurora65's test ROM, -1 for none */
static int rom_identify(const uint8_t *p, uint32_t n)
{
    if ((n == 12288 && has_text(p, n, 0, "AURORA65 TEST ROM")) || (n == 16384 && has_text(p, n, 0x1000, "AURORA65 TEST ROM"))) return -2;
    for (uint32_t i = 0; i < sizeof known / sizeof known[0]; i++) {
        if (known[i].size != n) continue;
        uint32_t k = 0;
        while (k < known[i].n && crc32(p + known[i].chip[k].off, known[i].chip[k].len) == known[i].chip[k].crc) k++;
        if (k == known[i].n) return (int)i;
    }
    return -1;
}
static void log_sha(const char *what, const uint8_t *p, uint32_t n)
{
    uint8_t d[32];
    char hex[65];
    sha256(p, n, d);
    for (int i = 0; i < 32; i++) { hex[2 * i] = "0123456789abcdef"[d[i] >> 4]; hex[2 * i + 1] = "0123456789abcdef"[d[i] & 15]; }
    hex[64] = 0;
    kprintf("[A2]   %s: %u bytes, SHA-256 %s\n", what, n, hex);
}

/* ---- disks ---------------------------------------------------------------------------------------------------
   Each image is a file read and written through a65_io at any offset and length (a 256-byte sector of a .dsk, a
   512-byte block 64 bytes into a .2mg, a WOZ track and its CRC). In front of the file a sector overlay (the PC card's):
   what the Apple writes goes into the Pi's memory by 512-byte sector of the file (a part sector read from the file
   first), reads see the overlay first, and the sectors that changed go back into the file in place, in runs, through
   the PC card's file calls (the file's own clusters, never its size or a directory entry). When: the library writes a
   block image's blocks to its io at once and a Disk II track's sectors at a65_apple_flush; the card calls that flush and
   writes the overlay back FLUSH_IDLE_MS after the Apple's last change to the image, FLUSH_MAX_MS after the oldest at
   most, and everything at once at power off, reset, eject and a change of model (the PC card's schedule). a2.d1ro=on
   (and so on) attaches an image read only: its changes stay in the library's memory and its file is never written. */
struct ovsec { struct ovsec *next; uint32_t lba, dirty; uint8_t data[512]; };
struct a2img {
    char name[64], path[128];
    struct pfile f;
    a65_io io;
    int open, ro, ro_opt, failed;
    uint32_t size, kind;
    int32_t unit;
    struct ovsec **ov;
    uint32_t held, dirty;             /* sectors in memory; of those, not yet written back */
    uint32_t reads, writes, flushed, cursor, changes_seen;
    int pending;                      /* the Apple changed it since the last write-back */
    uint64_t first_t, last_t, flush_t0;
};
static struct a2img img[A68A_DISKS_MAX];
static uint32_t nimg;
static int32_t unit_img[A68A_UNITS] = { -1, -1, -1, -1, -1, -1 };
static uint32_t io_writes;
static uint8_t runbuf[FLUSH_RUN * 512] __attribute__((aligned(16)));

static struct ovsec *ov_find(struct a2img *m, uint32_t lba)
{
    if (!m->ov) return 0;
    for (struct ovsec *s = m->ov[lba % OV_BUCKETS]; s; s = s->next)
        if (s->lba == lba) return s;
    return 0;
}
static void ov_drop(struct a2img *m, struct ovsec *s)
{
    struct ovsec **pp = &m->ov[s->lba % OV_BUCKETS];
    while (*pp && *pp != s) pp = &(*pp)->next;
    if (*pp) *pp = s->next;
    a65_host_free(s);
    m->held--;
}
static int img_rd(void *ctx, uint64_t off, void *buf, uint32_t n)
{
    struct a2img *m = ctx;
    uint8_t *b = buf;
    m->reads++;
    if (!m->open || off > m->size || n > m->size - off) return -1;
    while (n) {
        uint32_t lba = (uint32_t)(off >> 9), in = (uint32_t)(off & 511), k = 512 - in;
        if (k > n) k = n;
        struct ovsec *s = ov_find(m, lba);
        if (s) memcpy(b, s->data + in, k);
        else {
            uint32_t run = k;                       /* sectors not in the overlay in one read */
            while (run < n && !ov_find(m, (uint32_t)((off + run) >> 9)) && run < (64u << 10)) run += n - run < 512 ? n - run : 512;
            if (pf_read(&m->f, (uint32_t)off, b, run) < 0) return -1;
            k = run;
        }
        b += k; off += k; n -= k;
    }
    return 0;
}
static int img_wr(void *ctx, uint64_t off, const void *buf, uint32_t n)
{
    struct a2img *m = ctx;
    const uint8_t *b = buf;
    m->writes++; io_writes++;
    if (!m->open || off > m->size || n > m->size - off) return -1;
    if (!m->ov) {
        m->ov = a65_host_alloc(OV_BUCKETS * sizeof *m->ov);
        if (!m->ov) return -1;
    }
    uint64_t t = now();
    while (n) {
        uint32_t lba = (uint32_t)(off >> 9), in = (uint32_t)(off & 511), k = 512 - in;
        if (k > n) k = n;
        struct ovsec *s = ov_find(m, lba);
        if (!s) {
            s = a65_host_alloc(sizeof *s);
            if (!s) return -1;
            if ((in || k < 512) && pf_read(&m->f, lba << 9, s->data, 512) < 0) { a65_host_free(s); return -1; }
            s->lba = lba; s->dirty = 0; s->next = m->ov[lba % OV_BUCKETS]; m->ov[lba % OV_BUCKETS] = s;
            m->held++;
        }
        memcpy(s->data + in, b, k);
        if (!s->dirty && !m->ro && !m->failed) { s->dirty = 1; m->dirty++; }
        b += k; off += k; n -= k;
    }
    if (!m->pending) m->first_t = t;
    m->pending = 1; m->last_t = t;
    return 0;
}
/* the overlay's changed sectors back into the file, in runs, until there are none or the deadline (0: none) */
static void flush_image(struct a2img *m, uint64_t deadline)
{
    while (m->dirty && (!deadline || now() < deadline)) {
        struct ovsec *s = 0;
        for (uint32_t k = 0; k < OV_BUCKETS && !s; k++) {
            uint32_t bkt = (m->cursor + k) % OV_BUCKETS;
            for (struct ovsec *q = m->ov[bkt]; q; q = q->next)
                if (q->dirty) { s = q; m->cursor = bkt; break; }
        }
        if (!s) { m->dirty = 0; break; }
        uint32_t lba = s->lba, n = 0;
        struct ovsec *run[FLUSH_RUN];
        while (n < FLUSH_RUN) {
            struct ovsec *q = ov_find(m, lba + n);
            if (!q || !q->dirty) break;
            memcpy(runbuf + n * 512u, q->data, 512);
            run[n++] = q;
        }
        if (pf_write(&m->f, lba << 9, runbuf, n * 512u) < 0) {
            m->failed = 1;
            for (uint32_t k = 0; k < OV_BUCKETS; k++)
                for (struct ovsec *q = m->ov[k]; q; q = q->next) q->dirty = 0;
            kprintf("[A2] %s: writing sectors %u-%u back failed: its changes stay in memory from now on\n", m->name, lba, lba + n - 1);
            say("A disk's changes could not be written back to the card: they stay in memory (the kernel log says which).");
            m->dirty = 0;
            return;
        }
        for (uint32_t k = 0; k < n; k++) ov_drop(m, run[k]);
        m->dirty -= n; m->flushed += n;
    }
    if (!m->dirty && m->flushed) {
        kprintf("[A2] %s: %u sector%s written back to the %s (%u ms)\n", m->name, m->flushed, m->flushed == 1 ? "" : "s",
                m->f.sh ? "host's file" : "card", us_since(m->flush_t0) / 1000u);
        m->flushed = 0;
    }
}
/* the library's changes (a Disk II's tracks) into the overlay, and the overlay into the file: what is due, or all */
static void flush_unit(uint32_t u, uint64_t deadline)
{
    int32_t k = unit_img[u];
    if (k < 0) return;
    struct a2img *m = &img[k];
    if (!m->flushed) m->flush_t0 = now();
    if (M && a65_apple_flush(M, (int)u) < 0) kprintf("[A2] %s: the machine could not write its changes (an I/O error): tried again later\n", m->name);
    flush_image(m, deadline);
    if (!m->dirty) m->pending = 0;
}
static void flush_all(void) { for (uint32_t u = 0; u < A68A_UNITS; u++) flush_unit(u, 0); }
static uint64_t flush_check_t;
static void flush_poll(void)
{
    uint64_t t = now();
    if (t - flush_check_t < ms_ticks(50)) return;
    flush_check_t = t;
    uint64_t deadline = t + (uint64_t)FLUSH_SLICE_US * hz() / 1000000u;
    for (uint32_t u = 0; u < A68A_UNITS; u++) {
        int32_t k = unit_img[u];
        if (k < 0 || !M) continue;
        struct a2img *m = &img[k];
        struct a65_disk_state st;
        if (a65_apple_disk(M, u, &st) < 0) continue;
        if (st.changes != m->changes_seen) {
            if (!m->pending) m->first_t = t;
            m->pending = 1; m->last_t = t; m->changes_seen = st.changes;
        }
        if (!m->pending || m->ro) continue;
        if (t - m->last_t < ms_ticks(FLUSH_IDLE_MS) && t - m->first_t < ms_ticks(FLUSH_MAX_MS)) continue;
        flush_unit(u, deadline);
        if (now() >= deadline) break;
    }
}
static uint32_t dirty_units(void)
{
    uint32_t n = 0;
    for (uint32_t u = 0; u < A68A_UNITS; u++) if (unit_img[u] >= 0 && !img[unit_img[u]].ro && (img[unit_img[u]].pending || img[unit_img[u]].dirty)) n++;
    return n;
}

/* ---- the image list ------------------------------------------------------------------------------------------- */
static int ends_ci(const char *s, const char *suf)
{
    uint32_t n = 0, k = 0;
    while (s[n]) n++;
    while (suf[k]) k++;
    if (k > n) return 0;
    for (uint32_t i = 0; i < k; i++) { char c = s[n - k + i]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c != suf[i]) return 0; }
    return 1;
}
static int a2_ext(const char *n)
{
    return ends_ci(n, ".dsk") || ends_ci(n, ".do") || ends_ci(n, ".po") || ends_ci(n, ".nib") || ends_ci(n, ".woz") ||
           ends_ci(n, ".2mg") || ends_ci(n, ".hdv");
}
/* a 5.25" image or a block image, by its name and size (the library has the last word when it is attached) */
static uint32_t kind_of(const char *n, uint32_t size)
{
    if (ends_ci(n, ".nib") || ends_ci(n, ".woz")) return A68A_KIND_525;
    if (ends_ci(n, ".hdv")) return A68A_KIND_BLOCK;
    if (ends_ci(n, ".2mg")) return size >= 143360 && size <= 143360 + 1024 ? A68A_KIND_525 : A68A_KIND_BLOCK;
    return size == 143360 || size == 116480 ? A68A_KIND_525 : A68A_KIND_BLOCK;
}
static int same(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static int img_add(const char *path, uint32_t size)
{
    for (uint32_t i = 0; i < nimg; i++) if (same(img[i].path, path)) return (int)i;
    if (!path[0] || nimg >= A68A_DISKS_MAX) return -1;
    struct a2img *m = &img[nimg];
    uint32_t i = 0;
    while (path[i] && i < sizeof m->path - 1) { m->path[i] = path[i]; i++; }
    m->path[i] = 0;
    const char *n = path;
    for (const char *q = path; *q; q++) if (*q == '/') n = q + 1;
    for (i = 0; n[i] && i < sizeof m->name - 1; i++) m->name[i] = n[i];
    m->name[i] = 0;
    m->size = size; m->kind = kind_of(m->name, size); m->unit = -1;
    return (int)nimg++;
}
static int img_open(struct a2img *m)
{
    if (m->open) return 0;
    if (pf_open(&m->f, m->path, !m->ro) < 0) return -1;
    m->size = m->f.size;
    m->kind = kind_of(m->name, m->size);
    if (!m->f.rw) m->ro = 1;
    m->io.read = img_rd; m->io.write = m->ro ? 0 : img_wr; m->io.size = m->size; m->io.ctx = m;
    m->open = 1;
    kprintf("[A2] %s: %u KB, %s%s\n", m->name, m->size >> 10, m->kind == A68A_KIND_525 ? "a 5.25\" image" : "a block image",
            !m->ro ? ", its changes written back to it" : m->ro_opt ? ", read only (a2.d1ro=on ...): its changes stay in memory" : ", read only here: its changes stay in memory");
    return 0;
}
static void disks_publish(void)
{
    struct a68a_disks *dk = (struct a68a_disks *)(win + A68A_OFF_DISKS);
    for (uint32_t i = 0; i < nimg; i++) {
        uint32_t k = 0;
        while (img[i].name[k] && k < sizeof dk->d[i].name - 1) { dk->d[i].name[k] = img[i].name[k]; k++; }
        dk->d[i].name[k] = 0;
        dk->d[i].size = img[i].size; dk->d[i].kind = img[i].kind;
        dk->d[i].flags = img[i].ro ? A68A_DF_RO : 0; dk->d[i].unit = img[i].unit;
    }
    for (uint32_t u = 0; u < A68A_UNITS; u++) dk->unit[u] = unit_img[u];
    DMB();
    dk->count = nimg;
    dk->seq = dk->seq + 1;
}
static void card_list_cb(void *ctx, const char *name, uint32_t size)
{
    (void)ctx;
    if (a2_ext(name) && size >= 1024 && !(name[0] == '.' && name[1] == '_')) img_add(name, size);
}
static void rescan(void)
{
    psmac_card_pc_list(card_list_cb, 0);
    disks_publish();
}
static const char *const unit_name[A68A_UNITS] = { "drive 1 (slot 6)", "drive 2 (slot 6)", "HD1 (slot 7)", "HD2 (slot 7)", "HD3 (slot 5)", "HD4 (slot 5)" };
/* into a unit of the machine (it exists): 0, or -1 with the library's reason said */
static int put_in(uint32_t i, uint32_t u)
{
    struct a2img *m = &img[i];
    const char *why = 0;
    if (a65_apple_attach(M, u, &m->io, m->name, m->ro, &why) < 0) {
        char t[MSG_MAX], *p = t;
        put(&p, m->name); put(&p, " cannot go into "); put(&p, unit_name[u]); put(&p, ": "); put(&p, why ? why : "refused"); *p = 0;
        tell(t);
        return -1;
    }
    struct a65_disk_state st;
    if (!a65_apple_disk(M, u, &st)) m->changes_seen = st.changes;
    unit_img[u] = (int32_t)i; m->unit = (int32_t)u;
    kprintf("[A2] %s is in %s\n", m->name, unit_name[u]);
    return 0;
}
static void attach(uint32_t i, uint32_t u)
{
    if (i >= nimg) { tell("There is no such disk image."); return; }
    struct a2img *m = &img[i];
    if (img_open(m) < 0) { tell("That disk image could not be opened."); return; }
    if (u == A68A_UNIT_AUTO) {
        uint32_t a = m->kind == A68A_KIND_525 ? A68A_UNIT_D1 : A68A_UNIT_HD1, b = m->kind == A68A_KIND_525 ? A68A_UNIT_D2 : A68A_UNIT_HD4;
        u = a;
        for (uint32_t k = a; k <= b; k++) if (unit_img[k] < 0) { u = k; break; }
    }
    if (u >= A68A_UNITS) { tell("There is no such drive."); return; }
    if (m->unit == (int32_t)u) return;
    if (m->unit >= 0) { tell("That image is in another drive: eject it first."); return; }
    if (!M) { unit_img[u] = (int32_t)i; m->unit = (int32_t)u; disks_publish(); return; }   /* put in when the machine is built */
    int32_t old = unit_img[u];
    if (old >= 0) {                                   /* the one in the drive out, its changes on the card */
        flush_unit(u, 0);
        a65_apple_detach(M, u);
        flush_image(&img[old], 0);
        img[old].unit = -1; unit_img[u] = -1;
    }
    put_in(i, u);
    disks_publish();
}
static void detach(uint32_t u, uint32_t i)
{
    if (u == A68A_UNIT_AUTO) u = i < nimg && img[i].unit >= 0 ? (uint32_t)img[i].unit : A68A_UNITS;
    if (u >= A68A_UNITS || unit_img[u] < 0) return;
    struct a2img *m = &img[unit_img[u]];
    if (M) {
        flush_unit(u, 0);
        if (a65_apple_detach(M, u) < 0) kprintf("[A2] %s: writing back at the eject failed\n", m->name);
        flush_image(m, 0);
    }
    m->unit = -1; unit_img[u] = -1;
    kprintf("[A2] %s is out of %s\n", m->name, unit_name[u]);
    disks_publish();
}
/* the options' images, at the start: their units (a2.d1ro=on ...: read only); a2.img1= ... a2.img8= more in the list
   (the testbed's way: semihosting cannot list a host directory) */
static void disks_from_options(void)
{
    static const char *const key[A68A_UNITS] = { "a2.d1", "a2.d2", "a2.hd1", "a2.hd2", "a2.hd3", "a2.hd4" };
    static const char *const rokey[A68A_UNITS] = { "a2.d1ro", "a2.d2ro", "a2.hd1ro", "a2.hd2ro", "a2.hd3ro", "a2.hd4ro" };
    char v[128];
    for (uint32_t u = 0; u < A68A_UNITS; u++) {
        str_opt(v, sizeof v, key[u], "");
        if (!v[0]) continue;
        int k = img_add(v, 0);
        if (k < 0) continue;
        img[k].ro = img[k].ro_opt = is_on(rokey[u]);
        if (img_open(&img[k]) < 0) { kprintf("[A2] %s=%s could not be opened: the Apple starts without it\n", key[u], v); continue; }
        if (unit_img[u] < 0 && img[k].unit < 0) { unit_img[u] = k; img[k].unit = (int32_t)u; }
    }
    for (char k2[] = "a2.img1"; k2[6] <= '8'; k2[6]++) {
        str_opt(v, sizeof v, k2, "");
        if (!v[0]) continue;
        int k = img_add(v, 0);
        if (k >= 0 && img_open(&img[k]) < 0) kprintf("[A2] %s=%s could not be opened\n", k2, v);
    }
    rescan();
}

/* ---- the machine: its ROM read, identified, the machine built and its disks put in ---------------------------- */
static uint8_t *rom_mem, *p5_mem;
static uint32_t rom_len;
static int cur_model = -1;
static uint64_t start_t;
static int powered, paused, halted;
static uint32_t opts_now = ~0u;
static void pace_reset(void);
static int phase_start(void);

static int start_model(void) { return opt.model >= 0 ? opt.model : (int)A68A_MODEL_IIEE; }
/* the ROM for a model into rom_mem: 0, -2 while the card is not read yet, -1 (said why). With neither a2.model= nor
   a2.rom= the enhanced IIe's file is looked for first, then the others' (apple2e.rom, apple2plus.rom, ...): the machine
   is the one whose ROM the owner put there. A known ROM says which model it is; a2.model= given, the owner's word wins */
static int read_rom(int model, int *model_out)
{
    const char *name = opt.rom[0] && model == start_model() ? opt.rom : model_rom[model];
    struct pfile f;
    int r = pf_open(&f, name, 0);
    if (r == -2) return -2;
    if (r < 0 && opt.model < 0 && !opt.rom[0] && phase_start()) {
        static const int order[] = { A68A_MODEL_IIE, A68A_MODEL_IIPLUS, A68A_MODEL_II, A68A_MODEL_IIC };
        for (uint32_t i = 0; i < 4 && r < 0; i++) if ((r = pf_open(&f, model_rom[order[i]], 0)) == 0) { name = model_rom[order[i]]; model = order[i]; }
    }
    char t[MSG_MAX], *p = t;
    if (r < 0) {
        put(&p, "No ROM for the "); put(&p, model_name[model]); put(&p, ": your own machine's dump goes on the card as ");
        put(&p, name); put(&p, " (or a2.rom=)."); *p = 0;
        set_state(A68A_S_FAILED, A68A_E_NOROM, t);
        return -1;
    }
    if (f.size != 12288 && f.size != 16384 && f.size != 32768) {
        put(&p, name); put(&p, " is not an Apple II ROM: 12,288 bytes for the II and II+, 16,384 for the IIe and IIc."); *p = 0;
        set_state(A68A_S_FAILED, A68A_E_NOROM, t);
        return -1;
    }
    uint8_t *b = a65_host_alloc(f.size);
    if (!b) { set_state(A68A_S_FAILED, A68A_E_NOMEM, "No memory for the ROM."); return -1; }
    if (pf_read(&f, 0, b, f.size) < 0) { a65_host_free(b); set_state(A68A_S_FAILED, A68A_E_NOROM, "The ROM could not be read from the card."); return -1; }
    int id = rom_identify(b, f.size);
    kprintf("[A2] ROM %s: %s\n", name, id == -2 ? "Aurora65's own test ROM" : id >= 0 ? known[id].name : "not a ROM Aurora65's romcheck.py knows (used as it is)");
    log_sha(name, b, f.size);
    if (id >= 0 && known[id].model < 0) {
        a65_host_free(b);
        put(&p, name); put(&p, " is the "); put(&p, known[id].name);
        put(&p, ", a 32K IIc ROM: it needs the IIc's own ROM banking, which Aurora65 has not yet (the original IIc ROM, 255, works)."); *p = 0;
        set_state(A68A_S_FAILED, A68A_E_NOROM, t);
        return -1;
    }
    blk->rom_known = id == -2 ? 2u : id >= 0 ? 1u : 0u;
    if (id >= 0 && known[id].model != model) {
        if (opt.model < 0) model = known[id].model;
        else kprintf("[A2] the ROM is the %s's, a2.model= asks for the %s: built as asked\n", model_name[known[id].model], model_name[model]);
    }
    if (rom_mem) a65_host_free(rom_mem);
    rom_mem = b; rom_len = f.size;
    *model_out = model;
    return 0;
}
static void read_p5(void)
{
    if (p5_mem) return;
    struct pfile f;
    if (pf_open(&f, opt.p5, 0) < 0 || f.size != 256) { kprintf("[A2] no Disk II boot PROM (a2.p5=%s): Aurora65's stub, which boots only with Aurora65's test ROM\n", opt.p5); return; }
    p5_mem = a65_host_alloc(256);
    if (!p5_mem || pf_read(&f, 0, p5_mem, 256) < 0) { if (p5_mem) a65_host_free(p5_mem); p5_mem = 0; return; }
    kprintf("[A2] Disk II boot PROM %s: %s\n", opt.p5, crc32(p5_mem, 256) == 0xce7144f6u ? "the 16-sector P5 (341-0027)" : "not the P5 romcheck.py knows (used as it is)");
}
/* 0 built; -2 try again later (the card not read yet); -1 failed (said why) */
static int build(int model)
{
    int r = read_rom(model, &model);
    if (r) return r;
    read_p5();
    struct a65_apple_config cfg;
    a65_apple_defaults(&cfg, model);
    cfg.rom = rom_mem; cfg.rom_len = rom_len; cfg.p5 = p5_mem; cfg.audio_rate = AUDIO_RATE;
    const char *why = 0;
    uint64_t t0 = now();
    a65_apple *n = a65_apple_create(&cfg, &why);
    if (!n) {
        char t[MSG_MAX], *p = t;
        put(&p, "The "); put(&p, model_name[model]); put(&p, " could not be built: "); put(&p, why ? why : "no memory"); *p = 0;
        set_state(A68A_S_FAILED, A68A_E_BUILD, t);
        return -1;
    }
    M = n; cur_model = model; blk->model = (uint32_t)model;
    a65_host_free(rom_mem); rom_mem = 0;              /* the library keeps its own copy */
    opts_now = ~0u;
    for (uint32_t u = 0; u < A68A_UNITS; u++) {
        int32_t k = unit_img[u];
        if (k < 0) continue;
        unit_img[u] = -1; img[k].unit = -1;
        put_in((uint32_t)k, u);
    }
    disks_publish();
    kprintf("[A2] the %s is built in %u ms, %u KiB of the card's memory in use\n", model_name[model], us_since(t0) / 1000u,
            (uint32_t)((tlsf_get_total_size(pool) - tlsf_get_free_size(pool)) >> 10));
    powered = 1; paused = 0; halted = 0;
    pace_reset();
    set_state(A68A_S_RUNNING, 0, "Running.");
    return 0;
}
/* the machine away, its disks written back (a change of model) */
static void unbuild(void)
{
    if (!M) return;
    flush_all();
    for (uint32_t u = 0; u < A68A_UNITS; u++) if (unit_img[u] >= 0) { a65_apple_detach(M, u); flush_image(&img[unit_img[u]], 0); }
    a65_apple_destroy(M);
    M = 0;
}

/* ---- input ------------------------------------------------------------------------------------------- */
static uint32_t keys_in, joy_in;
static uint16_t mods_now;
static unsigned mouse_btn;
static uint64_t key_t;
static void buttons(void)
{
    a65_apple_buttons(M, ((mods_now & 0x0100) || mouse_btn ? 1u : 0u) | (mods_now & 0x0800 ? 2u : 0u));   /* cmdKey, optionKey */
}
static void release_all(void)
{
    if (!M) return;
    mods_now = 0; mouse_btn = 0;
    a65_apple_key_held(M, 0);
    a65_apple_mac_modifiers(M, 0);
    buttons();
}
static void input(void)
{
    static uint32_t focus_ack;
    uint32_t f = blk->focus;
    if (f != focus_ack) {
        if (!f) release_all();
        focus_ack = f;
    }
    uint32_t h = blk->in_head, t = blk->in_tail;
    if (h - t > A68A_IN_SLOTS) t = h - A68A_IN_SLOTS;  /* a ring overrun by the Mac: the oldest are lost */
    DMB();
    for (; t != h; t++) {
        const volatile struct a68a_in *e = (const volatile struct a68a_in *)(win + A68A_OFF_INPUT) + (t & (A68A_IN_SLOTS - 1));
        uint32_t type = e->type;
        if (!M || !powered || paused) continue;
        switch (type) {
        case A68A_IN_KEYDOWN:
            /* one key at a time, as the Apple's keyboard latch: the next waits until the program took this one (or
               KEY_WAIT_MS, after which it replaces it, as a real latch does) */
            if (!a65_apple_key_taken(M) && now() - key_t < ms_ticks(KEY_WAIT_MS)) goto out;
            mods_now = e->modifiers;
            if (a65_apple_mac_key(M, (uint8_t)e->dx, (uint8_t)e->code, e->modifiers) >= 0) { a65_apple_key_held(M, 1); keys_in++; key_t = now(); }
            buttons();
            break;
        case A68A_IN_KEYUP: a65_apple_key_held(M, 0); break;
        case A68A_IN_MODS: mods_now = e->modifiers; a65_apple_mac_modifiers(M, mods_now); buttons(); break;
        case A68A_IN_JOY:
            a65_apple_paddle(M, 0, (uint8_t)(e->dx < 0 ? 0 : e->dx > 255 ? 255 : e->dx));
            a65_apple_paddle(M, 1, (uint8_t)(e->dy < 0 ? 0 : e->dy > 255 ? 255 : e->dy));
            mouse_btn = e->buttons & 1; buttons(); joy_in++;
            break;
        case A68A_IN_RELEASE: release_all(); break;
        }
    }
out:
    DMB();
    blk->in_tail = t;
}

/* ---- drawing ------------------------------------------------------------------------------------------ */
static const uint8_t bayer[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 } };

static struct { uint32_t seq, screen, visible, rgn, vflags, scale, options; int32_t t, l, b, r, sx, sy; } g;   /* the place, as taken */
static struct pv { uint8_t *base; uint32_t w, h, rb, depth; const volatile uint32_t *clut; uint32_t clut_seq; int gray; } sf;
static uint8_t *ctab4k;               /* 12-bit colour -> the nearest entry of the screen's colour table (2-8 bits) */
static uint32_t ctab_seq = ~0u, ctab_depth;
/* the picture's layout: one bit (cells80: 80 columns from the cells, else the dots), its size at 1:1, the whole number
   it is scaled by on a deeper screen */
static struct { int one, cells80; uint32_t pw, ph, k; int32_t inset; } L;
static int need_full = 1;
static uint32_t last_key = ~0u;
static uint64_t draw_t, stat_t;
static struct { int32_t x0, y0, x1, y1; } dirty;
struct under { int on; int32_t x0, y0, x1, y1; };
static struct under under, again;     /* the pointer's box left undrawn (psmac_pc.c's way), and drawn again AGAIN_MS later */
static int32_t under_box[4];
static uint64_t again_t;
static uint32_t draw_us_sum, draws_n, draws_all, menu_skips, rgn_skips;

static void under_add(struct under *h, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 >= x1 || y0 >= y1) return;
    if (!h->on) { h->on = 1; h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1; return; }
    if (x0 < h->x0) h->x0 = x0;
    if (y0 < h->y0) h->y0 = y0;
    if (x1 > h->x1) h->x1 = x1;
    if (y1 > h->y1) h->y1 = y1;
}
static void mark(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 >= x1 || y0 >= y1) return;
    if (dirty.x0 >= dirty.x1) { dirty.x0 = x0; dirty.y0 = y0; dirty.x1 = x1; dirty.y1 = y1; return; }
    if (x0 < dirty.x0) dirty.x0 = x0;
    if (y0 < dirty.y0) dirty.y0 = y0;
    if (x1 > dirty.x1) dirty.x1 = x1;
    if (y1 > dirty.y1) dirty.y1 = y1;
}
static inline uint32_t lum(uint32_t c) { return (((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 151 + (c & 255) * 28) >> 8; }
static uint32_t nearest(uint32_t c, uint32_t n)
{
    uint32_t best = 0, bd = ~0u;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = sf.clut[i] & 0xffffffu;
        if (sf.gray) { uint32_t y = lum(p); p = y << 16 | y << 8 | y; }
        int dr = (int)((c >> 16) & 255) - (int)((p >> 16) & 255), dg = (int)((c >> 8) & 255) - (int)((p >> 8) & 255), db = (int)(c & 255) - (int)(p & 255);
        uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static inline void put_px(uint8_t *row, int32_t x, uint32_t rgb, int black)
{
    switch (sf.depth) {
    case 1: { uint8_t bit = (uint8_t)(0x80u >> (x & 7)); if (black) row[x >> 3] |= bit; else row[x >> 3] &= (uint8_t)~bit; break; }
    case 2: case 4: case 8: {
        uint32_t idx = ctab4k ? ctab4k[((rgb >> 20) & 15) << 8 | ((rgb >> 12) & 15) << 4 | ((rgb >> 4) & 15)] : 0;
        if (sf.depth == 8) row[x] = (uint8_t)idx;
        else if (sf.depth == 4) { uint32_t sh = x & 1 ? 0 : 4; row[x >> 1] = (uint8_t)((row[x >> 1] & ~(15u << sh)) | (idx & 15) << sh); }
        else { uint32_t sh = 6 - (uint32_t)(x & 3) * 2; row[x >> 2] = (uint8_t)((row[x >> 2] & ~(3u << sh)) | (idx & 3) << sh); }
        break;
    }
    case 16: ((uint16_t *)row)[x] = (uint16_t)(((rgb >> 19) & 31) << 10 | ((rgb >> 11) & 31) << 5 | ((rgb >> 3) & 31)); break;
    default: ((uint32_t *)row)[x] = rgb & 0xffffffu; break;
    }
}
/* 80 columns on one bit: the cell's dot at px, py of the 480 x 192 picture, in Aurora65's font at 6 x 8 (its glyphs are
   5 dots wide in the Apple's 7-dot cells: the sixth column is the space between letters) */
static inline int cell80(const struct a65_screen *s, uint32_t px, uint32_t py)
{
    uint32_t r = py >> 3, line = py & 7, c = px / 6, x = px % 6;
    uint16_t cell = s->cells[r * A65_SCREEN_COLS + c];
    uint32_t ch = cell & 0x7f, at = cell >> 8;
    int bit = 0;
    if (at & A68A_ATTR_MOUSETEXT) bit = x < 5 && line < 7 && ((x + line) & 1);   /* MouseText is not drawn yet (Aurora65): a checked box */
    else if (ch >= 0x20 && x < 5) bit = (a65_font[ch - 0x20][line] >> x) & 1;
    if ((at & A68A_ATTR_INVERSE) || ((at & A68A_ATTR_FLASH) && s->flash_on)) bit ^= 1;
    return bit;
}
/* the picture's luminance at px, py of the one-bit layout */
static inline uint32_t pic_lum(const struct a65_screen *s, uint32_t px, uint32_t py)
{
    if (L.cells80) return cell80(s, px, py) ? 255u : 0u;
    /* 280 wide: an Apple pixel is two dots; the second is the pixel itself whether or not its byte's bit 7 delays it */
    return lum(s->dots[py * A65_SCREEN_W + (L.pw == 280 ? px * 2 + 1 : px)]);
}
static void area(const struct a65_screen *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 >= x1 || y0 >= y1) return;
    int sense = (g.options & A68A_O_APPLESENSE) != 0;          /* white on black, as the Apple */
    uint32_t ww = (uint32_t)(g.r - g.l), wh = (uint32_t)(g.b - g.t);
    int fit = g.scale == A68A_SCALE_FIT;
    for (int32_t y = y0; y < y1; y++) {
        uint8_t *row = sf.base + (uint32_t)y * sf.rb;
        uint32_t wy = (uint32_t)(y - g.t);
        const uint8_t *bay = bayer[y & 7];
        for (int32_t x = x0; x < x1; x++) {
            uint32_t wx = (uint32_t)(x - g.l);
            if (!M || !powered) {                              /* no Apple: grey on one bit, black on deeper screens */
                put_px(row, x, 0, sf.depth == 1 ? (x ^ y) & 1 : 0);
                continue;
            }
            if (L.one) {
                uint32_t v = 0;
                int in;
                uint32_t px, py;
                if (fit) { px = wx * L.pw / ww; py = wy * L.ph / wh; in = 1; }
                else {
                    int32_t qx = (int32_t)wx - L.inset + g.sx, qy = (int32_t)wy + g.sy;
                    in = qx >= 0 && qy >= 0 && (uint32_t)qx < L.pw && (uint32_t)qy < L.ph;
                    px = (uint32_t)qx; py = (uint32_t)qy;
                }
                if (in) v = pic_lum(s, px, py);
                int lit = v > bay[x & 7] * 4u + 2u;
                put_px(row, x, 0, sense ? !lit : lit);
            } else {
                uint32_t px = (wx + (uint32_t)g.sx) / L.k, py = (wy + (uint32_t)g.sy) / (2u * L.k);
                uint32_t c = px < A65_SCREEN_W && py < A65_SCREEN_H ? s->dots[py * A65_SCREEN_W + px] : 0;
                put_px(row, x, c, 0);
            }
        }
    }
}
static void area_around(const struct a65_screen *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const int32_t *cb)
{
    if (!cb || cb[2] <= y0 || cb[0] >= y1 || cb[3] <= x0 || cb[1] >= x1) { area(s, x0, y0, x1, y1); return; }
    int32_t t = cb[0] > y0 ? cb[0] : y0, b = cb[2] < y1 ? cb[2] : y1, l = cb[1] > x0 ? cb[1] : x0, r = cb[3] < x1 ? cb[3] : x1;
    area(s, x0, y0, x1, t);
    area(s, x0, b, x1, y1);
    area(s, x0, t, l, b);
    area(s, r, t, x1, b);
    under_add(&under, l - g.l, t - g.t, r - g.l, b - g.t);
}
static void area_part(const struct a65_screen *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const int32_t *cb)
{
    if (!(g.options & A68A_O_GROWBOX)) { area_around(s, x0, y0, x1, y1, cb); return; }
    int32_t gt = g.b - 15, gl = g.r - 15;
    if (y0 < gt) area_around(s, x0, y0, x1, y1 < gt ? y1 : gt, cb);
    if (y1 > gt) area_around(s, x0, y0 > gt ? y0 : gt, x1 < gl ? x1 : gl, y1, cb);
}
static void geometry(void)
{
    uint32_t s1 = blk->geom_seq;
    if (s1 == g.seq) return;
    DMB();
    uint32_t screen = blk->screen, vis = blk->visible, scale = blk->scale, op = blk->options;
    int32_t t = blk->top, l = blk->left, b = blk->bottom, r = blk->right, sx = blk->scroll_x, sy = blk->scroll_y;
    int tagged = blk->clip_tag == A68A_CLIP_TAG + s1;
    uint32_t rgn = tagged ? blk->vis_rgn : 0, vf = blk->vis_flags;
    DMB();
    if (blk->geom_seq != s1) return;
    g.seq = s1; g.screen = screen; g.visible = vis; g.t = t; g.l = l; g.b = b; g.r = r; g.rgn = rgn; g.vflags = vf;
    g.scale = scale; g.options = op; g.sx = sx < 0 ? 0 : sx; g.sy = sy < 0 ? 0 : sy;
    need_full = 1;
}
/* the layout for this screen and the Apple's mode; the library drawing hires (and text, double hires) as a monochrome
   monitor would on one bit, unless the owner asked for colour; lores always in colour (dithered on one bit) */
static void layout(const struct a65_screen *s)
{
    L.one = sf.depth == 1;
    if (L.one) {
        L.cells80 = s->mode == A65_SCR_TEXT && s->cols == 80;
        L.pw = L.cells80 ? 480 : s->mode == A65_SCR_DHIRES ? 560 : 280;
        L.ph = 192; L.inset = INSET; L.k = 1;
        blk->nat_w = L.pw + 2u * INSET; blk->nat_h = 192;
    } else {
        L.cells80 = 0; L.pw = 560; L.ph = 384; L.inset = 0;
        uint32_t ww = (uint32_t)(g.r - g.l), wh = (uint32_t)(g.b - g.t), k = 1;
        if (g.scale == A68A_SCALE_FIT) { k = ww / 560 < wh / 384 ? ww / 560 : wh / 384; if (k < 1) k = 1; }
        L.k = k;
        blk->nat_w = 560; blk->nat_h = 384;
    }
    uint32_t want = L.one && s->mode != A65_SCR_LORES && !(g.options & A68A_O_COLOUR) ? A65_OPT_MONO : 0;
    if (want != opts_now) { a65_apple_options(M, want); opts_now = want; need_full = 1; }
    uint32_t key = (uint32_t)L.one | (uint32_t)L.cells80 << 1 | L.pw << 2 | L.k << 12 | (uint32_t)s->mode << 20;
    if (key != last_key) { last_key = key; need_full = 1; }
}
static void tables(void)
{
    if (sf.depth >= 2 && sf.depth <= 8 && (sf.clut_seq != ctab_seq || sf.depth != ctab_depth)) {
        uint32_t n = 1u << sf.depth;
        if (!ctab4k) ctab4k = a65_host_alloc(4096);
        if (ctab4k) for (uint32_t k = 0; k < 4096; k++) ctab4k[k] = (uint8_t)nearest((k >> 8) * 0x110000u | ((k >> 4) & 15) * 0x1100u | (k & 15) * 0x11u, n);
        ctab_seq = sf.clut_seq; ctab_depth = sf.depth;
    }
}
/* the text cells into the block, for programs that read them (and the host test) */
static void publish_text(const struct a65_screen *s)
{
    memcpy(win + A68A_OFF_TEXT, (const void *)s->cells, sizeof s->cells);
    blk->mode = s->mode; blk->cols = s->cols;
}
static int redraw_req;
static void draw(int force)
{
    uint64_t t0 = now();
    static const struct a65_screen blank;
    const struct a65_screen *s = M ? a65_apple_screen(M) : &blank;
    if (psmac_wclip_menu_down()) { menu_skips++; return; }    /* any program's menu over the screen: later */
    if (!g.visible || !psmac_display_surface((int)g.screen, &sf.base, &sf.w, &sf.h, &sf.rb, &sf.depth, &sf.clut, &sf.clut_seq, &sf.gray)) return;
    int32_t ww = g.r - g.l, wh = g.b - g.t;
    if (ww <= 0 || wh <= 0 || ww > 4096 || wh > 4096) return;
    tables();
    if (M) layout(s);
    if (force) need_full = 1;
    if (M && s->dirty_y1) {                                   /* the Apple's rows that changed, as window rows, whole width */
        int32_t y0 = s->dirty_y0, y1 = s->dirty_y1;
        if (!L.one) mark(0, (int32_t)(y0 * 2 * (int32_t)L.k) - g.sy, ww, (int32_t)(y1 * 2 * (int32_t)L.k) - g.sy);
        else if (g.scale == A68A_SCALE_FIT) mark(0, y0 * wh / 192, ww, (y1 * wh + 191) / 192 + 1);
        else mark(0, y0 - g.sy, ww, y1 - g.sy);
        ((struct a65_screen *)s)->dirty_y1 = 0;              /* aurora65.h: the reader clears it */
    }
    int32_t cbox[4];
    int okv;
    uint32_t vis = psmac_wclip_rd32(0x8CC, &okv) >> 24;      /* CrsrVis: no box while the pointer is hidden */
    const int32_t *cb = (!okv || vis) && psmac_wclip_cursor_box(g.screen, cbox) ? cbox : 0;
    if (need_full) under.on = again.on = 0;
    else {
        if (under.on && (!cb || cb[0] != under_box[0] || cb[1] != under_box[1] || cb[2] != under_box[2] || cb[3] != under_box[3])) {
            mark(under.x0, under.y0, under.x1, under.y1);
            under_add(&again, under.x0, under.y0, under.x1, under.y1);
            again_t = now() + ms_ticks(AGAIN_MS);
            under.on = 0;
        }
        if (again.on && now() >= again_t) { mark(again.x0, again.y0, again.x1, again.y1); again.on = 0; }
    }
    if (need_full) { dirty.x0 = 0; dirty.y0 = 0; dirty.x1 = ww; dirty.y1 = wh; }
    if (M) publish_text(s);
    if (dirty.x0 >= dirty.x1) return;
    int32_t cx0 = g.l + (dirty.x0 > 0 ? dirty.x0 : 0), cy0 = g.t + (dirty.y0 > 0 ? dirty.y0 : 0);
    int32_t cx1 = g.l + (dirty.x1 < ww ? dirty.x1 : ww), cy1 = g.t + (dirty.y1 < wh ? dirty.y1 : wh);
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > (int32_t)sf.w) cx1 = (int32_t)sf.w;
    if (cy1 > (int32_t)sf.h) cy1 = (int32_t)sf.h;
    dirty.x1 = dirty.x0;
    need_full = 0;
    if (cx0 < cx1 && cy0 < cy1) {
        if (!g.rgn) area_part(s, cx0, cy0, cx1, cy1, cb);
        else {
            int32_t rr[PSMAC_WCLIP_MAX][4];
            int n = psmac_wclip_parts(rr, g.rgn, g.vflags & A68A_VF_24BIT ? PSMAC_WCLIP_24BIT : 0, g.l, g.t, cx0, cy0, cx1, cy1);
            if (n < 0) { rgn_skips++; mark(cx0 - g.l, cy0 - g.t, cx1 - g.l, cy1 - g.t); return; }
            for (int k = 0; k < n; k++) area_part(s, rr[k][1], rr[k][0], rr[k][3], rr[k][2], cb);
        }
        DMB();
        if (under.on && cb) for (int k = 0; k < 4; k++) under_box[k] = cb[k];
    }
    blk->drawn_seq = s->seq;
    draw_us_sum += us_since(t0); draws_n++; draws_all++;
}

/* ---- the slice: the Apple's time as real time passes (Aurora65's DESIGN, "Each guest's ready and slice") ----------
   due: the Apple's microseconds not yet run, the real time since t_ref less what was given. Ready when there is at least
   a millisecond due (16 ms while the Apple is idle, so that an idle Apple is called a sixteenth as often with a longer
   piece each time). A slice runs what is due, at most RUN_MAX_US and at most what the core's slice can run at the
   measured rate; more than BEHIND_MAX_US behind (a busy shared core, a debugger stop) the rest is dropped: the Apple
   slows down rather than bursting. */
static uint64_t t_ref, given_us, dropped_us, busy_ticks, run_us_sum, idle_us_sum;
static uint32_t slice_max_us;
static uint32_t rate_x16 = 64;        /* the Apple's microseconds a microsecond of the core runs, x 16 (starts at 4) */
static void pace_reset(void) { t_ref = now(); given_us = 0; }
static int64_t due_us(uint64_t t) { return (int64_t)((t - t_ref) * 1000000u / hz()) - (int64_t)given_us; }
static int16_t pcm[2048];
static int snd_on;
static uint64_t snd_try_t;
static void sound(void)
{
    if (!snd_on) {
        if (now() - snd_try_t < hz()) { a65_apple_audio(M, pcm, sizeof pcm / sizeof pcm[0]); return; }   /* kept drained */
        snd_try_t = now();
        snd_on = psmac_audio_guest_open(AUDIO_RATE);
        if (snd_on) kprintf("[A2] the speaker plays through the Mac's sound output (%u Hz)\n", AUDIO_RATE);
    }
    for (;;) {
        uint32_t room = psmac_audio_guest_room();
        unsigned n = a65_apple_audio(M, pcm, room < sizeof pcm / sizeof pcm[0] ? room : (unsigned)(sizeof pcm / sizeof pcm[0]));
        if (!n) break;
        psmac_audio_guest_push(pcm, n);
        if (n < sizeof pcm / sizeof pcm[0]) break;
    }
}
static int a2_ready(uint64_t t)
{
    if (!M || !powered || paused || halted) return 0;
    return due_us(t) >= (a65_apple_idle(M) ? 16000 : 1000);
}
static int a2_slice(uint32_t budget_us)
{
    uint64_t t = now();
    int64_t d = due_us(t);
    if (d > (int64_t)BEHIND_MAX_US) {
        uint64_t drop = (uint64_t)d - RUN_MAX_US;
        t_ref += drop * hz() / 1000000u; dropped_us += drop;
        d = RUN_MAX_US;
    }
    if (d <= 0) return 0;
    uint32_t run = d > (int64_t)RUN_MAX_US ? RUN_MAX_US : (uint32_t)d;
    uint32_t cap = budget_us * rate_x16 / 16u;
    if (cap < 250) cap = 250;
    if (run > cap) run = cap;
    int idle0 = a65_apple_idle(M);
    int st = a65_apple_run(M, run);
    uint64_t dt = now() - t;
    busy_ticks += dt;
    given_us += run; run_us_sum += run; idle_us_sum += a65_apple_idle_us(M);
    if (given_us >= 1000000u) { given_us -= 1000000u; t_ref += hz(); }   /* the pace's origin a second on: no overflow */
    uint32_t us = (uint32_t)(dt * 1000000u / hz());
    if (us > slice_max_us) slice_max_us = us;
    if (!idle0 && !a65_apple_idle(M) && us > 50) {
        uint32_t r = run * 16u / us;
        if (r < 4) r = 4;
        if (r > 16u * 256u) r = 16u * 256u;
        rate_x16 = (rate_x16 * 7 + r + 7) / 8;
    }
    sound();
    if (st == A65_ST_HALTED && !halted) { halted = 1; set_state(A68A_S_HALTED, 0, "The Apple's processor stopped (a JAM instruction): Reset starts it again."); }
    return 1;
}

/* ---- commands ----------------------------------------------------------------------------------------- */
enum { P_IDLE, P_START, P_RUN, P_DEAD };
static int phase = P_IDLE;
static int want_model;
static int phase_start(void) { return phase == P_START; }
static void command(void)
{
    uint32_t seq = blk->cmd_seq;
    if (seq == blk->done_seq) return;
    DMB();
    uint32_t c = blk->cmd, a0 = blk->arg0, a1 = blk->arg1;
    switch (c) {
    case A68A_C_POWER_ON:
        if (phase == P_IDLE || phase == P_DEAD) { phase = P_START; start_t = now(); want_model = cur_model >= 0 ? cur_model : opt.model >= 0 ? opt.model : (int)A68A_MODEL_IIEE; set_state(A68A_S_STARTING, 0, "Starting..."); break; }
        if (M && !powered) { a65_apple_power(M, 1); powered = 1; paused = 0; halted = 0; pace_reset(); set_state(A68A_S_RUNNING, 0, "Running."); redraw_req = 1; }
        break;
    case A68A_C_POWER_OFF:                            /* every disk written back: then the Pi may be switched off */
        if (M && powered) { release_all(); flush_all(); a65_apple_power(M, 0); powered = 0; set_state(A68A_S_OFF, 0, "The Apple II is off."); redraw_req = 1; }
        break;
    case A68A_C_RESET:
        if (M && powered) {
            flush_all();
            a65_apple_reset(M, a0 == 1);
            paused = 0; halted = 0; pace_reset();
            set_state(A68A_S_RUNNING, 0, a0 == 1 ? "Running (restarted cold)." : "Running (reset).");
        }
        break;
    case A68A_C_PAUSE: if (M && powered && !paused) { paused = 1; set_state(A68A_S_PAUSED, 0, "Paused."); } break;
    case A68A_C_RESUME: if (M && powered && paused) { paused = 0; pace_reset(); set_state(A68A_S_RUNNING, 0, "Running."); } break;
    case A68A_C_REDRAW: redraw_req = 1; break;
    case A68A_C_ATTACH: attach(a0, a1); break;
    case A68A_C_DETACH: detach(a1, a0); break;
    case A68A_C_RESCAN: rescan(); break;
    case A68A_C_MODEL:
        if (a0 < A68A_MODELS && phase == P_RUN && (int)a0 != cur_model) {
            int was = cur_model;
            unbuild();
            if (build((int)a0) < 0) {                 /* that model has no ROM here: the one before again, and why */
                char why[128]; uint32_t i = 0;
                while (blk->message[i] && i < sizeof why - 1) { why[i] = blk->message[i]; i++; }
                why[i] = 0;
                if (build(was) < 0) phase = P_DEAD;
                else say(why);
            }
            redraw_req = 1;
        } else if (a0 < A68A_MODELS && phase != P_RUN) { opt.model = (int)a0; opt.rom[0] = 0; blk->model = a0; }
        break;
    }
    DMB();
    blk->done_seq = seq;
}

/* ---- the testbed's own eyes (testbed only): a2.testwin=SCREEN,LEFT,TOP,RIGHT,BOTTOM sets the place from the kernel
   (the card's drawing is seen before the Mac's program runs); a2.textlog=1 logs every changed line of the Apple's text
   screen, "[A2] | text" ------------------------------------------------------------------------------------------- */
static int textlog;
static uint32_t row_hash[A65_SCREEN_ROWS], textlog_seq;
static void test_options(void)
{
    const char *v = psmac_option("a2.testwin");
    if (v && v[0] >= '0' && v[0] <= '9') {
        int32_t n[5] = { 0, 0, 0, 0, 0 };
        for (int i = 0; i < 5 && *v; i++) {
            while (*v >= '0' && *v <= '9') n[i] = n[i] * 10 + (*v++ - '0');
            if (*v == ',') v++;
        }
        blk->screen = (uint32_t)n[0]; blk->left = n[1]; blk->top = n[2]; blk->right = n[3]; blk->bottom = n[4];
        blk->visible = 1;
        DMB();
        blk->geom_seq = blk->geom_seq + 1;
        kprintf("[A2] a2.testwin: the window at %d,%d-%d,%d on screen %d, set by the kernel\n", n[1], n[2], n[3], n[4], n[0]);
    }
    v = psmac_option("a2.textlog");
    textlog = v && v[0] == '1';
}
static void text_log(void)
{
    if (!textlog || !M) return;
    const struct a65_screen *s = a65_apple_screen(M);
    if (s->seq == textlog_seq) return;
    textlog_seq = s->seq;
    for (uint32_t r = 0; r < A65_SCREEN_ROWS; r++) {
        uint32_t h = 2166136261u, last = 0;
        char line[A65_SCREEN_COLS + 1];
        for (uint32_t c = 0; c < s->cols; c++) {
            uint32_t ch8 = s->cells[r * A65_SCREEN_COLS + c] & 0x7f;
            line[c] = ch8 >= 0x20 && ch8 < 0x7f ? (char)ch8 : ' ';
            if (line[c] != ' ') last = c + 1;
            h = (h ^ ch8) * 16777619u;
        }
        line[last] = 0;
        if (h != row_hash[r]) { row_hash[r] = h; if (last) kprintf("[A2] | %s\n", line); }
    }
}

/* ---- the guest (psmac_guest.c) ---------------------------------------------------------------------------- */
static uint32_t disk_writes_last;
static void numbers(void)
{
    uint64_t t = now();
    if (t - stat_t < hz()) return;
    uint64_t el = t - stat_t;
    stat_t = t;
    uint32_t el_us = (uint32_t)(el * 1000000u / hz());
    struct a65_apple_stats st = { 0, 0, 0, 0, 0 };
    if (M) a65_apple_stats(M, &st);
    blk->cycles_hi = (uint32_t)(st.cycles >> 32); blk->cycles_lo = (uint32_t)st.cycles;
    blk->speed_pm = el_us ? (uint32_t)(run_us_sum * 1000u / el_us) : 0;
    blk->idle_pm = el_us ? (uint32_t)(idle_us_sum * 1000u / el_us) : 0;
    blk->load = (uint32_t)(busy_ticks * 1000u / el);
    blk->idle = M ? (uint32_t)a65_apple_idle(M) : 0;
    blk->draw_us = draws_n ? draw_us_sum / draws_n : 0;
    blk->draws = draws_all;
    blk->disk_busy = io_writes - disk_writes_last; disk_writes_last = io_writes;
    blk->disk_dirty = dirty_units();
    uint32_t mot = 0;
    for (uint32_t u = 0; M && u < A68A_UNITS; u++) { struct a65_disk_state ds; if (!a65_apple_disk(M, u, &ds) && ds.motor) mot |= 1u << u; }
    blk->motor = mot;
    blk->core = (uint32_t)chosen;
    busy_ticks = 0; run_us_sum = idle_us_sum = 0; draw_us_sum = draws_n = 0;
}
static int my_core(void)
{
    if (opt.core) return (int)opt.core;
    return psmac_board_solo ? 2 : 1;         /* the PC card's rule: the solo board's core 2 is light, the SE's bus and the bridge's are not */
}
static int a2_core(void)
{
    if (!ready) return -1;
    if (chosen < 0) {
        int c = my_core(), expect = -1;
        if (__atomic_compare_exchange_n(&chosen, &expect, c, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            kprintf("[A2] the Apple II runs on core %d (%s)\n", c, opt.core ? "a2.core=" : psmac_board_solo ? "the solo board: core 2, beside the PC card if it is there"
                    : "slices on core 1, beside the PC card if it is there");
            stat_t = draw_t = now();
            test_options();
            phase = P_START;                  /* a2=on: the Apple starts with the Mac, as a card in a slot would */
            start_t = now();
            want_model = opt.model >= 0 ? opt.model : (int)A68A_MODEL_IIEE;
            set_state(A68A_S_STARTING, 0, "Starting...");
        }
    }
    return chosen;
}
static int a2_service(void)
{
    command();
    geometry();
    if (phase == P_START) {
        static int listed;
        if (!listed) { listed = 1; disks_from_options(); }
        int r = build(want_model);
        if (r == -2) {                         /* the SD card not looked at yet (core 3): again, for 20 s */
            if (us_since(start_t) > 20000000u) { set_state(A68A_S_FAILED, A68A_E_NOROM, "The SD card could not be read: no ROM."); phase = P_DEAD; }
            return 1;
        }
        phase = r ? P_DEAD : P_RUN;
        redraw_req = 1;
        return 1;
    }
    if (phase == P_RUN) {
        input();
        flush_poll();
        if (M) blk->screen_seq = a65_apple_screen(M)->seq;
    }
    if (ready && now() - draw_t >= hz() / DRAW_HZ) {
        draw_t = now();
        draw(redraw_req);
        redraw_req = 0;
    }
    text_log();
    numbers();
    return 0;
}
static int a2_front(void) { return ready && blk->front == 1; }
struct psmac_guest psmac_a2_guest = { "Apple II", a2_core, a2_service, a2_ready, a2_slice, a2_front, 0, 0, 0, 0 };

/* the Apple as its core's only guest (tools/a2card_test.c drives this, and psmac_guest_poll) */
int psmac_a2_poll(int core)
{
    if (a2_core() != core) return 0;
    if (a2_service()) return 1;
    return a2_ready(now()) ? a2_slice(core == 2 ? 2000u : 1000u) : 0;
}

void psmac_a2_report(void)
{
    if (!ready) return;
    static const char *const names[] = { "off", "starting", "running", "paused", "halted", "failed" };
    uint32_t st = blk->state, rd = 0, wr = 0, held_n = 0;
    for (uint32_t i = 0; i < nimg; i++) { rd += img[i].reads; wr += img[i].writes; held_n += img[i].held; }
    kprintf("[A2]   %s, the %s on core %d: %u per mille of its own speed, %u per mille of the core busy, %u of its time idle; %u pictures drawn (%u us each), %u skipped under a menu, %u for the region; keys %u; disks %u reads %u writes (%u sectors in memory, %u units to write back); %u ms dropped behind; %u.%02u x real time a core microsecond, slices up to %u us%s%s\n",
            st < 6 ? names[st] : "?", cur_model >= 0 ? model_name[cur_model] : "Apple II", chosen, blk->speed_pm, blk->load, blk->idle_pm,
            blk->draws, blk->draw_us, menu_skips, rgn_skips, keys_in, rd, wr, held_n, blk->disk_dirty, (uint32_t)(dropped_us / 1000u),
            rate_x16 / 16u, rate_x16 % 16u * 100u / 16u, slice_max_us, st == A68A_S_FAILED ? ": " : "", st == A68A_S_FAILED ? (const char *)blk->message : "");
}
