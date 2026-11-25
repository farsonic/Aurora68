/* SPDX-License-Identifier: MPL-2.0
 * mac/scsitest/scsitest.c - "Aurora68 SCSI Test": the virtual SCSI devices (emu68/src/psmac/psmac_scsidev.c, docs/08
 * item 148) through the Mac's own SCSI Manager, reported in the Pi's log. A test program, not part of the release.
 *
 * Why it exists (25 November 2026). The virtual CD-ROM drive is meant for A/UX, whose kernel drives the 5380
 * itself, and for Apple's CD-ROM driver; neither is in the testbed's System 7.1 image. What can be shown without
 * them is the SCSI level: the commands a driver sends, through the ROM's SCSI Manager (_SCSIDispatch: SCSIGet,
 * SCSISelect, SCSICmd, SCSIRead and SCSIRBlind, SCSIWrite and SCSIWBlind, SCSIComplete), which drives the same
 * 5380 registers and pseudo-DMA windows A/UX does. Put in Startup Items, it looks at every SCSI ID:
 *   - every device: INQUIRY (its type and names) and READ CAPACITY, so a real disk beside the virtual ones shows;
 *   - a CD-ROM (type 5): TEST UNIT READY (and REQUEST SENSE after a CHECK CONDITION), READ CAPACITY in 512-byte
 *     blocks, READ(10) of blocks 0-63 (polled) with their CRC-32, MODE SENSE, MODE SELECT to 2048-byte blocks,
 *     READ CAPACITY again, READ(10) of frames 0-15 (blind) whose CRC-32 must be the same, frame 16 (an ISO 9660
 *     volume's descriptor, "CD001"), READ TOC, MODE SELECT back to 512 and READ CAPACITY;
 *   - a disk whose INQUIRY says "Virtual Disk" (scsi.disk=, never a real one): MODE SENSE of Apple's page $30,
 *     WRITE(10) of 64 blocks at block 1000 (blind) of a fixed pattern, READ(10) of them back (polled), compared.
 * A number in the program's name ("Aurora68 SCSI Test 60") makes it look three times, that many seconds apart (a CD
 * changed through the remote control in between shows as UNIT ATTENTION and a new CRC).
 * Every step is a "[MAC] SCSI Test:" line; the host checks the CRCs against the image files (and the disk image's
 * blocks 1000-1063 against the pattern: byte i is ((i * 37 + 11) ^ (i >> 8)) & 255).
 */
#include "toolbox.h"

short sc_get(void), sc_select(int id), sc_cmd(const void *cdb, int count), sc_complete(short *stat, short *msg, long wait), sc_xfer(void *tib, int sel);
int io_get_sr(void);
void io_set_sr(int sr);
enum { SC_READ = 5, SC_WRITE = 6, SC_RBLIND = 8, SC_WBLIND = 9 };
struct tib { short op; long p1, p2; } __attribute__((packed));     /* a transfer instruction: scInc 1, scStop 7 */
_Static_assert(sizeof(struct tib) == 10, "tib");

/* ---- the Pi's log, as mac/iotest writes it ---- */
static char logline[200];
static volatile unsigned char *win(void) { return (volatile unsigned char *)(LM_MMU32BIT ? 0xF9000000UL : 0x00900000UL); }
static unsigned long log_n;
static void log_add(const char *s) { while (*s && log_n < sizeof logline - 1) logline[log_n++] = *s++; }
static void log_num(long v)
{
	char b[12], *q = b + 11;
	unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
	*q = 0;
	do { *--q = '0' + u % 10; u /= 10; } while (u);
	if (v < 0) *--q = '-';
	log_add(q);
}
static void log_hex(unsigned long v, int digits)
{
	char b[9];
	for (int k = 0; k < digits; k++) b[k] = "0123456789abcdef"[(v >> (4 * (digits - 1 - k))) & 15];
	b[digits] = 0;
	log_add(b);
}
static void log_text(const unsigned char *p, int n) { char b[2] = { 0, 0 }; for (int i = 0; i < n; i++) { b[0] = p[i] >= ' ' && p[i] < 127 ? (char)p[i] : '.'; log_add(b); } }
static void log_start(int id) { log_n = 0; log_add("SCSI Test: "); if (id >= 0) { log_add("ID "); log_num(id); log_add(": "); } }
static void log_end(void)
{
	volatile unsigned char *c = win() + 0x1000;
	volatile unsigned long *rq = (volatile unsigned long *)c, *dn = (volatile unsigned long *)(c + 4);
	unsigned long k, req;
	if (*(volatile unsigned long *)win() != 0x50534D42UL) return;
	int sr = io_get_sr();
	io_set_sr(sr | 0x0700);
	req = *rq;
	for (k = 0; *dn != req && k < 2000000; k++) ;
	*(volatile unsigned short *)(c + 8) = 2;
	*(volatile unsigned long *)(c + 28) = (unsigned long)logline;
	*(volatile unsigned long *)(c + 32) = log_n;
	*rq = req + 1;
	for (k = 0; *dn != req + 1 && k < 4000000; k++) ;
	io_set_sr(sr);
}
static unsigned long crc32(unsigned long crc, const unsigned char *p, unsigned long n)
{
	crc = ~crc;
	while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1)); }
	return ~crc;
}
static unsigned char pattern(unsigned long i) { return (unsigned char)((i * 37 + 11) ^ (i >> 8)); }

/* ---- one command through the SCSI Manager ---- */
static short stat, msg;
static const char *how(int sel) { return sel == SC_RBLIND || sel == SC_WBLIND ? "blind" : "polled"; }
/* the result: the SCSI Manager's error (0 good) for the transfer or the completion; stat the status byte */
static short run(int id, const unsigned char *cdb, int len, void *buf, long n, int sel)
{
	struct tib t[2];
	short e = sc_get();
	stat = msg = -1;
	if (e) return e;
	if ((e = sc_select(id))) return e;
	if ((e = sc_cmd(cdb, len))) { sc_complete(&stat, &msg, 300); return e; }
	if (n) {
		t[0].op = 1; t[0].p1 = (long)buf; t[0].p2 = n;
		t[1].op = 7; t[1].p1 = 0; t[1].p2 = 0;
		e = sc_xfer(t, sel);
	}
	short e2 = sc_complete(&stat, &msg, 300);
	return e ? e : e2;
}
static unsigned char cdb[12], buf_small[256];
static unsigned char *big;
#define BIG 32768L
static void c6(unsigned char op, unsigned long lba, unsigned char n) { memset(cdb, 0, sizeof cdb); cdb[0] = op; cdb[1] = (unsigned char)(lba >> 16 & 0x1f); cdb[2] = (unsigned char)(lba >> 8); cdb[3] = (unsigned char)lba; cdb[4] = n; }
static void c10(unsigned char op, unsigned long lba, unsigned n) { memset(cdb, 0, sizeof cdb); cdb[0] = op; cdb[2] = (unsigned char)(lba >> 24); cdb[3] = (unsigned char)(lba >> 16); cdb[4] = (unsigned char)(lba >> 8); cdb[5] = (unsigned char)lba; cdb[7] = (unsigned char)(n >> 8); cdb[8] = (unsigned char)n; }
static unsigned long be32(const unsigned char *p) { return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3]; }
static void result(short e) { log_add(" -> "); if (e) { log_add("error "); log_num(e); } else { log_add("status "); log_num(stat); } }
static void sense(int id)
{
	c6(0x03, 0, 18); memset(buf_small, 0, 18);
	short e = run(id, cdb, 6, buf_small, 18, SC_READ);
	log_start(id); log_add("REQUEST SENSE"); result(e); log_add(": key "); log_num(buf_small[2] & 15); log_add(", ASC $"); log_hex(buf_small[12], 2); log_end();
}
static int capacity(int id, unsigned long *blocks, unsigned long *bs)
{
	memset(cdb, 0, sizeof cdb); cdb[0] = 0x25;
	short e = run(id, cdb, 10, buf_small, 8, SC_READ);
	*blocks = be32(buf_small) + 1; *bs = be32(buf_small + 4);
	log_start(id); log_add("READ CAPACITY"); result(e); log_add(": "); log_num((long)*blocks); log_add(" blocks of "); log_num((long)*bs); log_end();
	return !e && !stat;
}
static int block_size(int id, unsigned long bs)
{
	unsigned char p[12] = { 0, 0, 0, 8, 0, 0, 0, 0, 0, (unsigned char)(bs >> 16), (unsigned char)(bs >> 8), (unsigned char)bs };
	c6(0x15, 0, 12); cdb[1] = 0x10;
	memcpy(buf_small, p, 12);
	short e = run(id, cdb, 6, buf_small, 12, SC_WRITE);
	log_start(id); log_add("MODE SELECT to "); log_num((long)bs); log_add("-byte blocks"); result(e); log_end();
	return !e && !stat;
}
static unsigned long read_crc(int id, unsigned long lba, unsigned n, unsigned long bs, int sel, const char *what)
{
	c10(0x28, lba, n); memset(big, 0, BIG);
	short e = run(id, cdb, 10, big, (long)n * (long)bs, sel);
	unsigned long c = crc32(0, big, (unsigned long)n * bs);
	log_start(id); log_add("READ(10) "); log_add(what); log_add(", "); log_add(how(sel)); result(e); log_add(", CRC-32 "); log_hex(c, 8); log_end();
	return e || stat ? 0 : c;
}
static void cd_test(int id)
{
	unsigned long blocks, bs, a, b;
	memset(cdb, 0, sizeof cdb);
	short e = run(id, cdb, 6, 0, 0, SC_READ);
	log_start(id); log_add("TEST UNIT READY"); result(e); log_end();
	if (!e && stat == 2) { sense(id); memset(cdb, 0, sizeof cdb); e = run(id, cdb, 6, 0, 0, SC_READ); log_start(id); log_add("TEST UNIT READY again"); result(e); log_end(); }
	if (!e && stat == 2) { sense(id); return; }	/* no CD in the drive */
	if (!capacity(id, &blocks, &bs)) return;
	a = read_crc(id, 0, 64, bs, SC_READ, "blocks 0-63 (32 KB at 512)");
	c6(0x1a, 0, 255); cdb[2] = 0x3f;
	e = run(id, cdb, 6, buf_small, 255, SC_READ);
	log_start(id); log_add("MODE SENSE(6)"); result(e); log_add(": "); log_num(buf_small[0] + 1); log_add(" bytes, block descriptor's block length ");
	log_num((long)((unsigned long)buf_small[9] << 16 | buf_small[10] << 8 | buf_small[11])); log_add(", medium type $"); log_hex(buf_small[1], 2); log_end();
	if (!block_size(id, 2048)) return;
	capacity(id, &blocks, &bs);
	b = read_crc(id, 0, 16, 2048, SC_RBLIND, "frames 0-15 (32 KB at 2048)");
	log_start(id); log_add(a && a == b ? "the same 32 KB at 512 and at 2048: good" : "the 32 KB at 512 and at 2048 differ: BAD"); log_end();
	c10(0x28, 16, 1);
	e = run(id, cdb, 10, big, 2048, SC_RBLIND);
	log_start(id); log_add("frame 16"); result(e); log_add(": $"); log_hex(big[0], 2); log_add(" '"); log_text(big + 1, 5); log_add("'");
	if (!memcmp(big + 1, "CD001", 5)) { log_add(", an ISO 9660 volume '"); log_text(big + 40, 16); log_add("', "); log_num((long)(big[80] | big[81] << 8 | (unsigned long)big[82] << 16)); log_add(" sectors"); }
	log_end();
	memset(cdb, 0, sizeof cdb); cdb[0] = 0x43; cdb[8] = 100;
	e = run(id, cdb, 10, buf_small, 20, SC_READ);
	log_start(id); log_add("READ TOC"); result(e); log_add(": tracks "); log_num(buf_small[2]); log_add("-"); log_num(buf_small[3]); log_add(", track 1 control $"); log_hex(buf_small[5], 2);
	log_add(", lead-out at frame "); log_num((long)be32(buf_small + 16)); log_end();
	block_size(id, 512);
	capacity(id, &blocks, &bs);
}
static void disk_test(int id)
{
	unsigned long blocks, bs;
	if (!capacity(id, &blocks, &bs) || blocks < 1100) return;
	c6(0x1a, 0, 255); cdb[2] = 0x30;
	short e = run(id, cdb, 6, buf_small, 36, SC_READ);
	log_start(id); log_add("MODE SENSE page $30"); result(e); log_add(": '"); log_text(buf_small + 14, 20); log_add("'"); log_end();
	for (long i = 0; i < BIG; i++) big[i] = pattern((unsigned long)i);
	unsigned long w = crc32(0, big, BIG);
	c10(0x2a, 1000, 64);
	e = run(id, cdb, 10, big, BIG, SC_WBLIND);
	log_start(id); log_add("WRITE(10) blocks 1000-1063, blind"); result(e); log_add(", CRC-32 "); log_hex(w, 8); log_end();
	unsigned long r = read_crc(id, 1000, 64, 512, SC_READ, "blocks 1000-1063 back");
	long bad = 0;
	for (long i = 0; i < BIG; i++) if (big[i] != pattern((unsigned long)i)) bad++;
	log_start(id); log_add(r == w && !bad ? "read back as written: good" : "read back differs: BAD, "); if (bad) { log_num(bad); log_add(" bytes"); } log_end();
}

#define LM_CURAPNAME ((const unsigned char *)0x910)
static long name_number(void)
{
	const unsigned char *n = LM_CURAPNAME;
	long v = 0;
	for (int i = 1; i <= n[0]; i++) { if (n[i] >= '0' && n[i] <= '9') v = v * 10 + (n[i] - '0'); else if (v) break; }
	return v;
}
static void idle(unsigned long ticks)
{
	EventRecord ev;
	unsigned long until = LM_TICKS + ticks;
	while (LM_TICKS < until) WaitNextEvent(everyEvent, &ev, 6, 0);
}
static void scan(void)
{
	for (int id = 0; id < 7; id++) {
		c6(0x12, 0, 36); memset(buf_small, 0, 36);
		short e = run(id, cdb, 6, buf_small, 36, SC_READ);
		if (e || stat) { log_start(id); log_add("nothing"); result(e); log_end(); continue; }
		log_start(id); log_add("type "); log_num(buf_small[0] & 0x1f); log_add(buf_small[1] & 0x80 ? ", removable, '" : ", '");
		log_text(buf_small + 8, 8); log_add("' '"); log_text(buf_small + 16, 16); log_add("' '"); log_text(buf_small + 32, 4); log_add("'"); log_end();
		if ((buf_small[0] & 0x1f) == 5) cd_test(id);
		else if (!(buf_small[0] & 0x1f) && !memcmp(buf_small + 16, "Virtual Disk", 12)) disk_test(id);
		else { unsigned long blocks, bs; capacity(id, &blocks, &bs); }
	}
}
int main(void)
{
	big = mac_newptr(BIG);
	if (!big) { log_start(-1); log_add("no memory"); log_end(); return 0; }
	long every = name_number(), passes = every > 0 ? 3 : 1;
	for (long p = 1; p <= passes; p++) {
		if (p > 1) idle((unsigned long)every * 60);
		log_start(-1); log_add("start, pass "); log_num(p); log_add(" of "); log_num(passes); log_end();
		scan();
	}
	log_start(-1); log_add("done"); log_end();
	return 0;
}
