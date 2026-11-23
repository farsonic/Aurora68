/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_pc.c — the Aurora68 PC Card: a PC (Aurora86) on a Pi core, shown in a window on the Mac's desktop
 * (docs/19-pc-card.md; Aurora86's docs/DESIGN.md, "P3: the window").
 *
 * Backstory (23 November 2026). The owner asked for an x86 card ("Let's make an x86 card and also integrate
 * Libreboot"), and then: "Aurora86 needs to run in a window on the Mac desktop." Aurora86, a project of its own
 * beside this repository, made the PC: Bochs (a Pentium II on an i440FX board), its firmware Libreboot's QEMU x86
 * image (coreboot with SeaBIOS), built freestanding and big-endian with this kernel's own flags as libaurora86.a,
 * whose only outside names are six a86_host_* functions. This file is everything Macintosh about it, as
 * psmac_videocard.c is for the video decoder:
 *  - Its place: slot $D (psmb.h, PSMB_PC_*): a 1 MiB window at $FD000000 and $FDD00000 (the slot's 24-bit
 *    megabyte) holding the block of psmac_pc.h, and the declaration ROM at the slot's top (psmac_pc_declrom.h,
 *    tools/mac/mkpcdeclrom.py: board "Aurora68 PC Card", the driver ".A68PC", mac/a68pc.s). The MMU shim maps the
 *    three as it maps slot $C's.
 *  - Its memory: the window, the ROM page and the PC's own memory, carved from the Pi's RAM by psmac_bus.c beside
 *    the other cards' (psmac_pccard_size, _map): the kernel's heap is about 52 MB and the PC needs some 95 MB with
 *    its 16 MB of RAM (Bochs's RAM, ROM space, the VGA's memory, its decoded-instruction cache) plus the 12 MiB
 *    firmware file. That region is a TLSF pool of its own (a86_host_alloc); the few allocations Bochs's global
 *    objects make in their constructors, which the kernel's start-up runs before this region exists (start.c, the
 *    __init_start list), come from the kernel's heap.
 *  - Its core: core 2 where it is free, core 1 elsewhere (the owner's decision, 23 November 2026). On the solo
 *    board core 2 is the service core, but light: the heartbeat and the VIA's time (psmac_solo_service), so the PC
 *    runs there in slices of about 2 ms between the loop's turns. On the SE board (and the Plus family) core 2 is
 *    the SE's bus, which cannot wait, and on the IIci testbed it carries every I/O cycle over the bridge: there the
 *    PC runs on core 1 in slices of about 1 ms between the sound output's turns and the video decoder's
 *    (psmac_usb.c's loop). pc.core=1|2 chooses for a test. A PC that halts (HLT in an idle loop, the firmware
 *    waiting for a key) gives its slice back: Bochs's pacing ("clock: sync=both") asks to sleep, and the library
 *    hands that back as a86_pc_idle_us().
 *  - Its files: the firmware (pc.rom=, Libreboot's 12 MiB image as Aurora86 builds it), the VGA BIOS (pc.vgarom=,
 *    SeaVGABIOS for Bochs's VGA) and a disk image (pc.disk0=), on the SD card (psmac_card.c's FAT reader) or, in the
 *    testbed, host files through semihosting. The firmware is read into the PC's memory a megabyte per slice; the
 *    disk is read on demand, a few sectors at a time, through a86_disk()'s callbacks; what the PC writes is kept in
 *    memory (an overlay) and, since the second round of 23 November 2026, written back into the image file in place
 *    ("disks" below); CDs change while the PC runs (the program's Disks menu).
 *  - Its screen: drawn by this core into the Mac's own frame buffer (psmac_display.c's surfaces, as the video
 *    decoder draws), inside the window's content rectangle and only where its visRgn shows (psmac_wclip.c), never
 *    in the box around the Mac's pointer, never while a menu is down, at most 30 times a second and only what
 *    changed. Text modes are drawn from the cells: on a one-bit screen in a 6 x 13 font (the X Window System's
 *    public-domain misc-fixed, psmac_pc_font.h), 80 x 25 in 480 x 325, and on a screen too short for that in a
 *    document window (the SE's 512 x 342: 301 lines below the menu bar and the title) in its 6 x 12, 480 x 300; black on
 *    white (the lighter of a cell's two colours is the paper; A68P_O_PCSENSE for the PC's own sense); on deeper
 *    screens in the PC's own font from VGA plane 2, in its colours. Graphics modes are scaled to the window by the
 *    nearest pixel, dithered (8 x 8 ordered) on one bit, through the screen's colour table at 2-8 bits.
 *  - Its input: Mac virtual key codes and mouse movement from the program through the block's ring, turned into
 *    Bochs's key codes (the ADB extended keyboard's table below) and PS/2 mouse movement; when the PC loses the
 *    keyboard, every key it holds is let go.
 * Built only with -DPSMAC_PC=ON (emu68/CMakeLists.txt, which links libaurora86.a): the normal kernel has none of
 * this. pc=on (default off until it is proven) puts the card in slot $D and starts the PC with the Mac.
 */
#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "psmac.h"
#include "psmb.h"
#include "psmac_pc.h"
#include "psmac_wclip.h"
#include "psmac_guest.h"
#include "psmac_pc_declrom.h"
#include "psmac_pc_font.h"
#include "aurora86.h"

#ifdef PSMAC_PC_HOST
#define KERNEL_ALIAS 0
#else
#define KERNEL_ALIAS 0xffffff9000000000ull
#endif
extern const char *psmac_option(const char *key);
extern void psmac_mmu_set_pc(uintptr_t win, uintptr_t rom);
extern int psmac_display_surface(int i, uint8_t **pix, uint32_t *w, uint32_t *h, uint32_t *rb, uint32_t *depth,
                                 const volatile uint32_t **clut, uint32_t *clut_seq, int *gray);
extern int64_t psmac_sh_open(const char *name, uint64_t mode);
extern int psmac_sh_read(int64_t fd, void *buf, uint64_t len);
extern int64_t psmac_sh_seek(int64_t fd, uint64_t pos);
extern int64_t psmac_sh_flen(int64_t fd);
extern int psmac_sh_available(void);
extern int psmac_card_pc_open(const char *name, uint32_t *size);                 /* psmac_card.c */
extern int psmac_card_pc_read(int file, uint32_t off, void *buf, uint32_t n);
extern int psmac_card_pc_write(int file, uint32_t off, const void *buf, uint32_t n);
extern int psmac_card_pc_writable(void);
extern int psmac_card_pc_list(void (*cb)(void *ctx, const char *name, uint32_t size), void *ctx);
extern int psmac_sh_write(int64_t fd, const void *buf, uint64_t len);
extern int psmac_board_se, psmac_board_solo;

/* the C library's jump buffer as libaurora86's runtime keeps it (glibc's layout: 22 longs and a mask; room to spare) */
typedef uint64_t a86_jmp_buf[48];
extern int a86_setjmp(a86_jmp_buf env) __attribute__((returns_twice));
extern void a86_longjmp(a86_jmp_buf env, int v) __attribute__((noreturn));

#ifdef PSMAC_PC_HOST                         /* tools/pccard_test.c: this file on the host, with the host's Aurora86 library */
uint64_t pc_host_now(void);
static inline uint64_t now(void) { return pc_host_now(); }
static inline uint64_t hz(void)  { return 1000000u; }
#define DMB() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define WAIT() for (;;)
#else
static inline uint64_t now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
#define DMB() __asm__ volatile("dmb ish" ::: "memory")
#define WAIT() for (;;) __asm__ volatile("wfe")
#endif
static inline uint32_t us_since(uint64_t t) { return (uint32_t)((now() - t) * 1000000u / hz()); }

#define HEAP_FIXED_MB  104u          /* the PC's memory beyond its RAM: 95 MiB measured (Aurora86's harness, 16 MB of RAM:
                                        "the PC is built: 30 ms, 95 MiB of the heap in use" without the firmware), the
                                        12 MiB firmware file, and room for the write overlay */
#define SLICE_US_CORE2 2000u         /* a slice on the solo board's core 2: the VIA's time waits this long at most */
#define SLICE_US_CORE1 1000u         /* on core 1: the sound output's DMA ring (48 ms) is looked at between slices */
#define DRAW_HZ        30u
#define BLINK_MS       266u          /* the cursor's and blinking text's half period: the VGA's 16 frames at 60 Hz */
#define LOAD_CHUNK     (1u << 20)    /* the firmware read a megabyte per poll */
#define TEXT_INSET     2             /* pixels of paper between the window's frame and the text's first and last columns:
                                        misc-fixed's and the VGA's glyphs use their cell's first column (C, the box
                                        lines), which touched the window's one-pixel frame and read as a cut-off letter
                                        ("D-ROM not configured" in the first QEMU run, 23 November 2026) */
#define AGAIN_MS       100u          /* the pointer's old box drawn a second time this long after the first: the Mac puts
                                        back what was under its pointer at the VBL after Mouse ($830) moves, which can
                                        come after the card's picture of that place */

static struct {
    int on;                          /* pc=on */
    uint32_t ram_mb, core;           /* pc.ram=, pc.core= (0: by the board) */
    char rom[128], vgarom[128], disk0[128];
} opt;
static uintptr_t card_phys;
static volatile struct a68p_block *blk;
static uint8_t *win;
static void *pool;                   /* the PC's TLSF pool */
static uintptr_t pool_lo, pool_hi;
static volatile int ready;           /* psmac_pccard_init done */
static int chosen = -1;              /* the core it runs on, decided at the first poll */

/* ---- options --------------------------------------------------------------------------------------- */
static uint32_t num(const char *v, uint32_t dflt)
{
    if (!v || v[0] < '0' || v[0] > '9') return dflt;
    uint32_t n = 0;
    while (*v >= '0' && *v <= '9') n = n * 10 + (uint32_t)(*v++ - '0');
    return n;
}
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
    const char *v = psmac_option("pc");
    opt.on = v && v[0] == 'o' && v[1] == 'n';
    opt.ram_mb = num(psmac_option("pc.ram"), 16);
    if (opt.ram_mb < 4) opt.ram_mb = 4;
    if (opt.ram_mb > 256) opt.ram_mb = 256;
    opt.core = num(psmac_option("pc.core"), 0);
    if (opt.core != 1 && opt.core != 2) opt.core = 0;
    str_opt(opt.rom, sizeof opt.rom, "pc.rom", "aurora86.rom");
    str_opt(opt.vgarom, sizeof opt.vgarom, "pc.vgarom", "aurora86-vga.bin");
    str_opt(opt.disk0, sizeof opt.disk0, "pc.disk0", "");
}
static uint32_t heap_bytes(void) { return (opt.ram_mb + HEAP_FIXED_MB) << 20; }

uint32_t psmac_pccard_size(void)
{
    options();
    return opt.on ? A68P_WIN_SIZE + PSMB_PC_ROM_SIZE + heap_bytes() : 0;
}
void psmac_pccard_map(uintptr_t phys)
{
    card_phys = phys;
    psmac_mmu_set_pc(phys, phys + A68P_WIN_SIZE);
}

/* ---- what libaurora86 asks of the host (aurora86.h) --------------------------------------------------- */
static a86_jmp_buf fatal_env;
static int fatal_armed;
static char fatal_why[96];

void *a86_host_alloc(size_t size)
{
    if (pool) return tlsf_malloc_aligned(pool, size, 16);
    return tlsf_malloc_aligned(tlsf, size, 16);   /* Bochs's constructors, at the kernel's start, before the pool */
}
void a86_host_free(void *p)
{
    if (!p) return;
    if ((uintptr_t)p >= pool_lo && (uintptr_t)p < pool_hi) tlsf_free(pool, p);
    else tlsf_free(tlsf, p);
}
/* Bochs's log, a line at a time behind "[A86]" */
static char logline[160];
static uint32_t loglen;
void a86_host_log(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\n' || loglen == sizeof logline - 1) {
            logline[loglen] = 0;
            if (loglen) kprintf("[A86] %s\n", logline);
            loglen = 0;
            if (c == '\n') continue;
        }
        if (c != '\r') logline[loglen++] = c;
    }
}
uint64_t a86_host_ns(void)
{
    uint64_t v = now(), f = hz();
    return v / f * 1000000000ull + v % f * 1000000000ull / f;
}
/* the PC's clock starts from the Mac's (Time, $20C: seconds since 1904, local time), as the DOS cards' did */
int64_t a86_host_epoch(void)
{
    int ok;
    uint32_t t = psmac_wclip_rd32(0x20C, &ok);
    return ok && t > 2082844800u ? (int64_t)t - 2082844800 : 0;
}
void a86_host_fatal(const char *why)
{
    uint32_t i = 0;
    while (why && why[i] && i < sizeof fatal_why - 1) { fatal_why[i] = why[i]; i++; }
    fatal_why[i] = 0;
    kprintf("[PC] the PC stopped: %s\n", fatal_why);
    if (fatal_armed) a86_longjmp(fatal_env, 1);
    WAIT();                                       /* outside a call of ours (cannot happen): this core is lost */
}

/* ---- the block ------------------------------------------------------------------------------------- */
static void say(const char *s)
{
    uint32_t i = 0;
    while (s[i] && i < sizeof blk->message - 1) { blk->message[i] = s[i]; i++; }
    blk->message[i] = 0;
}
static void tell(const char *s) { say(s); kprintf("[PC] %s\n", s); }   /* the program shows it; the log keeps it */
static void set_state(uint32_t st, uint32_t err, const char *msg)
{
    blk->state = st; blk->error = err;
    if (msg) { say(msg); kprintf("[PC] %s\n", msg); }
}

void psmac_pccard_init(void)
{
    if (!card_phys) return;
    uint8_t *rom = (uint8_t *)(KERNEL_ALIAS + card_phys + A68P_WIN_SIZE);
    for (uint32_t i = 0; i < PSMB_PC_ROM_SIZE; i++) rom[i] = 0;
    memcpy(rom + PSMB_PC_ROM_SIZE - sizeof psmac_pc_declrom, psmac_pc_declrom, sizeof psmac_pc_declrom);   /* the format block at the slot's top */
    arm_flush_cache((uintptr_t)rom, PSMB_PC_ROM_SIZE);
    win = (uint8_t *)(KERNEL_ALIAS + card_phys);
    for (uint32_t i = 0; i < A68P_WIN_SIZE; i += 8) *(uint64_t *)(win + i) = 0;
    blk = (volatile struct a68p_block *)win;
    struct a68p_disks *dk = (struct a68p_disks *)(win + A68P_OFF_DISKS);
    for (uint32_t i = 0; i < A68P_PLACES; i++) dk->place[i] = -1;
    uint8_t *heap = (uint8_t *)(KERNEL_ALIAS + card_phys + A68P_WIN_SIZE + PSMB_PC_ROM_SIZE);
    pool = tlsf_init_with_memory(heap, heap_bytes());
    pool_lo = (uintptr_t)heap; pool_hi = pool_lo + heap_bytes();
    blk->win_size = A68P_WIN_SIZE;
    blk->ram_mb = opt.ram_mb;
    blk->magic = A68P_MAGIC;
    set_state(A68P_S_OFF, 0, "The PC is off.");
    DMB();
    blk->version = A68P_VERSION;
    arm_flush_cache((uintptr_t)win, 4096);
    ready = 1;
    kprintf("[PC] the PC card in slot $%X: window $%08X (24-bit $%06X), ROM %u bytes at the slot's top; %u MiB for the PC (%u MB of RAM); firmware %s, VGA BIOS %s, disk %s\n",
            PSMB_PC_SLOT, PSMB_PC_WIN_PHYS, PSMB_PC_SLOT << 20, (uint32_t)sizeof psmac_pc_declrom, heap_bytes() >> 20, opt.ram_mb,
            opt.rom, opt.vgarom, opt.disk0[0] ? opt.disk0 : "none");
}

/* ---- files: the SD card, or the testbed's host through semihosting ---------------------------------------- */
struct pfile { int sh; int64_t fd; int card; uint32_t size; int rw; };
/* rw: opened for writing too, where it can be (a host file "r+b", the card when it is writable); f->rw says */
static int pf_open(struct pfile *f, const char *name, int rw)
{
    f->sh = 0; f->fd = -1; f->card = -1; f->size = 0; f->rw = 0;
    if (!name[0]) return -1;
    if (psmac_sh_available()) {                   /* the testbed: a host path */
        int64_t fd = rw ? psmac_sh_open(name, 3) : -1;   /* semihosting's SYS_OPEN modes: 1 "rb", 3 "r+b" */
        if (fd >= 0) f->rw = 1;
        else fd = psmac_sh_open(name, 1);
        if (fd >= 0) { f->sh = 1; f->fd = fd; f->size = (uint32_t)psmac_sh_flen(fd); return 0; }
    }
    int c = psmac_card_pc_open(name, &f->size);   /* the SD card's root */
    if (c < 0) return c;
    f->card = c;
    f->rw = rw && psmac_card_pc_writable();
    return 0;
}
static int pf_read(struct pfile *f, uint32_t off, void *buf, uint32_t n)
{
    if (f->sh) {
        if (psmac_sh_seek(f->fd, off) < 0) return -1;
        return psmac_sh_read(f->fd, buf, n) ? 0 : -1;
    }
    return psmac_card_pc_read(f->card, off, buf, n);
}
static int pf_write(struct pfile *f, uint32_t off, const void *buf, uint32_t n)
{
    if (!f->rw) return -1;
    if (f->sh) {
        if (psmac_sh_seek(f->fd, off) < 0) return -1;
        return psmac_sh_write(f->fd, buf, n) ? 0 : -1;
    }
    return psmac_card_pc_write(f->card, off, buf, n);
}

/* ---- the PC's start, a step per poll --------------------------------------------------------------------- */
enum { P_IDLE, P_ROM, P_VGA, P_BUILD, P_RUN, P_DEAD };
static int phase = P_IDLE;
static struct pfile romf;
static uint8_t *rom_mem, *vga_mem;
static uint32_t rom_got, vga_size;
static int powered;                   /* the PC exists and is not powered off */
static int paused;
static char bochsrc[2048];
static uint64_t start_t;

static void fmt_u(char **p, uint32_t v) { char t[12]; int k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) *(*p)++ = t[--k]; }
static void put(char **p, const char *s) { while (*s) *(*p)++ = *s++; }
/* ---- disks ---------------------------------------------------------------------------------------------------
   The PC's disks are image files: hard disks (IDE 0 master and slave, pc.disk0= and pc.disk1=) and a CD-ROM drive (IDE
   1 master, pc.cd=, ISO images); floppy images are listed but cannot be used (make_bochsrc says why). Their sectors are read from the
   file on demand; what the PC writes goes into memory first (an overlay, by sector) and from there back into the file.
   Until 23 November 2026 it stayed in memory and was lost when the Pi stopped; now it is written back, in place, into
   the file's own clusters (psmac_card_pc_write: no size change, no directory or FAT sector touched, as PiDisk's Mac
   images are written), or through semihosting into the host's file in the testbed:
    - when the PC has written nothing to that disk for FLUSH_IDLE_MS (DOS's writes come in bursts: a file is a few
      sectors of FAT, directory and data);
    - when the oldest sector not yet written back is FLUSH_MAX_MS old (a PC that never stops writing);
    - at once, all of it, when the program powers the PC off or resets it (PC > Power Off is how the owner makes sure
      everything is on the card before the Pi is switched off), and when an image leaves its drive.
   Writing back happens on the PC's own core between slices, FLUSH_SLICE_US at most per turn unless everything is asked
   for; sectors written back leave the overlay (the file has them now). pc.disk0ro=on (pc.disk1ro=on) keeps a hard
   disk's file as it is: its writes stay in memory, as before. A CD is always read only. A write that fails stops the
   writing back for that disk (its changes stay in memory) and says so in the log and the block's message.
   A CD can be changed while the PC runs (the program's Disks menu: A68P_C_ATTACH, _DETACH, through
   Aurora86's a86_pc_media); hard disks are fixed while the PC exists, as in a real PC. The image list is the options'
   files and, on the SD card, the root's files that look like PC images (pc_ext below; .img is the Mac's: psmac_card.c
   serves every .img in the root to the Mac except the ones the PC's options name). */
#define OV_BUCKETS     4096u
#define FLUSH_IDLE_MS  2000u
#define FLUSH_MAX_MS   10000u
#define FLUSH_SLICE_US 4000u
#define FLUSH_RUN      64u            /* sectors per write at most: 32 KB, a cluster on most cards */
#define PLACE_AUTO     A68P_PLACE_AUTO
#define P_IDE0M 0                     /* A68P_PLACE_*, as signed numbers */
#define P_IDE0S 1
#define P_CD    2
#define P_FDA   4
#define P_FDB   5
struct ovsec { struct ovsec *next; uint32_t lba, dirty; uint8_t data[512]; };
struct pimg {
    char name[64];                    /* as the list and the Disks menu show it */
    char path[128];                   /* as opened: a host path in the testbed, a name in the card's root */
    char a86name[16];                 /* the file's name for Bochs (a86_disk) */
    struct pfile f;
    int open, ro, ro_opt, failed, registered;      /* ro: its file is never written; ro_opt: by pc.diskNro=on */
    uint32_t size, sectors, kind;
    int32_t place;
    struct ovsec **ov;                /* the overlay's buckets, made at the first write */
    uint32_t held, dirty;             /* sectors in memory; of those, not yet written back */
    uint32_t reads, writes, flushed, cursor;
    uint64_t first_dirty_t, last_write_t, flush_t0;
};
static struct pimg img[A68P_DISKS_MAX];
static uint32_t nimg;
static int32_t place_img[A68P_PLACES] = { -1, -1, -1, -1, -1, -1 };
static uint32_t io_count;
static uint8_t runbuf[FLUSH_RUN * 512] __attribute__((aligned(16)));
static int pc_built(void);

static struct ovsec *ov_find(struct pimg *m, uint32_t lba)
{
    if (!m->ov) return 0;
    for (struct ovsec *s = m->ov[lba % OV_BUCKETS]; s; s = s->next)
        if (s->lba == lba) return s;
    return 0;
}
static void ov_drop(struct pimg *m, struct ovsec *s)
{
    struct ovsec **pp = &m->ov[s->lba % OV_BUCKETS];
    while (*pp && *pp != s) pp = &(*pp)->next;
    if (*pp) *pp = s->next;
    a86_host_free(s);
    m->held--;
}
static int disk_rd(void *ctx, void *buf, uint64_t off, size_t n)
{
    struct pimg *m = ctx;
    uint8_t *b = buf;
    io_count++; m->reads++;
    if (!m->open || off > m->size || n > m->size - off) return -1;
    while (n) {
        uint32_t lba = (uint32_t)(off >> 9), in = (uint32_t)(off & 511), k = 512 - in;
        if (k > n) k = (uint32_t)n;
        struct ovsec *s = ov_find(m, lba);
        if (s) memcpy(b, s->data + in, k);
        else {
            /* runs of sectors not in the overlay in one read */
            uint32_t run = k;
            while (run < n && !ov_find(m, (uint32_t)((off + run) >> 9)) && run < (64u << 10)) run += (uint32_t)(n - run < 512 ? n - run : 512);
            if (pf_read(&m->f, (uint32_t)off, b, run) < 0) return -1;
            k = run;
        }
        b += k; off += k; n -= k;
    }
    return 0;
}
static int disk_wr(void *ctx, void *buf, uint64_t off, size_t n)
{
    struct pimg *m = ctx;
    const uint8_t *b = buf;
    io_count++; m->writes++;
    if (!m->open || off > m->size || n > m->size - off) return -1;
    if (!m->ov) {
        m->ov = a86_host_alloc(OV_BUCKETS * sizeof *m->ov);
        if (!m->ov) return -1;
        for (uint32_t i = 0; i < OV_BUCKETS; i++) m->ov[i] = 0;
    }
    uint64_t t = now();
    while (n) {
        uint32_t lba = (uint32_t)(off >> 9), in = (uint32_t)(off & 511), k = 512 - in;
        if (k > n) k = (uint32_t)n;
        struct ovsec *s = ov_find(m, lba);
        if (!s) {
            s = a86_host_alloc(sizeof *s);
            if (!s) return -1;
            if (in || k < 512) { if (pf_read(&m->f, lba << 9, s->data, 512) < 0) { a86_host_free(s); return -1; } }
            s->lba = lba; s->dirty = 0; s->next = m->ov[lba % OV_BUCKETS]; m->ov[lba % OV_BUCKETS] = s;
            m->held++;
        }
        memcpy(s->data + in, b, k);
        if (!s->dirty && !m->ro && !m->failed) {          /* to be written back */
            if (!m->dirty) m->first_dirty_t = t;
            s->dirty = 1; m->dirty++;
        }
        b += k; off += k; n -= k;
    }
    m->last_write_t = t;
    return 0;
}

/* writing back: the overlay's sectors that changed, in runs of consecutive sectors, until there are none or until the
   deadline (0: none) */
static void flush_image(struct pimg *m, uint64_t deadline)
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
            kprintf("[PC] %s: writing sectors %u-%u back failed: its changes stay in memory from now on (%u sectors)\n",
                    m->name, lba, lba + n - 1, m->held);
            say("A disk's changes could not be written back to the card: they stay in memory (the kernel log says which).");
            m->dirty = 0;
            return;
        }
        for (uint32_t k = 0; k < n; k++) ov_drop(m, run[k]);
        m->dirty -= n; m->flushed += n;
    }
    if (!m->dirty && m->flushed) {
        kprintf("[PC] %s: %u sector%s written back to the %s (%u ms)\n", m->name, m->flushed, m->flushed == 1 ? "" : "s",
                m->f.sh ? "host's file" : "card", us_since(m->flush_t0) / 1000u);
        m->flushed = 0;
    }
}
/* all: everything now (power off, reset, eject), else what is due, FLUSH_SLICE_US at most */
static void flush_poll(int all)
{
    uint64_t t = now(), deadline = all ? 0 : t + (uint64_t)FLUSH_SLICE_US * hz() / 1000000u;
    for (uint32_t i = 0; i < nimg; i++) {
        struct pimg *m = &img[i];
        if (!m->dirty) continue;
        if (!all && t - m->last_write_t < (uint64_t)FLUSH_IDLE_MS * hz() / 1000u && t - m->first_dirty_t < (uint64_t)FLUSH_MAX_MS * hz() / 1000u) continue;
        if (!m->flushed) m->flush_t0 = t;
        flush_image(m, deadline);
        if (deadline && now() >= deadline) break;
    }
}
static uint32_t dirty_total(void) { uint32_t n = 0; for (uint32_t i = 0; i < nimg; i++) n += img[i].dirty; return n; }

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
static uint32_t kind_of(const char *name, uint32_t size)
{
    if (ends_ci(name, ".iso")) return A68P_KIND_CD;
    static const uint32_t fsz[] = { 163840, 184320, 327680, 368640, 737280, 1228800, 1474560, 1720320, 2949120 };   /* 160K-2.88M */
    for (uint32_t i = 0; i < sizeof fsz / sizeof fsz[0]; i++) if (size == fsz[i]) return A68P_KIND_FLOPPY;
    if (ends_ci(name, ".ima") || ends_ci(name, ".flp") || ends_ci(name, ".vfd")) return A68P_KIND_FLOPPY;
    return A68P_KIND_HD;
}
/* a PC image in the card's root by its name alone (.img is the Mac's unless an option names it) */
static int pc_ext(const char *name)
{
    return ends_ci(name, ".ima") || ends_ci(name, ".flp") || ends_ci(name, ".vfd") || ends_ci(name, ".iso") ||
           ends_ci(name, ".vhd") || ends_ci(name, ".hdd");
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
/* into the list (by its path; size 0: found when it is opened), its index or -1 */
static int img_add(const char *path, uint32_t size)
{
    for (uint32_t i = 0; i < nimg; i++) if (same(img[i].path, path)) return (int)i;
    if (!path[0] || nimg >= A68P_DISKS_MAX) return -1;
    struct pimg *m = &img[nimg];
    uint32_t i = 0;
    while (path[i] && i < sizeof m->path - 1) { m->path[i] = path[i]; i++; }
    m->path[i] = 0;
    const char *n = path;
    for (const char *q = path; *q; q++) if (*q == '/') n = q + 1;
    for (i = 0; n[i] && i < sizeof m->name - 1; i++) m->name[i] = n[i];
    m->name[i] = 0;
    char *p = m->a86name;
    put(&p, "/a86/d"); fmt_u(&p, nimg); put(&p, ".img"); *p = 0;
    m->size = size; m->sectors = size / 512u; m->kind = kind_of(m->name, size); m->place = -1;
    return (int)nimg++;
}
/* opened (for writing where it may be written), 0; or -1 */
static int img_open(struct pimg *m)
{
    if (m->open) return 0;
    if (pf_open(&m->f, m->path, !m->ro && m->kind != A68P_KIND_CD) < 0) return -1;
    m->size = m->f.size; m->sectors = m->size / 512u;
    m->kind = kind_of(m->name, m->size);
    if (m->kind == A68P_KIND_CD || !m->f.rw) m->ro = 1;
    if (!m->registered) {
        uint64_t sz = m->kind == A68P_KIND_HD ? (uint64_t)(m->sectors / (16u * 63u)) * 16u * 63u * 512u : m->size;
        if (a86_disk(m->a86name, sz, disk_rd, m->kind == A68P_KIND_CD ? 0 : disk_wr, m) < 0) return -1;
        m->registered = 1;
    }
    m->open = 1;
    kprintf("[PC] %s: %u KB, %s%s\n", m->name, m->size >> 10, m->kind == A68P_KIND_CD ? "a CD" : m->kind == A68P_KIND_FLOPPY ? "a floppy" : "a hard disk",
            m->kind == A68P_KIND_CD ? "" : m->f.rw && !m->ro ? ", its changes written back to it" :
            m->ro_opt ? ", kept as it is (pc.disk0ro=on or pc.disk1ro=on): its changes stay in memory" : ", read only here: its changes stay in memory");
    return 0;
}
/* the list as the Mac reads it (struct a68p_disks) */
static void disks_publish(void)
{
    struct a68p_disks *dk = (struct a68p_disks *)(win + A68P_OFF_DISKS);
    for (uint32_t i = 0; i < nimg; i++) {
        uint32_t k = 0;
        while (img[i].name[k] && k < sizeof dk->d[i].name - 1) { dk->d[i].name[k] = img[i].name[k]; k++; }
        dk->d[i].name[k] = 0;
        dk->d[i].sectors = img[i].sectors; dk->d[i].kind = img[i].kind;
        dk->d[i].flags = img[i].ro ? A68P_DF_RO : 0; dk->d[i].place = img[i].place;
    }
    for (uint32_t p = 0; p < A68P_PLACES; p++) dk->place[p] = place_img[p];
    DMB();
    dk->count = nimg;
    dk->seq = dk->seq + 1;
}
/* the SD card's root: its PC images (pc_ext) into the list */
static void card_list_cb(void *ctx, const char *name, uint32_t size)
{
    (void)ctx;
    if (pc_ext(name) && size >= 512) img_add(name, size);
}
static void rescan(void)
{
    psmac_card_pc_list(card_list_cb, 0);                /* -1, nothing added, where there is no card */
    disks_publish();
}
/* the options' images, at the start: their places (pc.disk0ro=on / pc.disk1ro=on: kept as they are) */
static void disks_from_options(void)
{
    static const struct { const char *key, *ro; int32_t place; } o[] = {
        { "pc.disk0", "pc.disk0ro", P_IDE0M }, { "pc.disk1", "pc.disk1ro", P_IDE0S }, { "pc.cd", 0, P_CD } };
    char v[128];
    for (uint32_t i = 0; i < sizeof o / sizeof o[0]; i++) {
        str_opt(v, sizeof v, o[i].key, "");
        if (!v[0]) continue;
        int k = img_add(v, 0);
        if (k < 0) continue;
        struct pimg *m = &img[k];
        if (o[i].ro) { const char *r = psmac_option(o[i].ro); m->ro = m->ro_opt = r && r[0] == 'o' && r[1] == 'n'; }
        if (img_open(m) < 0) { kprintf("[PC] %s=%s could not be opened: the PC starts without it\n", o[i].key, v); continue; }
        uint32_t want = o[i].place == P_CD ? A68P_KIND_CD : o[i].place >= P_FDA ? A68P_KIND_FLOPPY : A68P_KIND_HD;
        if (m->kind != want || (want == A68P_KIND_HD && m->sectors < 16u * 63u)) {
            kprintf("[PC] %s=%s is not a %s image: not attached\n", o[i].key, v, want == A68P_KIND_CD ? "CD" : want == A68P_KIND_FLOPPY ? "floppy" : "hard disk");
            continue;
        }
        if (place_img[o[i].place] < 0 && m->place < 0) { place_img[o[i].place] = k; m->place = o[i].place; }
    }
    /* pc.img1= ... pc.img8=: more images in the list, one an option (the testbed's way: semihosting cannot list a
       directory, and an option's value is 127 characters at most, too few for two of the testbed's paths) */
    for (char key[] = "pc.img1"; key[6] <= '8'; key[6]++) {
        str_opt(v, sizeof v, key, "");
        if (!v[0]) continue;
        int k = img_add(v, 0);
        if (k >= 0 && img_open(&img[k]) < 0) kprintf("[PC] %s=%s could not be opened\n", key, v);
    }
    rescan();
}
static int unit_of(int32_t place) { return place == P_FDA ? 0 : place == P_FDB ? 1 : place == P_CD ? 2 : -1; }
static const char *place_name(int32_t p) { return p == P_FDA ? "A:" : p == P_FDB ? "B:" : p == P_CD ? "the CD-ROM drive" : p == P_IDE0M ? "IDE 0 master" : "IDE 0 slave"; }
/* the Disks menu's choice: an image into a drive (PLACE_AUTO: its kind's), or out of one */
static void attach(uint32_t i, uint32_t place)
{
    if (i >= nimg) { tell("There is no such disk image."); return; }
    struct pimg *m = &img[i];
    if (img_open(m) < 0) { tell("That disk image could not be opened."); return; }
    if (m->kind == A68P_KIND_HD) {
        tell(m->place >= 0 ? "A hard disk stays while the PC exists, as in a real PC." :
            "Hard disks are chosen when the PC is built: pc.disk0= and pc.disk1= in cmdline.txt.");
        return;
    }
    if (m->kind == A68P_KIND_FLOPPY) {
        tell("The PC has no floppy drive: Libreboot's firmware does not offer one. A CD image (.iso) goes into its CD-ROM drive.");
        return;
    }
    int32_t p = (int32_t)place;
    if (place == PLACE_AUTO) p = P_CD;
    if ((m->kind == A68P_KIND_CD) != (p == P_CD) || unit_of(p) < 0) { tell("That image does not fit that drive."); return; }
    if (m->place == p) return;
    if (m->place >= 0) { tell("That image is in another drive: eject it first."); return; }
    int32_t old = place_img[p];
    if (old >= 0) { flush_image(&img[old], 0); img[old].place = -1; }   /* the one in the drive out, its changes on the card */
    if (pc_built() && a86_pc_media((unsigned)unit_of(p), m->a86name) < 0) {
        place_img[p] = -1;
        kprintf("[PC] %s could not go into %s\n", m->name, place_name(p));
        tell("The PC's drive did not take that image (the kernel log says more).");
        disks_publish();
        return;
    }
    place_img[p] = (int32_t)i; m->place = p;
    kprintf("[PC] %s is in %s\n", m->name, place_name(p));
    disks_publish();
}
static void detach(uint32_t place, uint32_t i)
{
    int32_t p = place == PLACE_AUTO ? (i < nimg ? img[i].place : -1) : (int32_t)place;
    if (p < 0 || p >= (int32_t)A68P_PLACES || place_img[p] < 0) return;
    if (unit_of(p) < 0) { tell("A hard disk stays while the PC exists, as in a real PC."); return; }
    struct pimg *m = &img[place_img[p]];
    if (pc_built()) a86_pc_media((unsigned)unit_of(p), 0);
    flush_image(m, 0);
    m->place = -1; place_img[p] = -1;
    kprintf("[PC] %s is out of %s\n", m->name, place_name(p));
    disks_publish();
}

static void make_bochsrc(void)
{
    char *p = bochsrc;
    put(&p, "display_library: a86\nmegs: "); fmt_u(&p, opt.ram_mb);
    put(&p, "\nromimage: file=/a86/pc.rom\nvgaromimage: file=/a86/vgabios.bin\n"
            "vga: extension=vbe, update_freq=25, realtime=1\n"
            "pci: enabled=1, chipset=i440fx, slot1=pcivga\n"
            "plugin_ctrl: fw_cfg=1\nfw_cfg: enabled=1, acpi_tables=0\n"
            "cpu: model=p2_klamath, ips=40000000\n"   /* the most the pacing lets it run: above the Pi 3B's estimated 9-32 MIPS (Aurora86 STATUS item 16) */
            "clock: sync=both, time0=utc\n"
            "com1: enabled=1, mode=null\n"
            "log: -\npanic: action=fatal\nerror: action=report\ninfo: action=ignore\ndebug: action=ignore\n"
            "keyboard: type=mf, serial_delay=150\nmouse: enabled=1, type=ps2\n");
    /* the hard disks (16 heads, 63 sectors a track: what DOS's FDISK and SeaBIOS agree on for these sizes); the CD-ROM
       drive always there, empty unless pc.cd= filled it, so that the Disks menu can. No floppy drives: Libreboot's
       SeaBIOS (a coreboot build) knows only the floppy drives coreboot's CBFS lists ("etc/floppy0", SeaBIOS's
       hw/floppy.c), Libreboot's QEMU image lists none, and DOS reads floppies through the BIOS: a drive configured here
       took the image (a86_pc_media) and DOS answered "unknown command given to driver" (23 November 2026). SeaBIOS
       ignores the CMOS boot order under coreboot: it tries the CD-ROM drive, then the hard disk */
    put(&p, "ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14\n");
    for (int32_t pl = P_IDE0M; pl <= P_IDE0S; pl++) {
        if (place_img[pl] < 0) continue;
        const struct pimg *m = &img[place_img[pl]];
        put(&p, pl == P_IDE0M ? "ata0-master" : "ata0-slave");
        put(&p, ": type=disk, path="); put(&p, m->a86name); put(&p, ", mode=flat, cylinders=");
        fmt_u(&p, m->sectors / (16u * 63u));
        put(&p, ", heads=16, spt=63\n");
    }
    put(&p, "ata1: enabled=1, ioaddr1=0x170, ioaddr2=0x370, irq=15\nata1-master: type=cdrom, path=");
    put(&p, place_img[P_CD] >= 0 ? img[place_img[P_CD]].a86name : "none");
    put(&p, place_img[P_CD] >= 0 ? ", status=inserted\nboot: cdrom, disk\n" : ", status=ejected\nboot: disk, cdrom\n");
    *p = 0;
}

static void start_step(void)
{
    switch (phase) {
    case P_ROM:
        if (!rom_mem) {
            int r = pf_open(&romf, opt.rom, 0);
            if (r == -2 && us_since(start_t) < 20000000u) return;          /* the SD card not looked at yet (core 3) */
            if (r < 0 || romf.size < 65536 || romf.size > (16u << 20)) {
                set_state(A68P_S_FAILED, A68P_E_NOROM, "The PC's firmware (pc.rom=) is not on the card.");
                phase = P_DEAD; return;
            }
            rom_mem = a86_host_alloc(romf.size);
            if (!rom_mem) { set_state(A68P_S_FAILED, A68P_E_NOMEM, "No memory for the PC's firmware."); phase = P_DEAD; return; }
            rom_got = 0;
            set_state(A68P_S_STARTING, 0, "Reading the PC's firmware...");
        }
        {
            uint32_t n = romf.size - rom_got < LOAD_CHUNK ? romf.size - rom_got : LOAD_CHUNK;
            if (pf_read(&romf, rom_got, rom_mem + rom_got, n) < 0) {
                set_state(A68P_S_FAILED, A68P_E_NOROM, "The PC's firmware could not be read.");
                phase = P_DEAD; return;
            }
            rom_got += n;
            if (rom_got == romf.size) phase = P_VGA;
        }
        return;
    case P_VGA: {
        struct pfile f;
        if (pf_open(&f, opt.vgarom, 0) < 0 || f.size < 1024 || f.size > (256u << 10) || !(vga_mem = a86_host_alloc(f.size)) ||
            pf_read(&f, 0, vga_mem, f.size) < 0) {
            set_state(A68P_S_FAILED, A68P_E_NOROM, "The PC's VGA BIOS (pc.vgarom=) is not on the card.");
            phase = P_DEAD; return;
        }
        vga_size = f.size;
        disks_from_options();
        phase = P_BUILD;
        return;
    }
    case P_BUILD: {
        uint64_t t0 = now();
        a86_blob("/a86/pc.rom", rom_mem, romf.size, 0);
        a86_blob("/a86/vgabios.bin", vga_mem, vga_size, 0);
        make_bochsrc();
        set_state(A68P_S_STARTING, 0, "Building the PC...");
        fatal_armed = 1;
        if (a86_setjmp(fatal_env)) {
            fatal_armed = 0;
            set_state(A68P_S_FAILED, A68P_E_BOCHS, fatal_why);
            phase = P_DEAD; return;
        }
        int r = a86_pc_start(bochsrc);
        fatal_armed = 0;
        if (r < 0) { set_state(A68P_S_FAILED, A68P_E_BOCHS, "The PC could not be built (the kernel log says why)."); phase = P_DEAD; return; }
        kprintf("[PC] the PC is built in %u ms: %u MB of RAM, %u MiB of its memory in use, firmware %u KiB, VGA BIOS %u KiB%s\n",
                us_since(t0) / 1000, opt.ram_mb, (uint32_t)((tlsf_get_total_size(pool) - tlsf_get_free_size(pool)) >> 20),
                romf.size >> 10, vga_size >> 10, place_img[P_IDE0M] >= 0 ? ", a disk" : ", no disk");
        phase = P_RUN; powered = 1;
        set_state(A68P_S_RUNNING, 0, "Running.");
        return;
    }
    }
}

/* ---- input ------------------------------------------------------------------------------------------- */
/* the ADB extended keyboard's virtual key codes (Inside Macintosh V, "The Toolbox Event Manager", figure 10; the
   'KMAP' of System 7 gives these in the event's message) as Bochs's key codes; 0xFF: no PC key */
#define NO 0xFF
static const uint8_t mac2pc[128] = {
    /* 00 */ A86_KEY_A, A86_KEY_S, A86_KEY_D, A86_KEY_F, A86_KEY_H, A86_KEY_G, A86_KEY_Z, A86_KEY_X,
    /* 08 */ A86_KEY_C, A86_KEY_V, A86_KEY_LEFT_BACKSLASH, A86_KEY_B, A86_KEY_Q, A86_KEY_W, A86_KEY_E, A86_KEY_R,
    /* 10 */ A86_KEY_Y, A86_KEY_T, A86_KEY_1, A86_KEY_2, A86_KEY_3, A86_KEY_4, A86_KEY_6, A86_KEY_5,
    /* 18 */ A86_KEY_EQUALS, A86_KEY_9, A86_KEY_7, A86_KEY_MINUS, A86_KEY_8, A86_KEY_0, A86_KEY_RIGHT_BRACKET, A86_KEY_O,
    /* 20 */ A86_KEY_U, A86_KEY_LEFT_BRACKET, A86_KEY_I, A86_KEY_P, A86_KEY_ENTER, A86_KEY_L, A86_KEY_J, A86_KEY_SINGLE_QUOTE,
    /* 28 */ A86_KEY_K, A86_KEY_SEMICOLON, A86_KEY_BACKSLASH, A86_KEY_COMMA, A86_KEY_SLASH, A86_KEY_N, A86_KEY_M, A86_KEY_PERIOD,
    /* 30 */ A86_KEY_TAB, A86_KEY_SPACE, A86_KEY_GRAVE, A86_KEY_BACKSPACE, A86_KEY_KP_ENTER, A86_KEY_ESC, NO, A86_KEY_WIN_L,
    /* 38 */ A86_KEY_SHIFT_L, A86_KEY_CAPS_LOCK, A86_KEY_ALT_L, A86_KEY_CTRL_L, A86_KEY_SHIFT_R, A86_KEY_ALT_R, A86_KEY_CTRL_R, NO,
    /* 40 */ NO, A86_KEY_KP_DELETE, NO, A86_KEY_KP_MULTIPLY, NO, A86_KEY_KP_ADD, NO, A86_KEY_NUM_LOCK,
    /* 48 */ NO, NO, NO, A86_KEY_KP_DIVIDE, A86_KEY_KP_ENTER, NO, A86_KEY_KP_SUBTRACT, NO,
    /* 50 */ NO, NO, A86_KEY_KP_INSERT, A86_KEY_KP_END, A86_KEY_KP_DOWN, A86_KEY_KP_PAGE_DOWN, A86_KEY_KP_LEFT, A86_KEY_KP_5,
    /* 58 */ A86_KEY_KP_RIGHT, A86_KEY_KP_HOME, NO, A86_KEY_KP_UP, A86_KEY_KP_PAGE_UP, NO, NO, NO,
    /* 60 */ A86_KEY_F5, A86_KEY_F6, A86_KEY_F7, A86_KEY_F3, A86_KEY_F8, A86_KEY_F9, NO, A86_KEY_F11,
    /* 68 */ NO, A86_KEY_PRINT, NO, A86_KEY_SCRL_LOCK, NO, A86_KEY_F10, A86_KEY_MENU, A86_KEY_F12,
    /* 70 */ NO, A86_KEY_PAUSE, A86_KEY_INSERT, A86_KEY_HOME, A86_KEY_PAGE_UP, A86_KEY_DELETE, A86_KEY_F4, A86_KEY_END,
    /* 78 */ A86_KEY_F2, A86_KEY_PAGE_DOWN, A86_KEY_F1, A86_KEY_LEFT, A86_KEY_RIGHT, A86_KEY_DOWN, A86_KEY_UP, NO,
};
static uint32_t held[(A86_KEY_NBKEYS_USED + 31) / 32];
static uint32_t keys_in, mouse_in;
static void key(unsigned k, int up)
{
    if (k >= A86_KEY_NBKEYS_USED) return;
    if (up) held[k / 32] &= ~(1u << (k % 32)); else held[k / 32] |= 1u << (k % 32);
    a86_pc_key(k, up);
    keys_in++;
}
static void release_all(void)
{
    for (unsigned k = 0; k < A86_KEY_NBKEYS_USED; k++)
        if (held[k / 32] & (1u << (k % 32))) key(k, 1);
}
static void input(void)
{
    uint32_t f = blk->focus;
    if (f != blk->focus_ack) {
        if (!f) release_all();
        blk->focus_ack = f;
    }
    uint32_t h = blk->in_head, t = blk->in_tail;
    if (h - t > A68P_IN_SLOTS) t = h - A68P_IN_SLOTS;   /* a ring overrun by the Mac: the oldest are lost */
    DMB();
    for (; t != h; t++) {
        const volatile struct a68p_in *e = (const volatile struct a68p_in *)(win + A68P_OFF_INPUT) + (t & (A68P_IN_SLOTS - 1));
        uint32_t type = e->type, code = e->code;
        if (powered && !paused) switch (type) {
        case A68P_IN_KEYDOWN: case A68P_IN_KEYUP:
            if (code < 128 && mac2pc[code] != NO) key(mac2pc[code], type == A68P_IN_KEYUP);
            break;
        case A68P_IN_BXKEY: key(code, e->modifiers & 1); break;
        case A68P_IN_MOUSE: a86_pc_mouse(e->dx, -e->dy, 0, e->buttons & 7); mouse_in++; break;   /* PS/2: y up */
        case A68P_IN_RELEASE: release_all(); break;
        }
    }
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
static uint32_t col16[16];            /* the attribute colours, 0x00RRGGBB */
static uint8_t  idx16[16];            /* their nearest entries in the screen's colour table (depths 2-8) */
static uint8_t  lum16[16];
static uint8_t *ctab4k;               /* graphics: 12-bit colour -> the nearest entry */
static uint32_t ctab_seq = ~0u, ctab_depth;
static uint32_t cw, ch;               /* the cell drawn: 6 x 13 on one bit, the PC's own on deeper screens */
static int small_font;
static int blink_on = 1;
static uint64_t blink_t, draw_t, stat_t;
static int need_full = 1, menu_was;
static struct { int32_t x0, y0, x1, y1; } dirty;      /* window pixels still to draw */
/* The box around the Mac's pointer (psmac_wclip_cursor_box) is left undrawn while the pointer is there (the Mac keeps
   what was under its pointer and puts it back when the pointer moves). Until 23 November 2026 it went back into the
   dirty rectangle at every picture, which drew it once the pointer had left, but grew the one dirty rectangle to span
   the pointer and whatever else changed (the cursor's cell at each blink: much of the window, 30 times a second), and
   lost the race with the Mac's own putting back. Now: what was left undrawn is kept here (under) (window pixels) with the
   box it was left for; when the box moves or goes (the pointer moved, or was hidden) that part is drawn at once
   and again AGAIN_MS later */
struct under { int on; int32_t x0, y0, x1, y1; };
static struct under under, again;
static int32_t under_box[4];                           /* the pointer's box (screen) when under was left */
static uint64_t again_t;
static void under_add(struct under *h, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 >= x1 || y0 >= y1) return;
    if (!h->on) { h->on = 1; h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1; return; }
    if (x0 < h->x0) h->x0 = x0;
    if (y0 < h->y0) h->y0 = y0;
    if (x1 > h->x1) h->x1 = x1;
    if (y1 > h->y1) h->y1 = y1;
}
static uint32_t drawn_seq_text = ~0u, last_mode;
static uint32_t draw_us_sum, draws_n, draws_all, menu_skips, rgn_skips;

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
static void mark(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 >= x1 || y0 >= y1) return;
    if (dirty.x0 >= dirty.x1) { dirty.x0 = x0; dirty.y0 = y0; dirty.x1 = x1; dirty.y1 = y1; return; }
    if (x0 < dirty.x0) dirty.x0 = x0;
    if (y0 < dirty.y0) dirty.y0 = y0;
    if (x1 > dirty.x1) dirty.x1 = x1;
    if (y1 > dirty.y1) dirty.y1 = y1;
}
/* text cells c0..c1, r0..r1 as window pixels: TEXT_INSET in, the inset itself with the first and last columns */
static void mark_cells(const struct a86_screen *s, uint32_t c0, uint32_t r0, uint32_t c1, uint32_t r1)
{
    int32_t x0 = (int32_t)(c0 * cw) + (c0 ? TEXT_INSET : 0), x1 = (int32_t)(c1 * cw) + TEXT_INSET + (c1 >= s->cols ? TEXT_INSET : 0);
    mark(x0, (int32_t)(r0 * ch), x1, (int32_t)(r1 * ch));
}

/* one pixel of the screen into the frame buffer at its depth: rgb, or (one bit) black */
static inline void put_px(uint8_t *row, int32_t x, uint32_t rgb, int black, uint32_t idx)
{
    switch (sf.depth) {
    case 1: { uint8_t bit = (uint8_t)(0x80u >> (x & 7)); if (black) row[x >> 3] |= bit; else row[x >> 3] &= (uint8_t)~bit; break; }
    case 2: { uint32_t sh = 6 - (uint32_t)(x & 3) * 2; row[x >> 2] = (uint8_t)((row[x >> 2] & ~(3u << sh)) | (idx & 3) << sh); break; }
    case 4: { uint32_t sh = x & 1 ? 0 : 4; row[x >> 1] = (uint8_t)((row[x >> 1] & ~(15u << sh)) | (idx & 15) << sh); break; }
    case 8: row[x] = (uint8_t)idx; break;
    case 16: ((uint16_t *)row)[x] = (uint16_t)(((rgb >> 19) & 31) << 10 | ((rgb >> 11) & 31) << 5 | ((rgb >> 3) & 31)); break;
    default: ((uint32_t *)row)[x] = rgb & 0xffffffu; break;
    }
}

/* the text screen's pixels in the rectangle (screen coordinates) */
static void text_area(const struct a86_screen *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint32_t cols = s->cols, rows = s->rows, chh = s->char_height ? s->char_height : 16;
    int pcsense = (g.options & A68P_O_PCSENSE) != 0;
    for (int32_t y = y0; y < y1; y++) {
        uint8_t *row = sf.base + (uint32_t)y * sf.rb;
        uint32_t wy = (uint32_t)(y - g.t), r = wy / ch, line = wy % ch;
        uint32_t srcline = small_font ? line * chh / ch : line;         /* the PC's scan line, for the cursor */
        for (int32_t x = x0; x < x1; x++) {
            /* the columns from TEXT_INSET on; the insets left and right in the colours of the row's first and last cells */
            int32_t wxs = x - g.l - TEXT_INSET;
            uint32_t c = wxs < 0 ? 0 : (uint32_t)wxs / cw, px = wxs < 0 ? 0 : (uint32_t)wxs % cw;
            int edge = wxs < 0;
            if (cols && c >= cols && (uint32_t)wxs < cols * cw + TEXT_INSET) { c = cols - 1; edge = 1; }
            uint32_t fg = 0, bg = 0, bit = 0;
            if (edge && r < rows) {
                uint32_t at = s->cells[r * cols + c] >> 8;
                fg = at & 15; bg = (at >> 4) & 7;
            } else if (r < rows && c < cols && line < (small_font ? ch : chh)) {
                uint16_t cell = s->cells[r * cols + c];
                uint32_t chr = cell & 0xff, at = cell >> 8;
                fg = at & 15; bg = (at >> 4) & 7;
                if (small_font) bit = (ch == 13 ? psmac_pc_font6x13[chr][line] : psmac_pc_font6x12[chr][line]) >> (7 - px) & 1;
                else bit = px < 8 ? s->font[chr * 32 + line] >> (7 - px) & 1 : (chr >= 0xC0 && chr <= 0xDF ? s->font[chr * 32 + line] & 1 : 0);
                if ((at & 0x80) && !blink_on) bit = 0;                  /* blinking text, in its off half */
                if (r == s->cursor_y && c == s->cursor_x && blink_on && s->cursor_start <= s->cursor_end &&
                    srcline >= s->cursor_start && srcline <= s->cursor_end) bit = 1;
            } else if (r < rows && c < cols) { bg = 0; }                 /* below the glyphs of a short cell */
            if (sf.depth == 1) {
                /* black on white: the lighter of the cell's colours is the paper (A68P_O_PCSENSE: the PC's own) */
                uint32_t lf = lum16[fg], lb = lum16[bg];
                int white = lf == lb ? (pcsense ? lb >= 128 : 1) : ((bit != 0) == (lf > lb));
                if (!pcsense && lf != lb) white = !white;
                put_px(row, x, 0, !white, 0);
            } else {
                uint32_t k = bit ? fg : bg;
                put_px(row, x, col16[k], 0, idx16[k]);
            }
        }
    }
}
/* the graphics picture, scaled to the window (or 1:1 from the scroll position), in the rectangle */
static void graphics_area(const struct a86_screen *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint32_t sw = s->width ? s->width : 640, sh = s->height ? s->height : 480;
    uint32_t ww = (uint32_t)(g.r - g.l), wh = (uint32_t)(g.b - g.t);
    int one = g.scale == A68P_SCALE_1, thr = (g.options & A68P_O_THRESHOLD) != 0;
    for (int32_t y = y0; y < y1; y++) {
        uint8_t *row = sf.base + (uint32_t)y * sf.rb;
        uint32_t wy = (uint32_t)(y - g.t), sy = one ? wy + (uint32_t)g.sy : wy * sh / wh;
        const uint8_t *bay = bayer[y & 7];
        for (int32_t x = x0; x < x1; x++) {
            uint32_t wx = (uint32_t)(x - g.l), sx = one ? wx + (uint32_t)g.sx : wx * sw / ww;
            uint32_t c = sx < sw && sy < sh && sx < A86_PIXELS_PITCH && s->pixels ? s->pixels[sy * A86_PIXELS_PITCH + sx] & 0xffffffu : 0;
            if (sf.depth == 1) put_px(row, x, 0, lum(c) < (thr ? 128u : bay[x & 7] * 4u + 2u), 0);
            else if (sf.depth <= 8) put_px(row, x, c, 0, ctab4k ? ctab4k[((c >> 20) & 15) << 8 | ((c >> 12) & 15) << 4 | ((c >> 4) & 15)] : 0);
            else put_px(row, x, c, 0, 0);
        }
    }
}
static void area(const struct a86_screen *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 >= x1 || y0 >= y1) return;
    if (!powered || s->mode == A86_MODE_NONE) {
        /* off: grey on one bit, black on deeper screens. No picture yet: the PC's background as its text will be drawn,
           white on one bit (black with View > White on Black), black on deeper screens. Until 23 November 2026 it was
           black on one bit too: a black window until the firmware's first words */
        int paper = (g.options & A68P_O_PCSENSE) != 0;
        for (int32_t y = y0; y < y1; y++) {
            uint8_t *row = sf.base + (uint32_t)y * sf.rb;
            for (int32_t x = x0; x < x1; x++) put_px(row, x, 0, sf.depth == 1 ? (!powered ? (x ^ y) & 1 : paper) : 0, idx16[0]);
        }
        return;
    }
    if (s->mode == A86_MODE_TEXT) text_area(s, x0, y0, x1, y1);
    else graphics_area(s, x0, y0, x1, y1);
}
/* the rectangle without the pointer's box (up to four pieces) */
static void area_around(const struct a86_screen *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const int32_t *cb)
{
    if (!cb || cb[2] <= y0 || cb[0] >= y1 || cb[3] <= x0 || cb[1] >= x1) { area(s, x0, y0, x1, y1); return; }
    int32_t t = cb[0] > y0 ? cb[0] : y0, b = cb[2] < y1 ? cb[2] : y1, l = cb[1] > x0 ? cb[1] : x0, r = cb[3] < x1 ? cb[3] : x1;
    area(s, x0, y0, x1, t);
    area(s, x0, b, x1, y1);
    area(s, x0, t, l, b);
    area(s, r, t, x1, b);
    under_add(&under, l - g.l, t - g.t, r - g.l, b - g.t);   /* the pointer's box: drawn once the pointer has moved on */
}

/* ... and without the grow box's corner, when the program draws one there */
static void area_part(const struct a86_screen *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const int32_t *cb)
{
    if (!(g.options & A68P_O_GROWBOX)) { area_around(s, x0, y0, x1, y1, cb); return; }
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
    int tagged = blk->clip_tag == A68P_CLIP_TAG + s1;
    uint32_t rgn = tagged ? blk->vis_rgn : 0, vf = blk->vis_flags;
    DMB();
    if (blk->geom_seq != s1) return;
    g.seq = s1; g.screen = screen; g.visible = vis; g.t = t; g.l = l; g.b = b; g.r = r; g.rgn = rgn; g.vflags = vf;
    g.scale = scale; g.options = op; g.sx = sx < 0 ? 0 : sx; g.sy = sy < 0 ? 0 : sy;
    need_full = 1;
}

static void tables(void)
{
    const struct a86_screen *s = a86_pc_screen();
    for (int i = 0; i < 16; i++) {
        const uint8_t *p = s->palette[s->actl[i]];
        col16[i] = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        lum16[i] = (uint8_t)lum(col16[i]);
    }
    if (sf.depth >= 2 && sf.depth <= 8) {
        uint32_t n = 1u << sf.depth;
        for (int i = 0; i < 16; i++) idx16[i] = (uint8_t)nearest(col16[i], n);
        if (s->mode == A86_MODE_GRAPHICS && (sf.clut_seq != ctab_seq || sf.depth != ctab_depth)) {
            if (!ctab4k) ctab4k = tlsf_malloc(pool, 4096);
            if (ctab4k) for (uint32_t k = 0; k < 4096; k++) ctab4k[k] = (uint8_t)nearest((k >> 8) * 0x110000u | ((k >> 4) & 15) * 0x1100u | (k & 15) * 0x11u, n);
            ctab_seq = sf.clut_seq; ctab_depth = sf.depth;
        }
    }
}

/* the PC's screen into the block for programs that read it (text, colours, font, DAC) */
static void publish(const struct a86_screen *s)
{
    uint32_t n = (uint32_t)s->cols * s->rows;
    if (n > A68P_TEXT_MAX) n = A68P_TEXT_MAX;
    memcpy(win + A68P_OFF_TEXT, (const void *)s->cells, n * 2);
    struct a68p_tinfo *ti = (struct a68p_tinfo *)(win + A68P_OFF_TINFO);
    for (int i = 0; i < 16; i++) ti->colours[i] = col16[i];
    ti->cols = s->cols; ti->rows = s->rows; ti->cursor_x = s->cursor_x; ti->cursor_y = s->cursor_y;
    ti->cursor_start = s->cursor_start; ti->cursor_end = s->cursor_end; ti->char_w = s->char_width; ti->char_h = s->char_height;
    ti->width = s->width; ti->height = s->height; ti->bpp = s->bpp; ti->seq = s->seq;
    if (s->seq != drawn_seq_text) {
        memcpy(win + A68P_OFF_FONT, s->font, sizeof s->font);
        uint32_t *dac = (uint32_t *)(win + A68P_OFF_DAC);
        for (int i = 0; i < 256; i++) dac[i] = (uint32_t)s->palette[i][0] << 16 | (uint32_t)s->palette[i][1] << 8 | s->palette[i][2];
    }
}

static void draw(int force)
{
    uint64_t t0 = now();
    struct a86_screen *s = (struct a86_screen *)a86_pc_screen();
    int menu = psmac_wclip_menu_down();
    if (menu) { menu_was = 1; menu_skips++; return; }    /* any program's menu over the screen: later */
    if (menu_was) { menu_was = 0; }
    if (!g.visible || !psmac_display_surface((int)g.screen, &sf.base, &sf.w, &sf.h, &sf.rb, &sf.depth, &sf.clut, &sf.clut_seq, &sf.gray)) return;
    int32_t ww = g.r - g.l, wh = g.b - g.t;
    if (ww <= 0 || wh <= 0 || ww > 4096 || wh > 4096) return;
    small_font = sf.depth == 1 && !(g.options & A68P_O_BIGTEXT);
    /* 6 x 13 where 25 rows of it fit below the menu bar in a document window (20 + 21 + 325 lines), else 6 x 12: the SE's
       342 lines (tools/mkpcfont.py) */
    cw = small_font ? 6 : (s->char_width == 9 ? 9 : 8);
    ch = small_font ? (sf.h >= 366 ? 13 : 12) : (s->char_height ? s->char_height : 16);
    tables();
    /* what to draw: everything, or the PC's dirty part (its cells, or its pixels, in window pixels) */
    if (s->mode != last_mode) { need_full = 1; last_mode = s->mode; }
    if (force) need_full = 1;
    if (s->dirty_x1) {
        if (s->mode == A86_MODE_TEXT) mark_cells(s, s->dirty_x0, s->dirty_y0, s->dirty_x1, s->dirty_y1);
        else if (g.scale == A68P_SCALE_1) mark(s->dirty_x0 - g.sx, s->dirty_y0 - g.sy, s->dirty_x1 - g.sx, s->dirty_y1 - g.sy);
        else if (s->width && s->height)
            mark((int32_t)((uint32_t)s->dirty_x0 * (uint32_t)ww / s->width), (int32_t)((uint32_t)s->dirty_y0 * (uint32_t)wh / s->height),
                 (int32_t)(((uint32_t)s->dirty_x1 * (uint32_t)ww + s->width - 1) / s->width) + 1, (int32_t)(((uint32_t)s->dirty_y1 * (uint32_t)wh + s->height - 1) / s->height) + 1);
        s->dirty_x1 = 0;                                  /* aurora86.h: the reader clears it */
    }
    /* the box around the Mac's pointer, unless the pointer is hidden (CrsrVis, $8CC, 0: Aurora68 PC hides it while
       the PC has the mouse; the first test in the testbed left a stale square in the middle of the window) */
    int32_t cbox[4];
    int okv;
    uint32_t vis = psmac_wclip_rd32(0x8CC, &okv) >> 24;
    const int32_t *cb = (!okv || vis) && psmac_wclip_cursor_box(g.screen, cbox) ? cbox : 0;
    if (need_full) under.on = again.on = 0;                /* all of it now, the box left out again */
    else {
        if (under.on && (!cb || cb[0] != under_box[0] || cb[1] != under_box[1] || cb[2] != under_box[2] || cb[3] != under_box[3])) {
            mark(under.x0, under.y0, under.x1, under.y1);     /* the pointer moved on or was hidden: what it covered, now... */
            under_add(&again, under.x0, under.y0, under.x1, under.y1);
            again_t = now() + (uint64_t)AGAIN_MS * hz() / 1000u;
            under.on = 0;
        }
        if (again.on && now() >= again_t) { mark(again.x0, again.y0, again.x1, again.y1); again.on = 0; }   /* ...and again */
    }
    if (need_full) { dirty.x0 = 0; dirty.y0 = 0; dirty.x1 = ww; dirty.y1 = wh; }
    if (dirty.x0 >= dirty.x1) { publish(s); return; }
    /* the part on the screen */
    int32_t cx0 = g.l + (dirty.x0 > 0 ? dirty.x0 : 0), cy0 = g.t + (dirty.y0 > 0 ? dirty.y0 : 0);
    int32_t cx1 = g.l + (dirty.x1 < ww ? dirty.x1 : ww), cy1 = g.t + (dirty.y1 < wh ? dirty.y1 : wh);
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > (int32_t)sf.w) cx1 = (int32_t)sf.w;
    if (cy1 > (int32_t)sf.h) cy1 = (int32_t)sf.h;
    dirty.x1 = dirty.x0;                                  /* taken */
    need_full = 0;
    if (cx0 < cx1 && cy0 < cy1) {
        if (!g.rgn) area_part(s, cx0, cy0, cx1, cy1, cb);
        else {
            int32_t rr[PSMAC_WCLIP_MAX][4];
            int n = psmac_wclip_parts(rr, g.rgn, g.vflags & A68P_VF_24BIT ? PSMAC_WCLIP_24BIT : 0, g.l, g.t, cx0, cy0, cx1, cy1);
            if (n < 0) { rgn_skips++; mark(cx0 - g.l, cy0 - g.t, cx1 - g.l, cy1 - g.t); return; }   /* not readable now: again next time */
            for (int k = 0; k < n; k++) area_part(s, rr[k][1], rr[k][0], rr[k][3], rr[k][2], cb);
        }
        DMB();
        if (under.on && cb) for (int k = 0; k < 4; k++) under_box[k] = cb[k];
    }
    publish(s);
    drawn_seq_text = s->seq;
    blk->drawn_seq = s->seq;
    draw_us_sum += us_since(t0); draws_n++; draws_all++;
}

/* the cursor's (and blinking text's) half period over: those cells again */
static void blink(void)
{
    if (now() - blink_t < (uint64_t)BLINK_MS * hz() / 1000u) return;
    blink_t = now();
    blink_on = !blink_on;
    const struct a86_screen *s = a86_pc_screen();
    if (s->mode != A86_MODE_TEXT) return;
    mark_cells(s, s->cursor_x, s->cursor_y, s->cursor_x + 1u, s->cursor_y + 1u);
    uint32_t n = (uint32_t)s->cols * s->rows;
    for (uint32_t i = 0; i < n; i++)
        if (s->cells[i] & 0x8000u) { uint32_t r = i / s->cols, c = i % s->cols; mark_cells(s, c, r, c + 1, r + 1); }
}

/* ---- commands ----------------------------------------------------------------------------------------- */
static int redraw_req;
static int pc_built(void) { return phase == P_RUN; }
static void command(void)
{
    uint32_t seq = blk->cmd_seq;
    if (seq == blk->done_seq) return;
    DMB();
    uint32_t c = blk->cmd;
    fatal_armed = 1;
    if (a86_setjmp(fatal_env)) { fatal_armed = 0; set_state(A68P_S_FAILED, A68P_E_BOCHS, fatal_why); phase = P_DEAD; blk->done_seq = seq; return; }
    switch (c) {
    case A68P_C_POWER_ON:
        if (phase == P_IDLE) { phase = P_ROM; start_t = now(); break; }
        if (phase == P_RUN && !powered) { a86_pc_power(1); powered = 1; paused = 0; set_state(A68P_S_RUNNING, 0, "Running."); redraw_req = 1; }
        break;
    case A68P_C_POWER_OFF:      /* everything the disks still hold written back: then the Pi may be switched off */
        if (phase == P_RUN && powered) { release_all(); a86_pc_power(0); powered = 0; flush_poll(1); set_state(A68P_S_OFF, 0, "The PC is off."); redraw_req = 1; }
        break;
    case A68P_C_RESET:
        if (phase == P_RUN && powered) { flush_poll(1); a86_pc_reset(); paused = 0; set_state(A68P_S_RUNNING, 0, "Running (reset)."); }
        break;
    case A68P_C_PAUSE: if (phase == P_RUN && powered) { paused = 1; set_state(A68P_S_PAUSED, 0, "Paused."); } break;
    case A68P_C_RESUME: if (phase == P_RUN && powered && paused) { paused = 0; set_state(A68P_S_RUNNING, 0, "Running."); } break;
    case A68P_C_CAD:
        if (phase == P_RUN && powered) {
            a86_pc_key(A86_KEY_CTRL_L, 0); a86_pc_key(A86_KEY_ALT_L, 0); a86_pc_key(A86_KEY_DELETE, 0);
            a86_pc_key(A86_KEY_DELETE, 1); a86_pc_key(A86_KEY_ALT_L, 1); a86_pc_key(A86_KEY_CTRL_L, 1);
        }
        break;
    case A68P_C_REDRAW: redraw_req = 1; break;
    case A68P_C_ATTACH: attach(blk->arg0, blk->arg1); break;
    case A68P_C_DETACH: detach(blk->arg1, blk->arg0); break;
    case A68P_C_RESCAN: rescan(); break;
    }
    fatal_armed = 0;
    DMB();
    blk->done_seq = seq;
}

/* ---- the testbed's own eyes (testbed only) ---------------------------------------------------------------
   pc.testwin=SCREEN,LEFT,TOP,RIGHT,BOTTOM: the place set by the kernel itself, visible, with no region (the whole
   rectangle drawn), as if a program had said so: the card's drawing is seen before the Mac's program runs.
   pc.textlog=1: every changed line of the PC's text screen into the kernel log, "[PC] | text" (as Aurora86's
   harness prints it), so a run's log says what the PC showed. */
static int textlog;
static uint32_t row_hash[A86_TEXT_ROWS_MAX], textlog_seq;
static void test_options(void)
{
    const char *v = psmac_option("pc.testwin");
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
        kprintf("[PC] pc.testwin: the window at %d,%d-%d,%d on screen %d, set by the kernel\n", n[1], n[2], n[3], n[4], n[0]);
    }
    v = psmac_option("pc.textlog");
    textlog = v && v[0] == '1';
}
static void text_log(void)
{
    const struct a86_screen *s = a86_pc_screen();
    if (!textlog || s->mode != A86_MODE_TEXT || s->seq == textlog_seq) return;
    textlog_seq = s->seq;
    for (uint32_t r = 0; r < s->rows && r < A86_TEXT_ROWS_MAX; r++) {
        uint32_t h = 2166136261u, last = 0;
        char line[A86_TEXT_COLS_MAX + 1];
        for (uint32_t c = 0; c < s->cols; c++) {
            uint32_t ch8 = s->cells[r * s->cols + c] & 0xff;
            line[c] = ch8 >= 0x20 && ch8 < 0x7f ? (char)ch8 : ' ';
            if (line[c] != ' ') last = c + 1;
            h = (h ^ ch8) * 16777619u;
        }
        line[last] = 0;
        if (h != row_hash[r]) { row_hash[r] = h; if (last) kprintf("[PC] | %s\n", line); }
    }
}

/* ---- the slice --------------------------------------------------------------------------------------- */
static uint64_t next_run, busy_ticks, idle_ticks, icount_last;
static uint32_t ipus = 5;            /* guest instructions per microsecond, measured (starts at 5 MIPS) */

static int my_core(void)
{
    if (opt.core) return (int)opt.core;
    return psmac_board_solo ? 2 : 1;  /* the solo board's core 2 is light; the SE's bus and the IIci bridge's are not */
}

static void numbers(void)
{
    uint64_t t = now();
    if (t - stat_t < hz()) return;
    uint64_t el = t - stat_t;
    stat_t = t;
    uint64_t ic = phase == P_RUN ? a86_pc_icount() : 0;
    blk->icount_hi = (uint32_t)(ic >> 32); blk->icount_lo = (uint32_t)ic;
    blk->mips_x10 = (uint32_t)((ic - icount_last) * hz() / el / 100000u);
    icount_last = ic;
    blk->load = (uint32_t)(busy_ticks * 1000u / el);
    blk->idle_pm = (uint32_t)(idle_ticks * 1000u / el);
    blk->draw_us = draws_n ? draw_us_sum / draws_n : 0;
    blk->draws = draws_all;
    blk->disk_busy = io_count;
    blk->disk_dirty = dirty_total();
    busy_ticks = idle_ticks = 0; draw_us_sum = draws_n = 0; io_count = 0;
    blk->core = (uint32_t)chosen;
}

/* The PC as a guest of its core (psmac_guest.c, 23 November 2026: the Apple II card shares the core). Until then this
   was one function, psmac_pc_poll(core), called from the core's loop: the commands, the window's place, a step of the
   start or else the input, a slice, the write-back, the blink, the picture. It is the same work in three parts now:
   pc_service() every turn (everything but the slice), pc_ready() (powered, not paused, not idle until later) and
   pc_slice() (a86_pc_run for the core's slice). The scheduler gives the PC its slice whenever it is ready and is the
   only guest, so a kernel with the PC card alone runs it as before; with the Apple II card too, the front window's
   machine goes first. */
static int my_core_now(void)
{
    if (!ready) return -1;
    if (chosen < 0) {
        int c = my_core(), expect = -1;
        if (__atomic_compare_exchange_n(&chosen, &expect, c, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            kprintf("[PC] the PC runs on core %d (%s)\n", c, opt.core ? "pc.core=" : psmac_board_solo ? "the solo board: core 2 is free"
                    : psmac_board_se ? "the SE's bus has core 2: slices on core 1" : "core 2 carries the bridge's cycles: slices on core 1");
            stat_t = blink_t = draw_t = now();
            test_options();
            phase = P_ROM;                    /* pc=on: the PC starts with the Mac, as a card in a slot would */
            start_t = now();
        }
    }
    return chosen;
}
static int pc_service(void)
{
    command();
    geometry();
    if (phase >= P_ROM && phase <= P_BUILD) { start_step(); return 1; }
    if (phase != P_RUN) { numbers(); return 0; }
    input();
    flush_poll(0);                        /* the disks' changes back into their files, when due */
    blink();
    const struct a86_screen *s = a86_pc_screen();
    blk->screen_seq = s->seq;
    blk->mode = s->mode;
    if (s->mode == A86_MODE_TEXT && cw) { blk->nat_w = s->cols * cw + 2u * TEXT_INSET; blk->nat_h = s->rows * ch; }   /* the cell as last drawn */
    else if (s->mode == A86_MODE_GRAPHICS) { blk->nat_w = s->width; blk->nat_h = s->height; }
    if (now() - draw_t >= hz() / DRAW_HZ) {
        draw_t = now();
        draw(redraw_req);
        redraw_req = 0;
    }
    text_log();
    numbers();
    return 0;
}
static int pc_ready(uint64_t t) { return phase == P_RUN && powered && !paused && t >= next_run; }
static int pc_slice(uint32_t slice_us)
{
    uint64_t t = now();
    uint64_t want = (uint64_t)ipus * slice_us;
    if (want < 2000) want = 2000;
    uint64_t ic0 = a86_pc_icount();
    fatal_armed = 1;
    if (a86_setjmp(fatal_env)) { fatal_armed = 0; set_state(A68P_S_FAILED, A68P_E_BOCHS, fatal_why); phase = P_DEAD; return 1; }
    int st = a86_pc_run(want);
    fatal_armed = 0;
    uint64_t dt = now() - t;
    busy_ticks += dt;
    uint32_t idle = a86_pc_idle_us();
    uint64_t did = a86_pc_icount() - ic0;
    if (!idle && did > 1000) {           /* a busy slice: its rate, smoothed */
        uint32_t us = (uint32_t)(dt * 1000000u / hz());
        uint32_t r = us ? (uint32_t)(did / us) : ipus;
        if (r < 1) r = 1;
        ipus = (ipus * 7 + r + 7) / 8;
    }
    if (idle) { next_run = now() + (uint64_t)idle * hz() / 1000000u; idle_ticks += (uint64_t)idle * hz() / 1000000u; }
    if (st == A86_STOPPED) { powered = 0; set_state(A68P_S_STOPPED, 0, "The PC switched itself off."); redraw_req = 1; }
    else if (st == A86_FAILED) { set_state(A68P_S_FAILED, A68P_E_BOCHS, "The PC failed (the kernel log has Bochs's words)."); phase = P_DEAD; }
    return 1;
}
static int pc_front(void) { return ready && blk->front == 1; }
struct psmac_guest psmac_pc_guest = { "PC", my_core_now, pc_service, pc_ready, pc_slice, pc_front, 0, 0, 0, 0 };

/* the PC as its core's only guest, as before the scheduler (tools/pccard_test.c) */
int psmac_pc_poll(int core)
{
    if (my_core_now() != core) return 0;
    if (pc_service()) return 1;
    return pc_ready(now()) ? pc_slice(core == 2 ? SLICE_US_CORE2 : SLICE_US_CORE1) : 0;
}

void psmac_pc_report(void)
{
    if (!ready) return;
    static const char *const names[] = { "off", "starting", "running", "paused", "stopped", "failed" };
    uint32_t st = blk->state, rd = 0, wr = 0, held_n = 0, dirty_n = 0;
    for (uint32_t i = 0; i < nimg; i++) { rd += img[i].reads; wr += img[i].writes; held_n += img[i].held; dirty_n += img[i].dirty; }
    kprintf("[PC]   %s on core %d: %u.%u MIPS, %u per mille of the core busy, %u idle; %u pictures drawn (%u us each), %u skipped under a menu, %u for the region; keys %u, mouse %u; disks %u reads %u writes (%u sectors in memory, %u to be written back)%s%s\n",
            st < 6 ? names[st] : "?", chosen, blk->mips_x10 / 10, blk->mips_x10 % 10, blk->load, blk->idle_pm, blk->draws, blk->draw_us,
            menu_skips, rgn_skips, keys_in, mouse_in, rd, wr, held_n, dirty_n,
            st == A68P_S_FAILED ? ": " : "", st == A68P_S_FAILED ? (const char *)blk->message : "");
}
