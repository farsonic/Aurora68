/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/scsidev_test.c - the virtual SCSI devices (emu68/src/psmac/psmac_scsidev.c) on the host: the NCR 5380
 * driven the way the Mac's SCSI Manager drives it (arbitration, selection, the REQ/ACK handshake, the pseudo-DMA
 * window), a CD-ROM and a disk behind it, and a fake real bus behind the kernel for the diversion the SE board
 * makes (a selection of a virtual ID kept off the real bus, the real chip given back at the next arbitration, a
 * real device that answers a virtual ID turning the virtual one off). Built and run by `make -C mac check`.
 * Written 25 November 2026 (docs/08 item 148).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include "psmac_scsidev.h"

/* ---- the kernel's side ---- */
static int quiet;
void kprintf(const char *fmt, ...) { if (quiet) return; va_list a; va_start(a, fmt); printf("    | "); vprintf(fmt, a); va_end(a); }
static uint64_t clock_us;
uint64_t psmac_scsidev_host_ticks(void) { return clock_us += 3; }
uint32_t psmac_icache_epoch;
static const char *opts;                       /* the boot arguments */
const char *psmac_option_nth(const char *key, int nth)
{
    static char val[8][128]; static int slot;
    size_t kl = strlen(key);
    for (const char *s = opts; s && *s; s++) {
        if ((s == opts || s[-1] == ' ') && !strncmp(s, key, kl) && s[kl] == '=') {
            if (nth--) continue;
            char *o = val[slot = (slot + 1) & 7]; int k = 0;
            for (const char *v = s + kl + 1; *v > ' ' && k < 127; v++) o[k++] = *v;
            o[k] = 0; return o;
        }
    }
    return 0;
}
const char *psmac_option(const char *key) { return psmac_option_nth(key, 0); }
static FILE *files[8]; static int nfiles;
int64_t psmac_sh_open(const char *name, uint64_t mode) { FILE *f = fopen(name, mode == 3 ? "r+b" : "rb"); if (!f) return -1; files[nfiles] = f; return nfiles++; }
int psmac_sh_read(int64_t fd, void *buf, uint64_t len) { return fread(buf, 1, len, files[fd]) == len; }
int psmac_sh_write(int64_t fd, const void *buf, uint64_t len) { return fwrite(buf, 1, len, files[fd]) == len; }
int64_t psmac_sh_seek(int64_t fd, uint64_t pos) { return fseek(files[fd], (long)pos, SEEK_SET); }
int64_t psmac_sh_flen(int64_t fd) { long p = ftell(files[fd]); fseek(files[fd], 0, SEEK_END); long l = ftell(files[fd]); fseek(files[fd], p, SEEK_SET); return l; }
int psmac_sh_available(void) { return 1; }
int psmac_card_need(const void *p, uint32_t len) { (void)p; (void)len; return 0; }
static uint32_t dirty_calls;
void psmac_card_dirty(const void *p, uint32_t len) { (void)p; (void)len; dirty_calls++; }
void psmac_card_scan_wait(void) {}
/* the card: handle 0 is a disk image in memory, as the card's cache would serve it */
static uint8_t *card_img; static uint32_t card_secs;
int psmac_card_img_io(int h, int write, uint32_t lba, uint32_t n, void *buf, int wait)
{
    (void)wait;
    if (h != 0 || lba + n > card_secs) return -1;
    if (write) memcpy(card_img + (size_t)lba * 512, buf, (size_t)n * 512); else memcpy(buf, card_img + (size_t)lba * 512, (size_t)n * 512);
    return (int)n;
}

/* ---- checks ---- */
static int fails, checks;
#define OK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the Mac's side: the 5380 as the SCSI Manager drives it ---- */
static int on_se;                              /* the writes go through the SE board's diversion */
static void fake_wr(uint32_t r, uint8_t v);
static void wr(uint32_t r, uint8_t v) { if (on_se) { if (!psmac_scsidev_bus_write(r, v)) fake_wr(r, v); } else psmac_scsidev_write(r, v); }
static uint32_t fake_rd(uint32_t r);
static uint32_t bus_rd(uint32_t r) { return on_se && !psmac_scsidev_local() ? fake_rd(r) : psmac_scsidev_read(r); }
static int wait_req(void) { for (int i = 0; i < 1000; i++) if (bus_rd(4) & 0x20) return 1; return 0; }
static int phase(void) { return (bus_rd(4) >> 2) & 7; }
/* arbitration and selection, as the IIci ROM makes them; 1 if the target answered */
static int select_id(int id, int atn)
{
    wr(0, 0x80); wr(2, 0x01);                  /* our ID, ARBITRATE */
    if (!(bus_rd(1) & 0x40)) return 0;          /* arbitration in progress */
    wr(1, 0x0c);                               /* SEL and BSY */
    wr(0, (uint8_t)(0x80 | 1 << id));
    wr(1, (uint8_t)(0x0d | (atn ? 2 : 0)));    /* the data bus */
    wr(2, 0x00);
    wr(1, (uint8_t)(0x05 | (atn ? 2 : 0)));    /* BSY released: the selection */
    int ok = 0;
    for (int i = 0; i < 200; i++) if (bus_rd(4) & 0x40) { ok = 1; break; }
    wr(1, atn ? 0x02 : 0x00);                  /* SEL released */
    return ok;
}
static void put_byte(uint8_t b, int ph)
{
    wr(3, (uint8_t)ph);
    if (!wait_req()) return;
    wr(0, b); wr(1, (uint8_t)(0x11 | (bus_rd(1) & 0x02))); wr(1, (uint8_t)(0x01 | (bus_rd(1) & 0x02)));
    wr(1, 0x00);
}
static int get_byte(int ph)
{
    wr(3, (uint8_t)ph);
    if (!wait_req()) return -1;
    int v = (int)bus_rd(0); wr(1, 0x10); wr(1, 0x00);
    return v;
}
static uint8_t last_status, last_msg;
/* one command: its bytes, then data in (into in, up to inmax; pseudo-DMA when dma) or data out, status, message */
static int cmd(int id, const uint8_t *cdb, int len, uint8_t *in, int inmax, const uint8_t *out, int outlen, int dma)
{
    if (!select_id(id, 0)) return -1;
    for (int i = 0; i < len; i++) put_byte(cdb[i], 2);
    int got = 0;
    if (wait_req() && phase() == 1) {
        if (dma) { wr(3, 1); if (dma == 3) psmac_scsidev_pdma_write(0x5a); wr(2, 0x02); wr(7, 0); while (got < inmax && (bus_rd(5) & 0x40)) in[got++] = psmac_scsidev_pdma_read(); wr(2, 0); }
        else while (wait_req() && phase() == 1) { int v = get_byte(1); if (got < inmax) in[got] = (uint8_t)v; got++; }
    } else if (wait_req() && phase() == 0) {
        if (dma) { wr(3, 0); if (dma == 3) psmac_scsidev_pdma_write(0x5a); wr(2, 0x02); wr(5, 0); for (int i = 0; i < outlen && (bus_rd(5) & 0x40); i++) psmac_scsidev_pdma_write(out[i]); wr(2, 0); }
        else { int i = 0; while (wait_req() && phase() == 0) put_byte(i < outlen ? out[i++] : 0, 0); }
    }
    if (dma == 2) {                            /* status and message by pseudo-DMA too, as A/UX 3's driver reads the status */
        last_status = last_msg = 0xee;
        if (wait_req() && phase() == 3) { wr(3, 3); wr(2, 0x02); wr(7, 0); if ((bus_rd(5) & 0x48) == 0x48) last_status = psmac_scsidev_pdma_read(); wr(2, 0); }
        if (wait_req() && phase() == 7) { wr(3, 7); wr(2, 0x02); wr(7, 0); if ((bus_rd(5) & 0x48) == 0x48) last_msg = psmac_scsidev_pdma_read(); wr(2, 0); }
    } else {
    if (wait_req() && phase() == 3) last_status = (uint8_t)get_byte(3); else last_status = 0xee;
    if (wait_req() && phase() == 7) last_msg = (uint8_t)get_byte(7); else last_msg = 0xee;
    }
    OK(!(bus_rd(4) & 0x40), "bus free after the command %02x (bus status %02x)", cdb[0], bus_rd(4));
    return got;
}
static uint8_t sense_key(int id, uint8_t *asc)
{
    uint8_t c[6] = { 0x03, 0, 0, 0, 18, 0 }, s[18] = { 0 };
    cmd(id, c, 6, s, 18, 0, 0, 0);
    if (asc) *asc = s[12];
    return s[2] & 0x0f;
}

/* ---- the fake real bus behind the SE board: a 5380's registers and a real target at the IDs in real_ids ---- */
static uint8_t f_odr, f_icr, f_mr, f_tcr; static int f_sel_id = -1; static uint8_t real_ids; static uint32_t f_writes, f_sel_seen[8];
static void fake_wr(uint32_t r, uint8_t v)
{
    f_writes++;
    if (r == 0) f_odr = v;
    if (r == 2) f_mr = v;
    if (r == 3) f_tcr = v;
    if (r == 1) {
        f_icr = v;
        if ((v & 0x05) == 0x05 && !(v & 0x08) && f_sel_id < 0)
            for (int i = 0; i < 7; i++) if ((f_odr & (1 << i))) { f_sel_seen[i]++; if (real_ids & (1 << i)) f_sel_id = i; }
        if (!(v & 0x04) && f_sel_id >= 0 && !(v & 0x01)) { /* SEL released: the real target goes on alone; the test drops it */ }
    }
}
static uint32_t fake_rd(uint32_t r)
{
    if (r == 1) return f_icr | ((f_mr & 1) ? 0x40 : 0);
    if (r == 2) return f_mr;
    if (r == 4) return (f_sel_id >= 0 ? 0x40 : 0) | (f_icr & 0x04 ? 0x02 : 0) | (f_icr & 0x08 ? 0x40 : 0);
    return 0;
}
static uint32_t w_rd(uint32_t r) { return fake_rd(r); }
static void w_wr(uint32_t r, uint8_t v) { fake_wr(r, v); }
static const struct psmac_scsi_wire fake_wire = { w_rd, w_wr };

/* ---- the images ---- */
#define CD_FRAMES 600                           /* 1.2 MB */
static const char *cd_path = "/tmp/scsidev_test_cd.iso", *cd2_path = "/tmp/scsidev_test_cd2.iso";
static void make_cd(const char *path, uint8_t tag)
{
    FILE *f = fopen(path, "wb");
    static uint8_t s[512];
    for (int i = 0; i < CD_FRAMES * 4; i++) {
        for (int k = 0; k < 512; k++) s[k] = (uint8_t)(i * 7 + k + tag);
        s[0] = (uint8_t)(i >> 8); s[1] = (uint8_t)i; s[2] = tag;
        fwrite(s, 1, 512, f);
    }
    fclose(f);
}
static int sec_ok(const uint8_t *b, int i, uint8_t tag) { return b[0] == (uint8_t)(i >> 8) && b[1] == (uint8_t)i && b[2] == tag && b[100] == (uint8_t)(i * 7 + 100 + tag); }

static void test_cd(int id, int dma)
{
    uint8_t b[8192], c[12];
    /* INQUIRY: Apple's AppleCD 300 */
    memset(c, 0, sizeof c); c[0] = 0x12; c[4] = 36;
    int n = cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(n == 36 && b[0] == 5 && (b[1] & 0x80) && !memcmp(b + 8, "SONY    CD-ROM CDU-8003A1.9a", 28), "INQUIRY of the CD-ROM (%d bytes, type %d)", n, b[0]);
    OK(last_status == 0 && last_msg == 0, "INQUIRY's status %02x message %02x", last_status, last_msg);
    /* TEST UNIT READY, READ CAPACITY in 512-byte blocks (an Apple drive at power on) */
    memset(c, 0, sizeof c); cmd(id, c, 6, b, sizeof b, 0, 0, dma); OK(last_status == 0, "TEST UNIT READY with a CD");
    memset(c, 0, sizeof c); c[0] = 0x25;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 8 && (b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]) == CD_FRAMES * 4 - 1 && b[6] == 2 && b[7] == 0, "READ CAPACITY at 512: %02x%02x%02x%02x / %02x%02x", b[0], b[1], b[2], b[3], b[6], b[7]);
    /* READ(6) of block 0 and READ(10) of blocks 9-12, in 512-byte blocks */
    memset(c, 0, sizeof c); c[0] = 0x08; c[4] = 1;
    n = cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(n == 512 && sec_ok(b, 0, 1), "READ(6) block 0 at 512 (%d bytes)", n);
    memset(c, 0, sizeof c); c[0] = 0x28; c[5] = 9; c[8] = 4;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 2048 && sec_ok(b, 9, 1) && sec_ok(b + 1536, 12, 1), "READ(10) blocks 9-12 at 512");
    /* MODE SENSE(6): the block descriptor says 512 */
    memset(c, 0, sizeof c); c[0] = 0x1a; c[2] = 0x3f; c[4] = 255;
    n = cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(n > 12 && b[3] == 8 && (b[9] << 16 | b[10] << 8 | b[11]) == 512 && b[1] == 1 && b[2] == 0x80, "MODE SENSE(6) block descriptor 512, medium type 1, write-protected (%d bytes)", n);
    /* MODE SELECT(6) to 2048, as Apple's CD-ROM driver does */
    uint8_t ms[12] = { 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0x08, 0x00 };
    memset(c, 0, sizeof c); c[0] = 0x15; c[1] = 0x10; c[4] = 12;
    cmd(id, c, 6, 0, 0, ms, 12, dma);
    OK(last_status == 0, "MODE SELECT to 2048");
    memset(c, 0, sizeof c); c[0] = 0x25;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 8 && (b[2] << 8 | b[3]) == CD_FRAMES - 1 && b[6] == 8, "READ CAPACITY at 2048: %02x%02x / %02x%02x", b[2], b[3], b[6], b[7]);
    memset(c, 0, sizeof c); c[0] = 0x28; c[5] = 3; c[8] = 2;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 4096 && sec_ok(b, 12, 1) && sec_ok(b + 3584, 19, 1), "READ(10) frames 3-4 at 2048 (%d bytes)", n);
    /* READ(12) */
    memset(c, 0, sizeof c); c[0] = 0xa8; c[5] = 1; c[9] = 1;
    n = cmd(id, c, 12, b, sizeof b, 0, 0, dma);
    OK(n == 2048 && sec_ok(b, 4, 1), "READ(12) frame 1");
    /* MODE SELECT(10) back to 512 */
    uint8_t ms10[16] = { 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0x02, 0x00 };
    memset(c, 0, sizeof c); c[0] = 0x55; c[1] = 0x10; c[8] = 16;
    cmd(id, c, 10, 0, 0, ms10, 16, dma);
    OK(last_status == 0, "MODE SELECT(10) to 512");
    memset(c, 0, sizeof c); c[0] = 0x5a; c[2] = 0x0d; c[8] = 255;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 8 + 8 + 8 && (b[13] << 16 | b[14] << 8 | b[15]) == 512 && b[16] == 0x0d && b[21] == 60 && b[23] == 75, "MODE SENSE(10) page $0D at 512 (%d bytes)", n);
    /* a block size neither 512 nor 2048: refused */
    ms[10] = 0x09; ms[11] = 0x30;
    memset(c, 0, sizeof c); c[0] = 0x15; c[4] = 12;
    cmd(id, c, 6, 0, 0, ms, 12, dma);
    uint8_t asc = 0;
    OK(last_status == 2 && sense_key(id, &asc) == 5 && asc == 0x26, "MODE SELECT to 2352 refused (ILLEGAL REQUEST, invalid field in the parameter list)");
    /* READ TOC, LBA and MSF */
    memset(c, 0, sizeof c); c[0] = 0x43; c[8] = 100;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 20 && b[1] == 18 && b[2] == 1 && b[3] == 1 && b[5] == 0x14 && b[6] == 1 && b[14] == 0xaa && (b[18] << 8 | b[19]) == CD_FRAMES, "READ TOC (LBA): track 1, lead-out at %u", b[18] << 8 | b[19]);
    memset(c, 0, sizeof c); c[0] = 0x43; c[1] = 2; c[8] = 100;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 20 && b[9] == 0 && b[10] == 2 && b[11] == 0 && b[17] == 0 && b[18] == 10 && b[19] == 0, "READ TOC (MSF): 00:02:00 and the lead-out at 00:10:00");
    memset(c, 0, sizeof c); c[0] = 0x43; c[2] = 1; c[8] = 100;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 12 && b[2] == 1 && b[3] == 1 && b[6] == 1, "READ TOC session information");
    /* Apple's READ TOC ($C1): first and last track, the lead-out in BCD, track 1's start */
    memset(c, 0, sizeof c); c[0] = 0xc1; c[8] = 4;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 4 && b[0] == 1 && b[1] == 1, "Apple READ TOC: tracks 1 to 1");
    memset(c, 0, sizeof c); c[0] = 0xc1; c[5] = 0x40; c[8] = 4;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 4 && b[0] == 0x00 && b[1] == 0x10 && b[2] == 0x00, "Apple READ TOC: the lead-out at 00:10:00 in BCD (%02x:%02x:%02x)", b[0], b[1], b[2]);
    memset(c, 0, sizeof c); c[0] = 0xc1; c[2] = 1; c[5] = 0x80; c[8] = 4;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 4 && b[0] == 0x14 && b[1] == 0 && b[2] == 2 && b[3] == 0, "Apple READ TOC: track 1, data, at 00:02:00");
    memset(c, 0, sizeof c); c[0] = 0xcc;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 6 && b[0] == 5 && last_status == 0, "Apple AUDIO STATUS: idle");
    memset(c, 0, sizeof c); c[0] = 0x1a; c[2] = 0x30; c[4] = 255;
    n = cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(n == 12 + 24 && b[12] == 0xb0 && !memcmp(b + 14, "APPLE COMPUTER, INC", 19), "the CD's MODE SENSE page $30");
    /* an unknown command: CHECK CONDITION, ILLEGAL REQUEST, invalid command */
    memset(c, 0, sizeof c); c[0] = 0xd6; c[9] = 0;
    cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(last_status == 2 && sense_key(id, &asc) == 5 && asc == 0x20, "a vendor command: ILLEGAL REQUEST $20");
    /* a write to the CD: refused */
    memset(c, 0, sizeof c); c[0] = 0x2a; c[8] = 1;
    cmd(id, c, 10, 0, 0, b, 512, dma);
    OK(last_status == 2 && sense_key(id, &asc) == 7 && asc == 0x27, "WRITE(10) to a CD: DATA PROTECT");
    memset(c, 0, sizeof c); c[0] = 0x06; c[2] = 2; c[4] = 22;
    n = cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(n == 22 && b[21] == 2, "Iomega's $06 on the CD: a write-protected disk");
    /* LUN 1 does not exist */
    memset(c, 0, sizeof c); c[0] = 0x12; c[1] = 0x20; c[4] = 36;
    n = cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(n == 36 && b[0] == 0x7f, "INQUIRY of LUN 1: no such unit ($%02x)", b[0]);
    memset(c, 0, sizeof c); c[1] = 0x20;
    cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(last_status == 2 && sense_key(id, &asc) == 5 && asc == 0x25, "TEST UNIT READY of LUN 1: LOGICAL UNIT NOT SUPPORTED");
    /* PREVENT, then an eject is refused; ALLOW, then the eject works and the drive is NOT READY */
    memset(c, 0, sizeof c); c[0] = 0x1e; c[4] = 1; cmd(id, c, 6, b, sizeof b, 0, 0, dma); OK(last_status == 0, "PREVENT MEDIUM REMOVAL");
    memset(c, 0, sizeof c); c[0] = 0x1b; c[4] = 2; cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(last_status == 2 && sense_key(id, &asc) == 5 && asc == 0x53, "an eject while the medium is held: refused");
    memset(c, 0, sizeof c); c[0] = 0x1e; cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    memset(c, 0, sizeof c); c[0] = 0x1b; c[4] = 2; cmd(id, c, 6, b, sizeof b, 0, 0, dma); OK(last_status == 0, "START STOP UNIT: eject");
    memset(c, 0, sizeof c); cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(last_status == 2 && sense_key(id, &asc) == 2 && asc == 0x3a, "TEST UNIT READY after the eject: NOT READY, MEDIUM NOT PRESENT");
    /* a new CD (the remote control's path): UNIT ATTENTION once, then the new CD's blocks */
    OK(psmac_scsidev_cd_request(cd2_path, id, 0) == 0, "a new CD asked for");
    memset(c, 0, sizeof c); cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(last_status == 2 && sense_key(id, &asc) == 6 && asc == 0x28, "TEST UNIT READY after the change: UNIT ATTENTION, medium changed");
    memset(c, 0, sizeof c); cmd(id, c, 6, b, sizeof b, 0, 0, dma); OK(last_status == 0, "then ready");
    memset(c, 0, sizeof c); c[0] = 0x08; c[3] = 5; c[4] = 1;
    n = cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(n == 512 && sec_ok(b, 5, 2), "the new CD's block 5");
    /* a bus reset: back to 512-byte blocks */
    ms[10] = 0x08; ms[11] = 0;
    memset(c, 0, sizeof c); c[0] = 0x15; c[4] = 12; cmd(id, c, 6, 0, 0, ms, 12, dma);
    wr(1, 0x80); wr(1, 0x00); bus_rd(7);         /* the SCSI bus reset, as the ROM makes it */
    memset(c, 0, sizeof c); c[0] = 0x25;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 8 && b[6] == 2, "READ CAPACITY after a bus reset: 512-byte blocks again");
    psmac_scsidev_cd_request(cd_path, id, 0);
    memset(c, 0, sizeof c); cmd(id, c, 6, b, sizeof b, 0, 0, dma); sense_key(id, 0);
}
static void test_disk(int id, int dma, uint8_t *img, uint32_t secs)
{
    uint8_t b[70000], c[12], asc;
    memset(c, 0, sizeof c); c[0] = 0x12; c[4] = 36;
    int n = cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(n == 36 && b[0] == 0 && !(b[1] & 0x80), "INQUIRY of the disk: type 0, fixed");
    memset(c, 0, sizeof c); c[0] = 0x25;
    n = cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(n == 8 && (uint32_t)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]) == secs - 1 && b[6] == 2, "READ CAPACITY of the disk");
    /* Apple's page $30, and the geometry pages */
    memset(c, 0, sizeof c); c[0] = 0x1a; c[2] = 0x30; c[4] = 255;
    n = cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(n == 12 + 24 && b[12] == 0xb0 && b[13] == 22 && !memcmp(b + 14, "APPLE COMPUTER, INC   ", 22), "MODE SENSE page $30: APPLE COMPUTER, INC");
    memset(c, 0, sizeof c); c[0] = 0x1a; c[1] = 8; c[2] = 0x04; c[4] = 255;
    n = cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(n == 4 + 24 && b[4] == 0x04 && b[9] == 16, "MODE SENSE page 4 without the block descriptor: 16 heads");
    memset(c, 0, sizeof c); c[0] = 0x1a; c[2] = 0x3f; c[4] = 255;
    n = cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(n == 12 + 12 + 16 + 24 + 24 + 20 + 12 + 24, "MODE SENSE all pages (%d bytes)", n);
    memset(c, 0, sizeof c); c[0] = 0x1a; c[2] = 0x2e; c[4] = 255;
    cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(last_status == 2 && sense_key(id, &asc) == 5 && asc == 0x24, "MODE SENSE of a page it has not: ILLEGAL REQUEST $24");
    /* WRITE(10) of 200 blocks (more than one 64 KB chunk), read back with READ(6) */
    static uint8_t w[200 * 512];
    for (unsigned i = 0; i < sizeof w; i++) w[i] = (uint8_t)(i * 31 + (i >> 9) + 5);
    memset(c, 0, sizeof c); c[0] = 0x2a; c[5] = 100; c[8] = 200;
    cmd(id, c, 10, 0, 0, w, sizeof w, dma);
    OK(last_status == 0, "WRITE(10) 200 blocks at 100");
    OK(!img || !memcmp(img + 100 * 512, w, sizeof w), "the image holds what was written");
    memset(c, 0, sizeof c); c[0] = 0x08; c[3] = 100; c[4] = 128;
    n = cmd(id, c, 6, b, sizeof b, 0, 0, dma);
    OK(n == 128 * 512 && !memcmp(b, w, 128 * 512), "READ(6) 128 blocks back");
    if (dma) {                                 /* the status and the message read by pseudo-DMA (A/UX 3) */
        memset(c, 0, sizeof c); c[0] = 0x08; c[3] = 100; c[4] = 2;
        n = cmd(id, c, 6, b, sizeof b, 0, 0, 2);
        OK(n == 2 * 512 && !memcmp(b, w, 2 * 512) && last_status == 0 && last_msg == 0, "READ(6) with status %02x and message %02x by pseudo-DMA", last_status, last_msg);
        memset(c, 0, sizeof c); c[0] = 0x1a; c[2] = 0x2e; c[4] = 255;
        cmd(id, c, 6, b, sizeof b, 0, 0, 2);
        OK(last_status == 2 && last_msg == 0, "CHECK CONDITION by pseudo-DMA: status %02x message %02x", last_status, last_msg);
        sense_key(id, 0);
        /* a write of the window before DMA mode is on (A/UX 3 does it once per transfer): not a data byte */
        static uint8_t k[512];
        for (unsigned i = 0; i < sizeof k; i++) k[i] = (uint8_t)(i * 7 + 3);
        memset(c, 0, sizeof c); c[0] = 0x0a; c[2] = 1; c[3] = 0x2c; c[4] = 1;
        cmd(id, c, 6, 0, 0, k, sizeof k, 3);
        OK(last_status == 0 && (!img || !memcmp(img + 300 * 512, k, 512)), "WRITE(6) with a window write before DMA mode: the block unshifted");
        memset(c, 0, sizeof c); c[0] = 0x08; c[2] = 1; c[3] = 0x2c; c[4] = 1;
        n = cmd(id, c, 6, b, sizeof b, 0, 0, 3);
        OK(n == 512 && !memcmp(b, k, 512) && last_status == 0, "READ(6) with a window write before DMA mode: the block back");
    }
    /* FORMAT UNIT with a defect list header and two defects: taken and dropped, the blocks kept */
    uint8_t fl[12] = { 0, 0x10, 0, 8, 0, 0, 0, 1, 0, 0, 0, 2 };
    memset(c, 0, sizeof c); c[0] = 0x04; c[1] = 0x10;
    cmd(id, c, 6, 0, 0, fl, 12, dma);
    OK(last_status == 0 && (!img || !memcmp(img + 100 * 512, w, 512)), "FORMAT UNIT with a defect list: no work, the blocks kept");
    /* VERIFY with BytChk, MODE SELECT with a parameter list longer than the target keeps */
    memset(c, 0, sizeof c); c[0] = 0x2f; c[1] = 2; c[5] = 100; c[8] = 2;
    cmd(id, c, 10, 0, 0, w, 1024, dma); OK(last_status == 0, "VERIFY with BytChk");
    static uint8_t big[700]; big[3] = 8; big[10] = 2;
    memset(c, 0, sizeof c); c[0] = 0x55; c[7] = 700 >> 8; c[8] = 700 & 255;
    uint8_t big10[700] = { 0 }; big10[7] = 8; big10[14] = 2;
    cmd(id, c, 10, 0, 0, big10, 700, dma); OK(last_status == 0, "MODE SELECT(10) of 700 bytes");
    memset(c, 0, sizeof c); c[0] = 0x15; c[4] = 12;
    uint8_t bad[12] = { 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0x08, 0 };
    cmd(id, c, 6, 0, 0, bad, 12, dma);
    OK(last_status == 2 && sense_key(id, &asc) == 5 && asc == 0x26, "a disk does not take 2048-byte blocks");
    /* out of range */
    memset(c, 0, sizeof c); c[0] = 0x28; c[2] = 0x7f; c[8] = 1;
    cmd(id, c, 10, b, sizeof b, 0, 0, dma);
    OK(last_status == 2 && sense_key(id, &asc) == 5 && asc == 0x21, "READ past the end: ILLEGAL REQUEST $21");
    memset(c, 0, sizeof c); c[0] = 0x35; cmd(id, c, 10, b, sizeof b, 0, 0, dma); OK(last_status == 0, "SYNCHRONIZE CACHE");
    memset(c, 0, sizeof c); c[0] = 0x37; c[8] = 4; n = cmd(id, c, 10, b, sizeof b, 0, 0, dma); OK(n == 4 && last_status == 0, "READ DEFECT DATA: an empty list");
    (void)big;
}
/* IDENTIFY with ATN, as A/UX and SCSI-2 drivers select */
static void test_identify(int id)
{
    uint8_t b[64];
    OK(select_id(id, 1), "selection with ATN");
    OK(wait_req() && phase() == 6, "message out after a selection with ATN (phase %d)", phase());
    wr(3, 6); wr(0, 0xc1); wr(1, 0x13); wr(1, 0x01); wr(1, 0x00);   /* IDENTIFY, disconnect allowed, LUN 1; ATN dropped with the byte */
    OK(wait_req() && phase() == 2, "command phase after IDENTIFY");
    uint8_t c[6] = { 0x12, 0, 0, 0, 36, 0 };
    for (int i = 0; i < 6; i++) put_byte(c[i], 2);
    int got = 0;
    while (wait_req() && phase() == 1) { int v = get_byte(1); if (got < 64) b[got] = (uint8_t)v; got++; }
    get_byte(3); get_byte(7);
    OK(got == 36 && b[0] == 0x7f, "INQUIRY after IDENTIFY LUN 1: no such unit");
}

static void solo(void)
{
    static char o1[512];
    snprintf(o1, sizeof o1, "mac.board=solo scsi.cd=%s scsi.disk=card.hda:1 scsi.disk=missing.hda:2 scsi.cdvendor=SONY", cd_path);
    opts = o1;
    psmac_scsidev_init(1);
    card_secs = 4096; card_img = calloc(card_secs, 512);
    static uint8_t ram[2048 * 512];
    psmac_scsidev_add_image(0, "solo.img", ram, -1, 0, sizeof ram, 1);
    psmac_scsidev_add_image(3, "clash.img", ram, -1, 0, sizeof ram, 1);   /* the CD's ID: refused */
    int w; const char *nm;
    OK((nm = psmac_scsidev_card_want(1, &w)) && !strcmp(nm, "card.hda") && w, "the disk at ID 1 wants card.hda from the card");
    OK(!psmac_scsidev_card_want(3, &w), "the CD (a host file) wants nothing from the card");
    psmac_scsidev_card_ready(1, 0, (uint64_t)card_secs * 512, 1);
    psmac_scsidev_card_missing(2);
    OK(psmac_scsidev_id_taken(3) && psmac_scsidev_id_taken(1) && !psmac_scsidev_id_taken(2), "IDs taken: 1 and 3, not 2 (no such file)");
    printf("solo board, register handshakes\n");
    test_cd(3, 0);
    test_disk(0, 0, ram, 2048);
    test_disk(1, 0, card_img, card_secs);
    test_identify(3);
    printf("solo board, pseudo-DMA\n");
    test_cd(3, 1);
    test_disk(1, 1, card_img, card_secs);
    OK(select_id(2, 0) == 0 && select_id(5, 0) == 0, "no target at IDs 2 and 5: the selection times out");
    OK(dirty_calls > 0, "writes to the image in memory marked for the card");
    psmac_scsidev_report();
    { char js[2048]; int k = psmac_scsidev_json(js, sizeof js); OK(k > 0 && strstr(js, "\"id\": 3, \"type\": \"cdrom\"") && strstr(js, "\"id\": 1, \"type\": \"disk\""), "the JSON lists the devices: %s", js); }
}
/* the SE board: a real bus behind the kernel, a real device at ID 6 and one at ID 4 (where a virtual disk is asked for) */
static void se(void)
{
    static char o1[512];
    snprintf(o1, sizeof o1, "mac.board=se scsi.cd=%s scsi.disk=card.hda:4,other.hda:5", cd_path);
    opts = o1;
    psmac_scsidev_init(0);
    psmac_scsidev_bus(&fake_wire, 0);
    card_secs = 4096; card_img = calloc(card_secs, 512);
    psmac_scsidev_card_ready(4, 0, (uint64_t)card_secs * 512, 1);
    psmac_scsidev_card_ready(5, 0, (uint64_t)card_secs * 512, 1);
    real_ids = 1 << 6 | 1 << 4;
    on_se = 1;
    printf("SE board: a real disk at ID 6\n");
    OK(select_id(6, 0) == 1 && !psmac_scsidev_local() && f_sel_seen[6] == 1, "the real disk at ID 6 answers on the real bus");
    f_sel_id = -1;                              /* (its connection, which is not this test's) */
    printf("SE board: the virtual CD at ID 3\n");
    uint32_t w0 = f_writes;
    test_cd(3, 0);
    OK(f_sel_seen[3] == 1, "ID 3's selection went to the real bus once, for the check (%u)", f_sel_seen[3]);
    test_cd(3, 1);
    OK(f_sel_seen[3] == 1, "and never again (%u)", f_sel_seen[3]);
    OK(f_writes > w0, "the real chip had every arbitration");
    printf("SE board: the real disk again, between virtual connections\n");
    OK(select_id(6, 0) == 1 && !psmac_scsidev_local() && f_sel_seen[6] == 2, "ID 6 answers on the real bus");
    f_sel_id = -1;
    printf("SE board: the virtual disk at ID 5\n");
    test_disk(5, 0, card_img, card_secs);
    test_disk(5, 1, card_img, card_secs);
    printf("SE board: a virtual disk asked for at ID 4, where a real device answers\n");
    quiet = 0;
    OK(select_id(4, 0) == 1 && !psmac_scsidev_local(), "ID 4's first selection: the real device answers on the real bus");
    f_sel_id = -1;
    char js[2048]; psmac_scsidev_json(js, sizeof js);
    OK(strstr(js, "\"id\": 4, \"type\": \"disk\"") && strstr(strstr(js, "\"id\": 4"), "\"off\": true"), "the virtual disk at ID 4 is off: %s", js);
    OK(select_id(4, 0) == 1 && !psmac_scsidev_local() && f_sel_seen[4] == 2, "and ID 4 stays the real device's");
    f_sel_id = -1;
    test_cd(3, 0);
    psmac_scsidev_report();
}
/* the SE board with nothing on its bus (no terminator, mac.sescsi=empty, the 128K): the 5380 answered here */
static void empty(void)
{
    static char o1[512];
    snprintf(o1, sizeof o1, "mac.board=se scsi.cd=%s", cd_path);
    opts = o1;
    psmac_scsidev_init(0);
    psmac_scsidev_bus(&fake_wire, 1);
    on_se = 1;
    real_ids = 1 << 6;
    uint32_t w0 = f_writes;
    OK(select_id(6, 0) == 0 && select_id(0, 0) == 0, "the empty bus: nobody at IDs 6 and 0");
    test_cd(3, 0);
    test_cd(3, 1);
    OK(f_writes == w0, "the real chip never touched");
}
int main(int argc, char **argv)
{
    make_cd(cd_path, 1); make_cd(cd2_path, 2);
    quiet = argc > 2;
    const char *m = argc > 1 ? argv[1] : "solo";
    if (!strcmp(m, "solo")) solo(); else if (!strcmp(m, "se")) se(); else empty();
    remove(cd_path); remove(cd2_path);
    if (fails) { printf("scsidev (%s): %d of %d checks failed\n", m, fails, checks); return 1; }
    printf("scsidev (%s): %d checks, all passed\n", m, checks);
    return 0;
}
