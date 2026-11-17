/*
 * psmac_board_solo.c — no Macintosh at all: the kernel is the logic board.
 * Copyright (c) 2026 PiStorm-SE project. MPL-2.0.
 *
 * A Pi on the bench with an HDMI monitor and a disk image in its container boots System 7 from
 * that image and shows it on the second screen, with no SE behind it. The IIci ROM still thinks
 * it is on a IIci (psmac_board_se.c keeps answering the RBV, ASC and VDAC, and its identity
 * bits); the four chips that file forwards to the SE's bus are modelled here instead:
 *
 *   VIA1    the 6522: ports, both timers at 783.36 kHz, the shift register, interrupt flags; the
 *           60.15 Hz vertical blank on CA1 and the clock chip's one-second tick on CA2
 *   RTC     the clock / parameter RAM chip on port B bits 0-2 (MAME's macrtc.cpp is the model),
 *           256 bytes of PRAM seeded from what the testbed's IIci left behind, the seconds counter
 *   ADB     the transceiver state machine on port B bits 3-5 and the shift register (MAME's
 *           macadb.cpp): no devices yet, so every Talk times out the way an empty bus does
 *   SCC     a Z8530 register file that remembers what it is told; no ports, no interrupts
 *   IWM     a floppy controller with no drive: every sense line reads "nothing here"
 *   SCSI    an NCR 5380 as the initiator sees it, with the pseudo-DMA windows, and a direct-access
 *           target behind it for each disk image: INQUIRY, READ/WRITE 6 and 10, READ CAPACITY,
 *           MODE SENSE with Apple's page $30, REQUEST SENSE, the rest answered as good
 *
 * Selected with mac.board=solo (which implies the SE layer's address translation and the second
 * screen as the main screen). Disk images come from the container (*.img, *.hda, *.image, *.hfv)
 * in RAM, so writes last until the power goes, or in the testbed from host files (mac.scsi0=path).
 * Written 17 November 2026, 10:30, after the first Pi 3B stood alone on a desk with its WiFi up
 * and nothing to boot (docs/08, item 35).
 */
#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "psmac.h"
#include "psmb.h"
#include "psmac_pram_default.h"

int psmac_board_solo;                       /* mac.board=solo */
extern int psmac_board_se;

int64_t psmac_sh_open(const char *name, uint64_t mode);
int     psmac_sh_read(int64_t fd, void *buf, uint64_t len);
int     psmac_sh_write(int64_t fd, const void *buf, uint64_t len);
int64_t psmac_sh_seek(int64_t fd, uint64_t pos);
int64_t psmac_sh_flen(int64_t fd);
int     psmac_sh_available(void);
const char *psmac_option(const char *key);
int psmac_se_rbv_irq(void);

static inline uint64_t cnt_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0":"=r"(v)); return v; }
static inline uint64_t cnt_frq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0":"=r"(v)); return v; }

/* one lock for every chip: the JIT core (bus accesses) and the service core (time) both come here */
static volatile uint8_t solo_lock;
static inline void lock(void) { while (__atomic_test_and_set(&solo_lock, __ATOMIC_ACQUIRE)) __asm__ volatile("yield"); }
static inline void unlock(void) { __atomic_clear(&solo_lock, __ATOMIC_RELEASE); }

static uint32_t trace_left;                 /* mac.solotrace=N: SCSI commands, ADB commands, RTC commands */
#define TRACE(...) do { if (trace_left) { trace_left--; kprintf("[PSMAC:SOLO] " __VA_ARGS__); } } while (0)

/* ==== time: the VIA's clock ================================================================== */
#define VIA_HZ   783360u
#define VBL_TICKS 13023u                    /* 783360 / 60.15 */
static uint64_t t_base;
static uint64_t via_now(void)
{
    uint64_t d = cnt_now() - t_base, f = cnt_frq();
    return (d / f) * VIA_HZ + ((d % f) * VIA_HZ) / f;     /* no 128-bit division in a freestanding kernel */
}
static uint64_t us_to_ticks(uint32_t us) { return ((uint64_t)us * VIA_HZ) / 1000000u; }

/* ==== the clock chip ========================================================================== */
static struct {
    uint8_t pram[256];
    uint32_t seconds;                       /* since 1904, as the Mac counts */
    int enb, clk, data_in, data_out;
    uint8_t byte; int bits, dir;            /* the byte in flight; dir 1 = chip to host */
    uint8_t cmd; int state;                 /* 0 normal, 1 write data pending, 2 extended command, 3 extended write */
    int xpaddr, write_protect;
    uint64_t next_second;
    uint32_t cmds;
} rtc;

static void rtc_execute(uint8_t data)
{
    rtc.cmds++;
    if (rtc.state == 2) {                   /* extended: second byte carries the low address bits */
        rtc.xpaddr = ((rtc.cmd & 7) << 5) | ((data & 0x7c) >> 2);
        if (rtc.cmd & 0x80) { rtc.dir = 1; rtc.byte = rtc.pram[rtc.xpaddr]; rtc.state = 0; }
        else { rtc.state = 3; rtc.byte = 0; rtc.bits = 0; }
        return;
    }
    if (rtc.state == 3) { rtc.pram[rtc.xpaddr] = data; rtc.state = 0; return; }
    if (rtc.state == 1) {
        rtc.state = 0;
        int i = (rtc.cmd >> 2) & 0x1f;
        if (rtc.write_protect && i != 13) return;
        if (i < 4) { rtc.seconds = (rtc.seconds & ~(0xffu << (8 * i))) | ((uint32_t)data << (8 * i)); }
        else if (i >= 8 && i < 12) rtc.pram[i] = data;
        else if (i == 12) { /* test register */ }
        else if (i == 13) rtc.write_protect = (data & 0x80) != 0;
        else if (i >= 16) rtc.pram[i] = data;
        return;
    }
    rtc.cmd = data;
    if ((rtc.cmd & 0x78) == 0x38) { rtc.state = 2; rtc.byte = 0; rtc.bits = 0; return; }
    if (rtc.cmd & 0x80) {
        int i = (rtc.cmd >> 2) & 0x1f;
        rtc.dir = 1;
        if (i < 8) rtc.byte = (rtc.seconds >> (8 * (i & 3))) & 0xff;
        else if (i >= 8 && i < 12) rtc.byte = rtc.pram[i];
        else if (i >= 16) rtc.byte = rtc.pram[i];
        else rtc.byte = 0;
    } else { rtc.state = 1; rtc.byte = 0; rtc.bits = 0; }
}
/* port B as the ROM drives it: bit 2 enable (low = listening), bit 1 clock, bit 0 data */
static void rtc_lines(int enb, int clk, int data)
{
    if (enb != rtc.enb) { rtc.enb = enb; rtc.byte = 0; rtc.bits = 0; rtc.dir = 0; rtc.data_out = 0; rtc.state = 0; }
    rtc.data_in = data;
    if (!clk && rtc.clk && !rtc.enb) {      /* falling edge shifts */
        if (rtc.dir) { if (rtc.bits > 0) rtc.bits--; rtc.data_out = (rtc.byte >> rtc.bits) & 1; }
        else { rtc.byte = (rtc.byte << 1) | (data ? 1 : 0); if (++rtc.bits == 8) { rtc_execute(rtc.byte); if (rtc.dir) rtc.bits = 8; else { rtc.byte = 0; rtc.bits = 0; } } }
    }
    rtc.clk = clk;
}

/* ==== the VIA ================================================================================ */
enum { IFR_CA2 = 0x01, IFR_CA1 = 0x02, IFR_SR = 0x04, IFR_CB2 = 0x08, IFR_CB1 = 0x10, IFR_T2 = 0x20, IFR_T1 = 0x40 };
static struct {
    uint8_t orb, ora, ddrb, ddra, acr, pcr, ifr, ier, sr;
    uint16_t t1l, t2l;
    uint64_t t1_start, t2_start; int t1_run, t2_run, t1_fired; uint64_t t1_seen;
    uint64_t next_vbl;
    uint32_t vbls, irqs;
} via;

/* ---- the ADB transceiver, as the ROM sees it through the VIA (macadb.cpp's model) ---- */
enum { ADB_NEW_COMMAND = 0, ADB_XFER_EVEN = 1, ADB_XFER_ODD = 2, ADB_IDLE = 3 };
static struct {
    int state, irq;                         /* irq: the transceiver's interrupt line, 1 = asserted (PB3 reads 0) */
    int direction;                          /* 1 = the Mac is shifting out, 0 = a device would shift in */
    int datasize; uint8_t buffer[8]; int waiting_cmd, last_talk;
    int pending; uint64_t due; uint8_t send;
    uint32_t cmds, talks;
} adb;

static void adb_talk(uint8_t cmd)
{
    int addr = cmd >> 4, kind = (cmd >> 2) & 3;
    adb.cmds++;
    TRACE("ADB command %02x (addr %d %s reg %d): no devices\n", cmd, addr, kind == 3 ? "talk" : kind == 2 ? "listen" : "reset/flush", cmd & 3);
    adb.direction = 0; adb.datasize = 0;
    if (kind == 3) { adb.last_talk = addr; adb.talks++; }
    adb.waiting_cmd = 0;
}
static void adb_newstate(int s)
{
    if (s == adb.state) return;
    adb.state = s;
    switch (s) {
    case ADB_NEW_COMMAND:
        adb.direction = 1; adb.waiting_cmd = 1; adb.irq = 0;
        adb.pending = 1; adb.due = via_now() + us_to_ticks(800);
        break;
    case ADB_XFER_EVEN: case ADB_XFER_ODD:
        if (adb.datasize > 0 && !adb.direction) { adb.send = adb.buffer[0]; adb.datasize--; for (int i = 0; i < adb.datasize; i++) adb.buffer[i] = adb.buffer[i + 1]; }
        else { adb.send = 0; adb.irq = 1; }          /* nothing to say: the line the ROM reads as "no response" */
        adb.pending = 1; adb.due = via_now() + us_to_ticks(800);
        break;
    case ADB_IDLE:
        adb.irq = 0; adb.pending = 0;
        break;
    }
}
static void adb_tick(uint64_t now)
{
    if (!adb.pending || now < adb.due) return;
    adb.pending = 0;
    if (adb.state == ADB_NEW_COMMAND) { adb_talk(via.sr); via.ifr |= IFR_SR; }
    else if (adb.state == ADB_XFER_EVEN || adb.state == ADB_XFER_ODD) { if (!adb.direction) via.sr = adb.send; via.ifr |= IFR_SR; }
}

static void via_timers(uint64_t now)
{
    if (via.t1_run) {
        uint64_t el = now - via.t1_start, period = (uint64_t)via.t1l + 2;
        if (via.acr & 0x40) { uint64_t n = el / period; if (n > via.t1_seen) { via.ifr |= IFR_T1; via.t1_seen = n; } }
        else if (!via.t1_fired && el >= period) { via.ifr |= IFR_T1; via.t1_fired = 1; }
    }
    if (via.t2_run && !(via.acr & 0x20)) {
        uint64_t el = now - via.t2_start;
        if (el >= (uint64_t)via.t2l + 2) { via.ifr |= IFR_T2; via.t2_run = 0; }
    }
    while (now >= via.next_vbl) { via.ifr |= IFR_CA1; via.next_vbl += VBL_TICKS; via.vbls++; }
    while (now >= rtc.next_second) { via.ifr |= IFR_CA2; rtc.next_second += VIA_HZ; rtc.seconds++; }
    adb_tick(now);
}
static uint16_t via_t1(uint64_t now)
{
    if (!via.t1_run) return via.t1l;
    uint64_t el = now - via.t1_start, period = (uint64_t)via.t1l + 2;
    if (via.acr & 0x40) return (uint16_t)(via.t1l - (el % period));
    return (uint16_t)(via.t1l - el);
}
static uint16_t via_t2(uint64_t now)
{
    if (!via.t2_start) return via.t2l;
    return (uint16_t)(via.t2l - (now - via.t2_start));
}
static int via_irq(void) { return (via.ifr & via.ier & 0x7f) != 0; }

static void via_apply_portb(void)
{
    /* outputs the ROM drives: the clock chip's three lines (only the bits set as outputs count) */
    uint8_t out = via.orb | ~via.ddrb;          /* an input line floats high */
    rtc_lines((out >> 2) & 1, (out >> 1) & 1, out & 1);
    if (via.ddrb & 0x30) adb_newstate((via.orb >> 4) & 3);
}
static uint8_t via_portb_in(void)
{
    uint8_t in = 0xff;
    if (!rtc.data_out) in &= ~0x01;
    if (adb.irq) in &= ~0x08;
    return in;
}
static uint32_t via_read(uint32_t reg)
{
    uint64_t now = via_now(); via_timers(now);
    uint32_t r;
    switch (reg) {
    case 0:  via.ifr &= ~(IFR_CB1 | IFR_CB2); r = (via.orb & via.ddrb) | (via_portb_in() & ~via.ddrb); break;
    case 1:  via.ifr &= ~IFR_CA1; if ((via.pcr & 0x0a) != 0x02) via.ifr &= ~IFR_CA2; /* fall through */
    case 15: r = ((via.ora & via.ddra) & 0x38) | 0xc7; break;   /* the IIci's identity (psmac_board_se.c), SCC W/REQ idle */
    case 2:  r = via.ddrb; break;
    case 3:  r = via.ddra; break;
    case 4:  via.ifr &= ~IFR_T1; r = via_t1(now) & 0xff; break;
    case 5:  r = via_t1(now) >> 8; break;
    case 6:  r = via.t1l & 0xff; break;
    case 7:  r = via.t1l >> 8; break;
    case 8:  via.ifr &= ~IFR_T2; r = via_t2(now) & 0xff; break;
    case 9:  r = via_t2(now) >> 8; break;
    case 10: via.ifr &= ~IFR_SR; r = via.sr; break;
    case 11: r = via.acr; break;
    case 12: r = via.pcr; break;
    case 13: r = (via.ifr & 0x7f) | (via_irq() ? 0x80 : 0); break;
    case 14: r = via.ier | 0x80; break;
    default: r = 0; break;
    }
    return r;
}
static void via_write(uint32_t reg, uint8_t d)
{
    uint64_t now = via_now(); via_timers(now);
    switch (reg) {
    case 0:  via.ifr &= ~(IFR_CB1 | IFR_CB2); via.orb = d; via_apply_portb(); break;
    case 1:  via.ifr &= ~IFR_CA1; if ((via.pcr & 0x0a) != 0x02) via.ifr &= ~IFR_CA2; /* fall through */
    case 15: via.ora = d; break;
    case 2:  via.ddrb = d; via_apply_portb(); break;
    case 3:  via.ddra = d; break;
    case 4: case 6: via.t1l = (via.t1l & 0xff00) | d; break;
    case 5:  via.t1l = (via.t1l & 0x00ff) | (d << 8); via.t1_start = now; via.t1_run = 1; via.t1_fired = 0; via.t1_seen = 0; via.ifr &= ~IFR_T1; break;
    case 7:  via.t1l = (via.t1l & 0x00ff) | (d << 8); via.ifr &= ~IFR_T1; break;
    case 8:  via.t2l = (via.t2l & 0xff00) | d; break;
    case 9:  via.t2l = (via.t2l & 0x00ff) | (d << 8); via.t2_start = now ? now : 1; via.t2_run = 1; via.ifr &= ~IFR_T2; break;
    case 10: via.ifr &= ~IFR_SR; via.sr = d; break;
    case 11: via.acr = d; break;
    case 12: via.pcr = d; break;
    case 13: via.ifr &= ~(d & 0x7f); break;
    case 14: if (d & 0x80) via.ier |= d & 0x7f; else via.ier &= ~(d & 0x7f); break;
    }
}

/* ==== the SCC: a register file ================================================================ */
static struct { uint8_t wr[2][16], ptr[2]; uint32_t writes; } scc;
static uint32_t scc_read(uint32_t off)
{
    int ch = (off & 2) ? 1 : 0;                /* +0 B control, +2 A control, +4 B data, +6 A data; index 0 = B, 1 = A */
    if (off & 4) return 0;                      /* data: nothing received */
    int p = scc.ptr[ch]; scc.ptr[ch] = 0;
    switch (p) {
    case 0:  return 0x6c;                       /* Tx empty, DCD, CTS, Tx underrun: an idle port */
    case 1:  return 0x07;                       /* all sent, residue codes */
    case 2:  return scc.wr[ch][2];
    case 3:  return 0;
    case 10: return 0;
    case 12: case 13: case 15: return scc.wr[ch][p];
    default: return 0;
    }
}
static void scc_write(uint32_t off, uint8_t d)
{
    int ch = (off & 2) ? 1 : 0;
    scc.writes++;
    if (off & 4) return;                        /* data: swallowed */
    if (scc.ptr[ch] == 0) {
        int p = d & 7; if ((d & 0x38) == 0x08) p += 8;
        if (p) { scc.ptr[ch] = p; return; }
        return;                                 /* WR0 commands: reset status, reset ints: nothing pending here */
    }
    scc.wr[ch][scc.ptr[ch]] = d;
    if (scc.ptr[ch] == 9) { scc.wr[0][9] = d; scc.wr[1][9] = d; }
    scc.ptr[ch] = 0;
}

/* ==== the IWM: no drive ======================================================================= */
static struct { uint16_t lines; uint8_t mode; } iwm;
static uint32_t iwm_access(uint32_t reg, int write, uint8_t d)
{
    if (reg & 1) iwm.lines |= 1u << (reg >> 1); else iwm.lines &= ~(1u << (reg >> 1));
    int q6 = (iwm.lines >> 6) & 1, q7 = (iwm.lines >> 7) & 1, motor = (iwm.lines >> 4) & 1;
    if (write) { if (q7 && q6 && !motor) iwm.mode = d & 0x1f; return 0; }
    if (!q7 && q6) return 0x80 | (motor ? 0x20 : 0) | iwm.mode;   /* status: sense line high (no drive), mode */
    if (q7 && q6) return 0xc0;                                    /* handshake: ready, no underrun */
    return 0xff;                                                  /* data: nothing */
}

/* ==== disk images ============================================================================= */
struct disk { char name[48]; uint8_t *ram; int64_t fd; uint32_t blocks; int writable; uint32_t reads, writes; };
static struct disk disks[7]; static int ndisk;
static uint8_t sector_buf[64 * 1024] __attribute__((aligned(64)));

void psmac_image_add(const char *name, uint8_t *data, uint32_t len)
{
    if (ndisk >= 7 || len < 1024) return;
    struct disk *d = &disks[ndisk++];
    int k = 0; while (name[k] && k < 47) { d->name[k] = name[k]; k++; } d->name[k] = 0;
    d->ram = data; d->fd = -1; d->blocks = len / 512; d->writable = 1;
}
int psmac_image_count(void) { return ndisk; }
int psmac_image_get(int i, const char **name, uint8_t **data, uint32_t *blocks)
{
    if (i < 0 || i >= ndisk || !disks[i].ram) return 0;
    *name = disks[i].name; *data = disks[i].ram; *blocks = disks[i].blocks; return 1;
}
static void disk_add_file(const char *path)
{
    if (ndisk >= 7 || !psmac_sh_available()) return;
    int64_t fd = psmac_sh_open(path, 3); int writable = 1;
    if (fd < 0) { fd = psmac_sh_open(path, 1); writable = 0; }
    if (fd < 0) { kprintf("[PSMAC:SOLO] cannot open disk image %s\n", path); return; }
    struct disk *d = &disks[ndisk++];
    const char *base = path; for (const char *s = path; *s; s++) if (*s == '/') base = s + 1;
    int k = 0; while (base[k] && k < 47) { d->name[k] = base[k]; k++; } d->name[k] = 0;
    d->ram = 0; d->fd = fd; d->blocks = (uint32_t)(psmac_sh_flen(fd) / 512); d->writable = writable;
}
static int disk_read(struct disk *d, uint32_t lba, uint32_t n, uint8_t *buf)
{
    d->reads += n;
    if (d->ram) { memcpy(buf, d->ram + (uint64_t)lba * 512, n * 512); return 1; }
    if (psmac_sh_seek(d->fd, (uint64_t)lba * 512) < 0) return 0;
    return psmac_sh_read(d->fd, buf, n * 512);
}
static int disk_write(struct disk *d, uint32_t lba, uint32_t n, const uint8_t *buf)
{
    d->writes += n;
    if (d->ram) { if (d->ram + (uint64_t)lba * 512 != buf) memcpy(d->ram + (uint64_t)lba * 512, buf, n * 512); return 1; }
    if (psmac_sh_seek(d->fd, (uint64_t)lba * 512) < 0) return 0;
    return psmac_sh_write(d->fd, buf, n * 512);
}

/* ==== the SCSI bus: an NCR 5380 and one target at a time ====================================== */
enum { PH_DATAOUT = 0, PH_DATAIN = 1, PH_CMD = 2, PH_STATUS = 3, PH_MSGOUT = 6, PH_MSGIN = 7, PH_FREE = 8 };
static struct {
    uint8_t odr, icr, mr, tcr, ser;
    int dma, dma_dir, irq, busy_err, aip, ack;
} n;
static struct {
    int phase, req;
    struct disk *d; int id;
    uint8_t cdb[16]; int cdb_n, cdb_len;
    uint8_t *buf; uint32_t buf_len, buf_pos;   /* the data phase's current chunk */
    uint32_t left, lba;                        /* bytes still to move after this chunk, next block */
    int chunked;                               /* chunks through sector_buf (file images) */
    uint8_t *wr_ram; uint32_t wr_len;          /* a WRITE straight into a RAM image: marked for the card when its data is in */
    uint8_t status, msgin, sense_key, asc;
    uint8_t small[256];
    uint32_t cmds, mismatches, underflows;
    struct { uint8_t cdb[10], status; uint32_t n; } ring[12]; uint32_t ring_n;
} t;
static const char *phase_name(int p) { static const char *const nm[] = { "data out", "data in", "command", "status", "?", "?", "message out", "message in", "free" }; return nm[p & 15]; }

static void target_free(void)
{
    if (t.phase != PH_FREE && n.dma) { n.irq = 1; n.dma = 0; t.mismatches++; }
    t.phase = PH_FREE; t.req = 0; t.d = 0;
    if (n.mr & 0x04) { n.busy_err = 1; n.irq = 1; }
}
static void set_phase(int p)
{
    if (p == PH_STATUS && t.ring_n) t.ring[(t.ring_n - 1) % 12].status = t.status;
    if (p != t.phase && n.dma) { n.irq = 1; n.dma = 0; t.mismatches++; }   /* a phase change ends a DMA transfer with an interrupt */
    t.phase = p; t.req = 1;
}
static void check_condition(uint8_t key, uint8_t asc) { t.status = 2; t.sense_key = key; t.asc = asc; }
static void data_in_small(uint32_t len, uint32_t alloc)
{
    if (alloc && alloc < len) len = alloc;
    t.buf = t.small; t.buf_len = len; t.buf_pos = 0; t.left = 0; t.chunked = 0;
    if (len) set_phase(PH_DATAIN); else set_phase(PH_STATUS);
}
static void next_chunk(int write)
{
    struct disk *d = t.d;
    uint32_t n_blk = t.left / 512; if (n_blk > sizeof(sector_buf) / 512) n_blk = sizeof(sector_buf) / 512;
    if (!write && !disk_read(d, t.lba, n_blk, sector_buf)) { check_condition(4, 0x11); set_phase(PH_STATUS); return; }
    t.buf = sector_buf; t.buf_len = n_blk * 512; t.buf_pos = 0; t.left -= n_blk * 512;
    t.lba += n_blk;
}
static void execute(void)
{
    struct disk *d = t.d;
    uint8_t op = t.cdb[0];
    uint32_t lba = 0, cnt = 0;
    t.status = 0; t.cmds++; t.wr_ram = 0;
    { unsigned k = t.ring_n++ % 12; for (int i = 0; i < 10; i++) t.ring[k].cdb[i] = t.cdb[i]; t.ring[k].n = t.cmds; }
    TRACE("SCSI %d: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", t.id, t.cdb[0], t.cdb[1], t.cdb[2], t.cdb[3], t.cdb[4], t.cdb[5], t.cdb[6], t.cdb[7], t.cdb[8], t.cdb[9]);
    if ((t.cdb[1] >> 5) && op != 0x12 && op != 0x03) { check_condition(5, 0x25); set_phase(PH_STATUS); return; }
    for (int i = 0; i < 64; i++) t.small[i] = 0;
    switch (op) {
    case 0x00: case 0x01: case 0x0b: case 0x2b: case 0x16: case 0x17: case 0x1b: case 0x1e: case 0x2f: case 0x35: case 0x04:
        set_phase(PH_STATUS); return;
    case 0x03: {                                /* REQUEST SENSE */
        t.small[0] = 0x70; t.small[2] = t.sense_key; t.small[7] = 10; t.small[12] = t.asc;
        t.sense_key = 0; t.asc = 0;
        data_in_small(18, t.cdb[4]); return; }
    case 0x12: {                                /* INQUIRY */
        static const char vendor[] = "AURORA68", product[] = "PiDisk SCSI     ", rev[] = "0.4 ";
        t.small[0] = (t.cdb[1] >> 5) ? 0x7f : 0x00; t.small[2] = 0x02; t.small[3] = 0x02; t.small[4] = 31;
        for (int i = 0; i < 8; i++) t.small[8 + i] = vendor[i];
        for (int i = 0; i < 16; i++) t.small[16 + i] = product[i];
        for (int i = 0; i < 4; i++) t.small[32 + i] = rev[i];
        data_in_small(36, t.cdb[4]); return; }
    case 0x25: {                                /* READ CAPACITY */
        uint32_t last = d->blocks - 1;
        t.small[0] = last >> 24; t.small[1] = last >> 16; t.small[2] = last >> 8; t.small[3] = last;
        t.small[6] = 2; t.small[7] = 0;
        data_in_small(8, 0); return; }
    case 0x1a: {                                /* MODE SENSE(6) */
        int page = t.cdb[2] & 0x3f, dbd = t.cdb[1] & 0x08; uint32_t p = 4;
        if (!dbd) { t.small[3] = 8; uint32_t b = d->blocks > 0xffffff ? 0 : d->blocks; t.small[5] = b >> 16; t.small[6] = b >> 8; t.small[7] = b; t.small[10] = 2; p = 12; }
        if (!d->writable) t.small[2] = 0x80;
        int any = 0;
        if (page == 1 || page == 0x3f) { t.small[p] = 0x01; t.small[p + 1] = 10; p += 12; any = 1; }
        if (page == 3 || page == 0x3f) { t.small[p] = 0x03; t.small[p + 1] = 22; t.small[p + 10] = 0; t.small[p + 11] = 32; t.small[p + 12] = 2; t.small[p + 13] = 0; t.small[p + 20] = 0x40; p += 24; any = 1; }
        if (page == 4 || page == 0x3f) { uint32_t cyl = d->blocks / (16 * 32); t.small[p] = 0x04; t.small[p + 1] = 22; t.small[p + 2] = cyl >> 16; t.small[p + 3] = cyl >> 8; t.small[p + 4] = cyl; t.small[p + 5] = 16; t.small[p + 20] = 0x1c; t.small[p + 21] = 0x20; p += 24; any = 1; }
        if (page == 8 || page == 0x3f) { t.small[p] = 0x08; t.small[p + 1] = 18; p += 20; any = 1; }
        if (page == 0x30 || page == 0x3f) { static const char apple[] = "APPLE COMPUTER, INC."; t.small[p] = 0x30; t.small[p + 1] = 22; for (int i = 0; i < 20; i++) t.small[p + 2 + i] = apple[i]; p += 24; any = 1; }
        if (!any && page != 0) { check_condition(5, 0x24); set_phase(PH_STATUS); return; }
        t.small[0] = p - 1;
        data_in_small(p, t.cdb[4]); return; }
    case 0x15: {                                /* MODE SELECT: taken and forgotten */
        uint32_t len = t.cdb[4];
        if (!len) { set_phase(PH_STATUS); return; }
        t.buf = t.small; t.buf_len = len > 255 ? 255 : len; t.buf_pos = 0; t.left = 0; t.chunked = 0; set_phase(PH_DATAOUT); return; }
    case 0x08: case 0x0a:                       /* READ(6), WRITE(6) */
        lba = ((t.cdb[1] & 0x1f) << 16) | (t.cdb[2] << 8) | t.cdb[3]; cnt = t.cdb[4] ? t.cdb[4] : 256; break;
    case 0x28: case 0x2a:                       /* READ(10), WRITE(10) */
        lba = ((uint32_t)t.cdb[2] << 24) | (t.cdb[3] << 16) | (t.cdb[4] << 8) | t.cdb[5]; cnt = (t.cdb[7] << 8) | t.cdb[8]; break;
    default:
        TRACE("SCSI %d: command %02x not supported\n", t.id, op);
        check_condition(5, 0x20); set_phase(PH_STATUS); return;
    }
    /* a transfer */
    if ((uint64_t)lba + cnt > d->blocks) { check_condition(5, 0x21); set_phase(PH_STATUS); return; }
    if (!cnt) { set_phase(PH_STATUS); return; }
    int write = (op == 0x0a || op == 0x2a);
    if (write && !d->writable) { check_condition(7, 0x27); set_phase(PH_STATUS); return; }
    if (d->ram) { t.buf = d->ram + (uint64_t)lba * 512; t.buf_len = cnt * 512; t.buf_pos = 0; t.left = 0; t.chunked = 0; if (write) { d->writes += cnt; t.wr_ram = t.buf; t.wr_len = cnt * 512; } else d->reads += cnt; }
    else { t.left = cnt * 512; t.lba = lba; t.chunked = 1; if (write) { uint32_t nb = cnt > sizeof(sector_buf) / 512 ? sizeof(sector_buf) / 512 : cnt; t.buf = sector_buf; t.buf_len = nb * 512; t.buf_pos = 0; t.left -= nb * 512; } else next_chunk(0); if (t.phase == PH_STATUS) return; }
    set_phase(write ? PH_DATAOUT : PH_DATAIN);
}
/* the byte on the bus, as the initiator reads it */
static uint8_t cur_data(void)
{
    if (t.phase == PH_DATAIN && t.buf_pos < t.buf_len) return t.buf[t.buf_pos];
    if (t.phase == PH_STATUS) return t.status;
    if (t.phase == PH_MSGIN) return t.msgin;
    if ((n.icr & 0x01) || (n.mr & 0x01)) return n.odr;
    return 0;
}
/* the initiator's ACK: the byte is taken or given, REQ drops */
static void ack_byte(void)
{
    if (!t.req) return;
    switch (t.phase) {
    case PH_CMD:
        if (t.cdb_n < 16) t.cdb[t.cdb_n] = n.odr;
        if (t.cdb_n == 0) { uint8_t g = n.odr >> 5; t.cdb_len = g == 0 ? 6 : (g == 1 || g == 2) ? 10 : g == 5 ? 12 : 6; }
        t.cdb_n++;
        break;
    case PH_MSGOUT: break;
    case PH_DATAOUT: if (t.buf_pos < t.buf_len) t.buf[t.buf_pos++] = n.odr; break;
    case PH_DATAIN:  if (t.buf_pos < t.buf_len) t.buf_pos++; break;
    default: break;
    }
    t.req = 0;
}
/* ACK released: the target moves on */
static void advance(void)
{
    switch (t.phase) {
    case PH_CMD:
        if (t.cdb_n < t.cdb_len) t.req = 1; else execute();
        break;
    case PH_MSGOUT:
        if (n.icr & 0x02) t.req = 1; else { t.cdb_n = 0; t.cdb_len = 6; set_phase(PH_CMD); }
        break;
    case PH_DATAIN:
        if (t.buf_pos < t.buf_len) t.req = 1;
        else if (t.left) { next_chunk(0); if (t.phase == PH_DATAIN) t.req = 1; }
        else set_phase(PH_STATUS);
        break;
    case PH_DATAOUT:
        if (t.buf_pos < t.buf_len) t.req = 1;
        else {
            if (t.chunked) {
                uint32_t nb = t.buf_len / 512;
                if (!disk_write(t.d, t.lba, nb, sector_buf)) { check_condition(4, 0x03); set_phase(PH_STATUS); break; }
                t.lba += nb;
                if (t.left) { nb = t.left / 512; if (nb > sizeof(sector_buf) / 512) nb = sizeof(sector_buf) / 512; t.buf_len = nb * 512; t.buf_pos = 0; t.left -= nb * 512; t.req = 1; break; }
            }
            if (t.wr_ram) { extern void psmac_card_dirty(const void *, uint32_t); psmac_card_dirty(t.wr_ram, t.wr_len); t.wr_ram = 0; }
            set_phase(PH_STATUS);
        }
        break;
    case PH_STATUS: t.msgin = 0; set_phase(PH_MSGIN); break;
    case PH_MSGIN:  target_free(); break;
    default: break;
    }
}
static void try_select(void)
{
    if (t.phase != PH_FREE || !(n.icr & 0x04) || (n.icr & 0x08) || !(n.icr & 0x01) || (n.icr & 0x80)) return;
    for (int i = 0; i < ndisk; i++) {
        if (!(n.odr & (1u << i))) continue;
        t.d = &disks[i]; t.id = i; t.cdb_n = 0; t.cdb_len = 6; t.status = 0;
        n.aip = 0;
        if (n.icr & 0x02) set_phase(PH_MSGOUT); else set_phase(PH_CMD);
        return;
    }
}
static int phase_match(void) { return t.phase != PH_FREE && (n.tcr & 7) == (t.phase & 7); }
static uint32_t scsi_read(uint32_t reg)
{
    switch (reg) {
    case 0: return cur_data();
    case 1: return (n.icr & 0x9f) | (n.aip ? 0x40 : 0);
    case 2: return n.mr;
    case 3: return n.tcr;
    case 4: {
        uint32_t v = 0;
        if (n.icr & 0x80) v |= 0x80;
        if (t.phase != PH_FREE || (n.icr & 0x08)) v |= 0x40;
        if (t.req) v |= 0x20;
        if (t.phase != PH_FREE) v |= (t.phase & 7) << 2;
        if (n.icr & 0x04) v |= 0x02;
        return v; }
    case 5: {
        uint32_t v = 0;
        if (n.dma && t.req && phase_match()) v |= 0x40;
        if (n.irq) v |= 0x10;
        if (phase_match()) v |= 0x08;
        if (n.busy_err) v |= 0x04;
        if (n.icr & 0x02) v |= 0x02;
        if (n.icr & 0x10) v |= 0x01;
        return v; }
    case 6: return cur_data();
    case 7: n.irq = 0; n.busy_err = 0; return 0;
    }
    return 0;
}
static void scsi_write(uint32_t reg, uint8_t d)
{
    switch (reg) {
    case 0: n.odr = d; break;
    case 1: {
        uint8_t was = n.icr;
        n.icr = d & 0x9f;
        if (d & 0x80) { if (!(was & 0x80)) { target_free(); n.irq = 1; } break; }
        try_select();
        int ack = (n.icr & 0x10) != 0, was_ack = (was & 0x10) != 0;
        if (ack && !was_ack && !n.dma) ack_byte();
        if (!ack && was_ack && !n.dma) advance();
        break; }
    case 2: n.mr = d; if (d & 0x01) n.aip = 1; if (!(d & 0x02)) n.dma = 0; break;
    case 3: n.tcr = d & 0x0f; break;
    case 4: n.ser = d; break;
    case 5: if (n.mr & 0x02) { n.dma = 1; n.dma_dir = 1; } break;
    case 6: break;
    case 7: if (n.mr & 0x02) { n.dma = 1; n.dma_dir = 0; } break;
    }
}
/* one byte through a pseudo-DMA window */
static uint8_t pdma_read(void)
{
    if (!(t.req && t.phase == PH_DATAIN)) { t.underflows++; return 0xff; }
    uint8_t v = cur_data(); ack_byte(); advance(); return v;
}
static void pdma_write(uint8_t d)
{
    if (!(t.req && (t.phase == PH_DATAOUT || t.phase == PH_CMD || t.phase == PH_MSGOUT))) { t.underflows++; return; }
    n.odr = d; ack_byte(); advance();
}

/* ==== the bus layer's entry: the four chips the SE layer would forward ======================== */
int psmac_solo_access(uint32_t addr, int size, int write, uint32_t data, uint32_t *val)
{
    if ((addr & 0xff000000u) != 0x50000000u) return 0;
    uint32_t off = addr & 0x000fffffu, r = 0;
    int handled = 1;
    lock();
    if (off < 0x2000 || (off >= 0x40000 && off < 0x42000)) {
        uint32_t reg = (off >> 9) & 15;
        if (write) via_write(reg, data & 0xff); else r = via_read(reg);
    } else if (off >= 0x4000 && off < 0x6000) {
        if (write) scc_write(off & 6, data & 0xff); else r = scc_read(off & 6);
    } else if (off >= 0x10000 && off < 0x12000) {
        uint32_t reg = (off >> 4) & 7;
        if (write) scsi_write(reg, data & 0xff); else r = scsi_read(reg);
    } else if ((off >= 0x12000 && off < 0x14000) || (off >= 0x6000 && off < 0x8000)) {
        if (write) { for (int i = 0; i < size; i++) pdma_write((data >> (8 * (size - 1 - i))) & 0xff); }
        else { for (int i = 0; i < size; i++) r = (r << 8) | pdma_read(); }
        unlock();
        *val = r;
        return 1;
    } else if (off >= 0x16000 && off < 0x18000) {
        uint32_t reg = (off >> 9) & 15;
        r = iwm_access(reg, write, data & 0xff);
    } else handled = 0;
    unlock();
    if (!handled) return 0;
    if (!write) *val = size == 1 ? r : size == 2 ? (r << 8) | r : (r << 24) | (r << 16) | (r << 8) | r;
    return 1;
}

/* the 5380's interrupt line, for the RBV's flag bit 3 (psmac_board_se.c): the ROM's SCSI Manager
   enables that bit and waits for the level-2 interrupt at the end of every transfer; without it
   each command completed on a timeout and System 7 took a second per block (docs/08, item 35) */
int psmac_solo_scsi_irq(void) { return n.irq; }

/* the level the board drives: the VIA on 1, the RBV (psmac_board_se.c) on 2, nothing on 4 */
unsigned psmac_solo_ipl(void)
{
    unsigned ipl = via_irq() ? 1 : 0;
    if (psmac_se_rbv_irq() && ipl < 2) ipl = 2;
    return ipl;
}

/* the service core: time passes for the chips, the JIT learns the level */
extern struct M68KState *__m68k_state;
void psmac_solo_service(void)
{
    if (!psmac_board_solo) return;
    lock(); via_timers(via_now()); unlock();
    unsigned ipl = psmac_solo_ipl();
    if (__m68k_state) __m68k_state->INTF.IPL = ipl;
}
void psmac_solo_reset(void)
{
    lock();
    uint64_t now = via_now();
    via.orb = via.ora = via.ddrb = via.ddra = via.acr = via.pcr = via.ifr = via.ier = 0; via.t1_run = via.t2_run = 0;
    via.next_vbl = now + VBL_TICKS; rtc.next_second = now + VIA_HZ;
    adb.state = ADB_IDLE; adb.irq = 0; adb.pending = 0; adb.datasize = 0;
    for (int c = 0; c < 2; c++) { for (int i = 0; i < 16; i++) scc.wr[c][i] = 0; scc.ptr[c] = 0; }
    iwm.lines = 0;
    n.odr = n.icr = n.mr = n.tcr = n.ser = 0; n.dma = n.irq = n.busy_err = n.aip = n.ack = 0;
    t.phase = PH_FREE; t.req = 0; t.d = 0;
    unlock();
}
void psmac_solo_init(void)
{
    t_base = cnt_now();
    for (int i = 0; i < 256; i++) rtc.pram[i] = psmac_pram_default[i];
    rtc.seconds = 0xe7214980u;              /* 17 November 2026, 00:00, in the Mac's seconds since 1904 */
    rtc.enb = 1;
    static const char *const keys[] = { "mac.scsi0", "mac.scsi1", "mac.scsi2", "mac.scsi3", "mac.scsi4", "mac.scsi5", "mac.scsi6" };
    for (int i = 0; i < 7; i++) { const char *p = psmac_option(keys[i]); if (p) disk_add_file(p); }
    psmac_solo_reset();
    kprintf("[PSMAC:SOLO] no Macintosh: VIA, RTC/PRAM, ADB (no devices), SCC, IWM (no drive) and an NCR 5380 answered here\n");
}
/* from psmac_setup_memory once the container has been read: the disks the Mac will find */
void psmac_solo_start(void)
{
    if (!psmac_board_solo) return;
    kprintf("[PSMAC:SOLO] %d SCSI disk%s%s\n", ndisk, ndisk == 1 ? "" : "s", ndisk ? ":" : " (put a *.img, *.hda or *.image file in the container)");
    for (int i = 0; i < ndisk; i++) kprintf("[PSMAC:SOLO]   SCSI %d: %s, %u blocks (%u MB)%s%s\n", i, disks[i].name, disks[i].blocks, disks[i].blocks / 2048, disks[i].ram ? ", in RAM (changes last until power off)" : ", host file", disks[i].writable ? "" : ", read-only");
}
void psmac_solo_options(const char *args)
{
    extern const char *find_token(const char *, const char *);
    const char *tok;
    if ((tok = find_token(args, "mac.board="))) { tok += 10; if (tok[0] == 's' && tok[1] == 'o' && tok[2] == 'l' && tok[3] == 'o') { psmac_board_solo = 1; psmac_board_se = 1; } }
    if ((tok = find_token(args, "mac.solotrace="))) { tok += 14; uint32_t v = 0; while (*tok >= '0' && *tok <= '9') v = v * 10 + (*tok++ - '0'); trace_left = v; }
}
void psmac_solo_report(void)
{
    if (!psmac_board_solo) return;
    kprintf("[PSMAC:SOLO] VIA IFR %02x IER %02x ACR %02x T1 %s T2 %s, %u VBLs | ADB state %d %u cmds | RTC %u cmds secs %08x | SCSI %s req %d, %u cmds, %u phase irqs, %u underflows | disk 0: %u read %u written\n",
            via.ifr, via.ier, via.acr, via.t1_run ? "on" : "off", via.t2_run ? "on" : "off", via.vbls, adb.state, adb.cmds, rtc.cmds, rtc.seconds,
            phase_name(t.phase), t.req, t.cmds, t.mismatches, t.underflows, disks[0].reads, disks[0].writes);
    kprintf("[PSMAC:SOLO]   SCC: %u writes; A (modem) WR1 %02x WR3 %02x WR4 %02x WR5 %02x WR9 %02x WR10 %02x WR11 %02x WR12 %02x WR14 %02x WR15 %02x | B (printer) WR1 %02x WR3 %02x WR4 %02x WR5 %02x WR10 %02x WR11 %02x WR12 %02x WR14 %02x WR15 %02x\n",
            scc.writes, scc.wr[1][1], scc.wr[1][3], scc.wr[1][4], scc.wr[1][5], scc.wr[1][9], scc.wr[1][10], scc.wr[1][11], scc.wr[1][12], scc.wr[1][14], scc.wr[1][15],
            scc.wr[0][1], scc.wr[0][3], scc.wr[0][4], scc.wr[0][5], scc.wr[0][10], scc.wr[0][11], scc.wr[0][12], scc.wr[0][14], scc.wr[0][15]);
    static uint32_t shown;
    if (t.ring_n != shown) {
        shown = t.ring_n;
        kprintf("[PSMAC:SOLO]   last SCSI commands (n: cdb -> status):");
        unsigned first = t.ring_n > 12 ? t.ring_n - 12 : 0;
        for (unsigned i = first; i < t.ring_n; i++) { unsigned k = i % 12; kprintf(" %u:%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x->%x", t.ring[k].n, t.ring[k].cdb[0], t.ring[k].cdb[1], t.ring[k].cdb[2], t.ring[k].cdb[3], t.ring[k].cdb[4], t.ring[k].cdb[5], t.ring[k].cdb[6], t.ring[k].cdb[7], t.ring[k].cdb[8], t.ring[k].cdb[9], t.ring[k].status); }
        kprintf("\n");
    }
}
