/* SPDX-License-Identifier: MPL-2.0
 * mac/iotest/iotest.c - "Aurora68 IO Test": the SE's floppy drive and modem port, exercised from
 * the Mac and reported in the Pi's log. A test program, not part of the release (docs/08 item 93).
 *
 * Why it exists. An outside review of the SE board asked for evidence that the 800K floppy (the
 * SE's IWM) and the serial ports (its SCC) work through Aurora68's board layer, which forwards
 * every cycle of the Mac's to the real chips. Neither can be watched from the Pi: the Mac's own
 * drivers (.Sony, .AIn/.AOut) do the work. This program does it the way any application would and
 * writes what happened, with byte counts and CRC-32s, into the kernel's log through the card's
 * INFO service, so a run of the testbed (or of a real SE) is judged from the log and from the
 * floppy image afterwards.
 *
 * What it does, in order (each step one or more "[MAC] IO Test:" lines):
 *   1. The floppy. It waits for a volume named "Floppy" (up to five minutes), reads HOSTFILE from
 *      it whole if there is one (put there on the host; its CRC-32 is logged), writes MACFILE
 *      (64 KiB of a fixed pattern, CRC-32 logged), flushes the volume and reads MACFILE back,
 *      comparing every byte. Then it unmounts and ejects the disk, waits for a disk named
 *      "Floppy" to come back (the testbed reinserts it; on an SE, put it back in), and reads and
 *      compares MACFILE again. MACFILE can be copied off the image on the host (hcopy) and
 *      compared byte for byte with the same pattern.
 *   2. The modem port. It opens .AOut and .AIn, sets 8N1 at the speed in its own name ("Aurora68
 *      IO Test 57600"; 9600 when the name carries none), sends a line "A68SER <speed> BEGIN",
 *      4096 bytes of a fixed pattern and a line "A68SER END", then reads for up to 60 seconds,
 *      expecting 4096 bytes of a second pattern from the other end, and logs how many came, how
 *      many differ from the pattern, their CRC-32 and the driver's error flags (overruns, framing).
 *      tools/iotest_host.py is the other end in the testbed (MAME's null modem on a TCP socket).
 * A name containing "NoFloppy" or "NoSerial" leaves that part out. With "Sound" in the name a
 * 50-second tone plays through the Sound Manager (asynchronously) at the start, every 40 s while it waits for the disk and again at each floppy step,
 * so the SE's speaker buffer is being refilled while the IWM works: the case the review asked about
 * (the kernel's mac.buswait=1 files the 68k's waits by whether the speaker plays).
 *
 * The patterns, both ends: byte i of MACFILE and of the Mac's serial block is
 * ((i * 37 + 11) ^ (i >> 8)) & 255; byte i of the host's serial block is ((i * 13 + 5) ^ (i >> 7)) & 255.
 */
#include "toolbox.h"

/* ---- the OS calls (glue.s) ---- */
short io_PBOpen(void *pb), io_PBClose(void *pb), io_PBRead(void *pb), io_PBWrite(void *pb),
      io_PBControl(void *pb), io_PBStatus(void *pb), io_PBGetVInfo(void *pb), io_PBCreate(void *pb),
      io_PBDelete(void *pb), io_PBUnmountVol(void *pb), io_PBFlushVol(void *pb), io_PBEject(void *pb);
int io_get_sr(void);
void io_set_sr(int sr);
short io_SndNewChannel(void **chan, short synth, long init, void *userRoutine);
short io_SndPlay(void *chan, void *sndHandle, int async);
short io_SndDisposeChannel(void *chan, int quitNow);
void **io_NewHandle(long size);

/* the Device Manager's control and status block (Inside Macintosh II, Device Manager) */
typedef struct {
	void *qLink;
	short qType, ioTrap;
	void *ioCmdAddr, *ioCompletion;
	short ioResult;
	const unsigned char *ioNamePtr;
	short ioVRefNum, ioCRefNum, csCode;
	short csParam[11];
} CntrlParam;
_Static_assert(__builtin_offsetof(CntrlParam, csParam) == 28, "CntrlParam");
/* the volume information call's block (Inside Macintosh IV, File Manager): the fields used */
typedef struct {
	void *qLink;
	short qType, ioTrap;
	void *ioCmdAddr, *ioCompletion;
	short ioResult;
	unsigned char *ioNamePtr;
	short ioVRefNum;
	long filler;
	short ioVolIndex;
	long ioVCrDate, ioVLsBkUp;
	short ioVAtrb, ioVNmFls, ioVDirSt, ioVBlLn, ioVNmAlBlks;
	long ioVAlBlkSiz, ioVClpSiz;
	short ioAlBlSt;
	long ioVNxtFNum;
	short ioVFrBlk;
} VolumeParam;
_Static_assert(__builtin_offsetof(VolumeParam, ioVolIndex) == 28 && __builtin_offsetof(VolumeParam, ioVFrBlk) == 62, "VolumeParam");

#define LM_CURAPNAME ((const unsigned char *)0x910)

/* ---- the Pi's log: the card's INFO service, as the Aurora68 Agent writes it (mac/piagent) ---- */
static char logline[200];
static volatile unsigned char *win(void)
{
	return (volatile unsigned char *)(LM_MMU32BIT ? 0xF9000000UL : 0x00900000UL);
}
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
static void log_hex(unsigned long v)
{
	char b[9];
	for (int k = 0; k < 8; k++) b[k] = "0123456789abcdef"[(v >> (28 - 4 * k)) & 15];
	b[8] = 0;
	log_add(b);
}
static void log_start(void) { log_n = 0; log_add("IO Test: "); }
static void log_end(void)
{
	volatile unsigned char *c = win() + 0x1000;                 /* the INFO service's command block */
	volatile unsigned long *rq = (volatile unsigned long *)c, *dn = (volatile unsigned long *)(c + 4);
	unsigned long k, req;
	if (*(volatile unsigned long *)win() != 0x50534D42UL) return;   /* 'PSMB': not an Aurora68 Mac */
	int sr = io_get_sr();
	io_set_sr(sr | 0x0700);
	req = *rq;
	for (k = 0; *dn != req && k < 2000000; k++) ;
	*(volatile unsigned short *)(c + 8) = 2;                    /* PSMB_INFO_LOG */
	*(volatile unsigned long *)(c + 28) = (unsigned long)logline;
	*(volatile unsigned long *)(c + 32) = log_n;
	*rq = req + 1;
	for (k = 0; *dn != req + 1 && k < 4000000; k++) ;
	io_set_sr(sr);
}
static void say(const char *s) { log_start(); log_add(s); log_end(); }

/* ---- small things ---- */
static unsigned long crc32(unsigned long crc, const unsigned char *p, unsigned long n)
{
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
	}
	return ~crc;
}
static unsigned char mac_byte(unsigned long i) { return (unsigned char)((i * 37 + 11) ^ (i >> 8)); }
static unsigned char host_byte(unsigned long i) { return (unsigned char)((i * 13 + 5) ^ (i >> 7)); }
static int name_has(const char *w)
{
	const unsigned char *n = LM_CURAPNAME;
	unsigned long len = strlen(w);
	for (int i = 1; i + (int)len <= n[0] + 1; i++)
		if (!memcmp(n + i, w, len)) return 1;
	return 0;
}
static long name_number(void)
{
	const unsigned char *n = LM_CURAPNAME;
	long v = 0;
	for (int i = 1; i <= n[0]; i++) {
		if (n[i] >= '0' && n[i] <= '9') v = v * 10 + (n[i] - '0');
		else if (v) break;
	}
	return v;
}
/* let the system run for a while: the Finder mounts inserted disks from its own event loop */
static void idle(unsigned long ticks)
{
	EventRecord ev;
	unsigned long until = LM_TICKS + ticks;
	while (LM_TICKS < until) WaitNextEvent(everyEvent, &ev, 6, 0);
}

/* ---- the tone ("Sound" in the name) ----
   A format 1 'snd ' in memory (Inside Macintosh VI, Sound Manager): one sampled-sound synthesizer,
   one bufferCmd pointing at a sampled sound header 20 bytes in, then 8-bit unsigned samples at
   11,127 Hz, a 440 Hz square wave a little over 50 seconds long. */
#define TONE_LEN 560000UL
static void **tone;
static void *tone_chan;
static void tone_make(void)
{
	tone = io_NewHandle(20 + 22 + TONE_LEN);
	if (!tone) { say("no memory for the tone; no sound"); return; }
	unsigned char *p = *tone;
	static const unsigned char head[42] = {
		0x00, 0x01, 0x00, 0x01, 0x00, 0x05, 0x00, 0x00, 0x00, 0x80,   /* format 1, one synth: sampled (5), initMono */
		0x00, 0x01, 0x80, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14,   /* one command: bufferCmd with a data offset, 20 */
		0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0x2B, 0x77, 0x45, 0xD1,   /* samplePtr 0 (the data follow), length, 11127.27 Hz */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C }; /* no loop, standard encoding, base note 60 */
	memcpy(p, head, sizeof head);
	p[24] = (unsigned char)(TONE_LEN >> 24); p[25] = (unsigned char)(TONE_LEN >> 16); p[26] = (unsigned char)(TONE_LEN >> 8); p[27] = (unsigned char)TONE_LEN;
	for (unsigned long i = 0; i < TONE_LEN; i++) p[42 + i] = (i * 880 / 11127) & 1 ? 0xA0 : 0x60;
}
static unsigned long tone_t;
static void tone_play(void)
{
	if (!tone) return;
	tone_t = LM_TICKS;
	if (tone_chan) io_SndDisposeChannel(tone_chan, 1);
	tone_chan = 0;
	short e1 = io_SndNewChannel(&tone_chan, 5, 0x80, 0), e2 = e1 ? 0 : io_SndPlay(tone_chan, tone, 1);
	if (e1 || e2) { log_start(); log_add("tone: SndNewChannel "); log_num(e1); log_add(", SndPlay "); log_num(e2); log_end(); }
}

/* ---- 1. the floppy ---- */
#define FILE_LEN 65536UL
static const unsigned char vol_name[] = "\007Floppy:";
static const unsigned char mac_file[] = "\016Floppy:MACFILE";
static const unsigned char host_file[] = "\017Floppy:HOSTFILE";
static unsigned char *buf;

static int floppy_mounted(void)
{
	VolumeParam vp;
	unsigned char name[32];
	memcpy(name, vol_name, vol_name[0] + 1);
	memset(&vp, 0, sizeof vp);
	vp.ioNamePtr = name; vp.ioVolIndex = -1;
	return io_PBGetVInfo(&vp) == 0 ? vp.ioVFrBlk + 1 : 0;      /* free blocks + 1, 0 when absent */
}
static int wait_floppy(unsigned long ticks)
{
	unsigned long until = LM_TICKS + ticks;
	while (LM_TICKS < until) {
		if (floppy_mounted()) return 1;
		if (tone && LM_TICKS - tone_t > 40 * 60) tone_play();   /* the tone again before the last one ends */
		idle(30);
	}
	return 0;
}
/* the whole of a file into buf (at most FILE_LEN); the length, or a negative error */
static long read_file(const unsigned char *name)
{
	ParamBlock pb;
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = name; pb.ioPermssn = 1;
	short err = io_PBOpen(&pb);
	if (err) return err;
	short ref = pb.ioRefNum;
	pb.u.io.ioBuffer = buf; pb.u.io.ioReqCount = FILE_LEN; pb.u.io.ioPosMode = fsFromStart; pb.u.io.ioPosOffset = 0;
	err = io_PBRead(&pb);
	long got = pb.u.io.ioActCount;
	memset(&pb, 0, sizeof pb);
	pb.ioRefNum = ref;
	io_PBClose(&pb);
	return err && err != -39 ? err : got;                    /* -39: end of file, the short file read whole */
}
static void check_macfile(const char *when)
{
	tone_play();
	unsigned long t0 = LM_TICKS;
	memset(buf, 0, FILE_LEN);
	long got = read_file(mac_file);
	unsigned long bad = 0, first = 0;
	for (unsigned long i = 0; got > 0 && i < (unsigned long)got; i++)
		if (buf[i] != mac_byte(i) && !bad++) first = i;
	log_start(); log_add("MACFILE read back "); log_add(when); log_add(": ");
	if (got < 0) { log_add("error "); log_num(got); }
	else {
		log_num(got); log_add(" bytes, CRC-32 "); log_hex(crc32(0, buf, (unsigned long)got));
		if (got == (long)FILE_LEN && !bad) log_add(", every byte as written");
		else { log_add(", "); log_num((long)bad); log_add(" bytes differ (the first at "); log_num((long)first); log_add(")"); }
		log_add(", in "); log_num((long)(LM_TICKS - t0)); log_add(" ticks");
	}
	log_end();
}
/* Blocks 0 and 2 of drive 1 straight from the .Sony driver (reference number -5), whatever the
   File Manager thinks of the disk: its result code says what a failed mount could not ("This disk
   is unreadable" says nothing more), and block 2 of an HFS disk starts with the signature "BD". */
static void raw_blocks(void)
{
	tone_play();
	for (long blk = 0; blk <= 2; blk += 2) {
		ParamBlock pb;
		memset(&pb, 0, sizeof pb);
		memset(buf, 0, 512);
		pb.ioRefNum = -5; pb.ioVRefNum = 1;
		pb.u.io.ioBuffer = buf; pb.u.io.ioReqCount = 512; pb.u.io.ioPosMode = fsFromStart; pb.u.io.ioPosOffset = blk * 512;
		unsigned long t0 = LM_TICKS;
		short err = io_PBRead(&pb);
		log_start(); log_add("floppy: block "); log_num(blk); log_add(" from the .Sony driver: result "); log_num(err);
		log_add(", "); log_num(pb.u.io.ioActCount); log_add(" bytes, first long $"); log_hex((unsigned long)buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3]);
		log_add(", in "); log_num((long)(LM_TICKS - t0)); log_add(" ticks"); log_end();
	}
}
static void floppy_test(void)
{
	say("floppy: waiting for a disk named Floppy (five minutes at most)");
	if (!wait_floppy(60 * 60)) {
		raw_blocks();
		if (!wait_floppy(4 * 60 * 60)) { say("floppy: no volume named Floppy appeared; floppy test left out"); raw_blocks(); return; }
	}
	log_start(); log_add("floppy: Floppy is mounted, "); log_num(floppy_mounted() - 1); log_add(" free allocation blocks"); log_end();

	tone_play();
	unsigned long t0 = LM_TICKS;
	long got = read_file(host_file);
	log_start(); log_add("HOSTFILE: ");
	if (got < 0) { log_add("not read, error "); log_num(got); }
	else { log_num(got); log_add(" bytes, CRC-32 "); log_hex(crc32(0, buf, (unsigned long)got)); log_add(", in "); log_num((long)(LM_TICKS - t0)); log_add(" ticks"); }
	log_end();

	/* MACFILE, written afresh */
	ParamBlock pb;
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = mac_file;
	io_PBDelete(&pb);
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = mac_file;
	short err = io_PBCreate(&pb);
	if (!err) {
		memset(&pb, 0, sizeof pb);
		pb.ioNamePtr = mac_file;
		pb.u.f.fdType = TYPE('B', 'I', 'N', 'A'); pb.u.f.fdCreator = TYPE('A', '6', '8', 'T');
		mac_PBGetFInfo(&pb);
		pb.u.f.fdType = TYPE('B', 'I', 'N', 'A'); pb.u.f.fdCreator = TYPE('A', '6', '8', 'T');
		mac_PBSetFInfo(&pb);
		memset(&pb, 0, sizeof pb);
		pb.ioNamePtr = mac_file; pb.ioPermssn = fsWrPerm;
		err = io_PBOpen(&pb);
	}
	if (err) { log_start(); log_add("MACFILE: could not be created and opened, error "); log_num(err); log_end(); return; }
	short ref = pb.ioRefNum;
	for (unsigned long i = 0; i < FILE_LEN; i++) buf[i] = mac_byte(i);
	tone_play();
	t0 = LM_TICKS;
	pb.u.io.ioBuffer = buf; pb.u.io.ioReqCount = FILE_LEN; pb.u.io.ioPosMode = fsFromStart; pb.u.io.ioPosOffset = 0;
	err = io_PBWrite(&pb);
	long wrote = pb.u.io.ioActCount;
	memset(&pb, 0, sizeof pb);
	pb.ioRefNum = ref;
	short cerr = io_PBClose(&pb);
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = vol_name;
	short ferr = io_PBFlushVol(&pb);
	log_start(); log_add("MACFILE written: "); log_num(wrote); log_add(" bytes, CRC-32 "); log_hex(crc32(0, buf, FILE_LEN));
	log_add(", write "); log_num(err); log_add(", close "); log_num(cerr); log_add(", flush "); log_num(ferr);
	log_add(", in "); log_num((long)(LM_TICKS - t0)); log_add(" ticks"); log_end();
	check_macfile("at once");

	/* out and in again */
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = vol_name;
	err = io_PBUnmountVol(&pb);
	CntrlParam cp;
	memset(&cp, 0, sizeof cp);
	cp.ioVRefNum = 1;                                       /* drive 1: the internal floppy */
	short eerr = io_PBEject(&cp);
	log_start(); log_add("floppy: unmounted ("); log_num(err); log_add(") and ejected ("); log_num(eerr); log_add("); waiting for it to come back"); log_end();
	idle(120);
	if (!wait_floppy(5 * 60 * 60)) { say("floppy: the disk did not come back within five minutes"); return; }
	say("floppy: Floppy is mounted again");
	check_macfile("after the reinsertion");
}

/* ---- 2. the modem port ---- */
#define SER_LEN 4096UL
static short ser_ctl(short ref, short code, const short *param, int n)
{
	CntrlParam cp;
	memset(&cp, 0, sizeof cp);
	cp.ioCRefNum = ref; cp.csCode = code;
	for (int i = 0; i < n; i++) cp.csParam[i] = param[i];
	return io_PBControl(&cp);
}
static long ser_avail(short in)
{
	CntrlParam cp;
	memset(&cp, 0, sizeof cp);
	cp.ioCRefNum = in; cp.csCode = 2;                          /* SerGetBuf */
	if (io_PBStatus(&cp)) return 0;
	return (long)((unsigned long)(unsigned short)cp.csParam[0] << 16 | (unsigned short)cp.csParam[1]);
}
static short ser_write(short out, const void *p, long n)
{
	ParamBlock pb;
	memset(&pb, 0, sizeof pb);
	pb.ioRefNum = out; pb.u.io.ioBuffer = (void *)p; pb.u.io.ioReqCount = n;
	return io_PBWrite(&pb);
}
static void serial_test(void)
{
	long speed = name_number();
	short cfg_baud;
	switch (speed) {                                            /* the Serial Driver's baud codes (Inside Macintosh II) */
	case 300: cfg_baud = 380; break;
	case 1200: cfg_baud = 94; break;
	case 2400: cfg_baud = 46; break;
	case 4800: cfg_baud = 22; break;
	case 19200: cfg_baud = 4; break;
	case 38400: cfg_baud = 1; break;
	case 57600: cfg_baud = 0; break;
	default: speed = 9600; cfg_baud = 10; break;
	}
	short cfg = cfg_baud + 0x0C00 /* data8 */ + 0x2000 /* noParity */ + 0x4000 /* stop10 */;
	ParamBlock pb;
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = (const unsigned char *)"\005.AOut";
	short e1 = io_PBOpen(&pb), out = pb.ioRefNum;
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = (const unsigned char *)"\004.AIn";
	short e2 = io_PBOpen(&pb), in = pb.ioRefNum;
	if (e1 || e2) { log_start(); log_add("serial: .AOut "); log_num(e1); log_add(", .AIn "); log_num(e2); log_add(": not opened"); log_end(); return; }
	static unsigned char inbuf[8192];
	short setbuf[3] = { (short)((unsigned long)inbuf >> 16), (short)(unsigned long)inbuf, (short)sizeof inbuf };
	short r1 = ser_ctl(in, 9, setbuf, 3);                       /* SerSetBuf: room for the whole block */
	short r2 = ser_ctl(out, 8, &cfg, 1), r3 = ser_ctl(in, 8, &cfg, 1);   /* SerReset */
	log_start(); log_add("serial: modem port open at "); log_num(speed); log_add(" baud 8N1 (SerSetBuf "); log_num(r1);
	log_add(", SerReset "); log_num(r2); log_add("/"); log_num(r3); log_add(")"); log_end();
	idle(60);

	/* out */
	static const char begin[] = "\r\nA68SER       BEGIN\r\n", end[] = "\r\nA68SER END\r\n";
	char head[sizeof begin];
	memcpy(head, begin, sizeof begin);
	for (long v = speed, k = 13; v && k > 8; v /= 10, k--) head[k] = '0' + v % 10;
	for (unsigned long i = 0; i < SER_LEN; i++) buf[i] = mac_byte(i);
	unsigned long t0 = LM_TICKS;
	short w1 = ser_write(out, head, sizeof begin - 1), w2 = ser_write(out, buf, SER_LEN), w3 = ser_write(out, end, sizeof end - 1);
	log_start(); log_add("serial: sent "); log_num((long)SER_LEN); log_add(" bytes between the BEGIN and END lines, CRC-32 ");
	log_hex(crc32(0, buf, SER_LEN)); log_add(", in "); log_num((long)(LM_TICKS - t0)); log_add(" ticks (");
	log_num(w1); log_add("/"); log_num(w2); log_add("/"); log_num(w3); log_add(")"); log_end();

	/* in */
	unsigned long got = 0, until = LM_TICKS + 60 * 60, last = LM_TICKS;
	memset(buf, 0, SER_LEN);
	while (got < SER_LEN && LM_TICKS < until) {
		long n = ser_avail(in);
		if (n > 0) {
			if (n > (long)(SER_LEN - got)) n = SER_LEN - got;
			memset(&pb, 0, sizeof pb);
			pb.ioRefNum = in; pb.u.io.ioBuffer = buf + got; pb.u.io.ioReqCount = n;
			io_PBRead(&pb);
			got += pb.u.io.ioActCount;
			last = LM_TICKS;
		} else {
			if (got && LM_TICKS - last > 10 * 60) break;        /* ten seconds of silence after data */
			idle(2);
		}
	}
	unsigned long bad = 0, first = 0;
	for (unsigned long i = 0; i < got; i++)
		if (buf[i] != host_byte(i) && !bad++) first = i;
	CntrlParam cp;
	memset(&cp, 0, sizeof cp);
	cp.ioCRefNum = in; cp.csCode = 8;                          /* SerStatus: cumErrs in the first byte */
	io_PBStatus(&cp);
	unsigned errs = (unsigned short)cp.csParam[0] >> 8;
	log_start(); log_add("serial: received "); log_num((long)got); log_add(" of "); log_num((long)SER_LEN); log_add(" bytes, ");
	log_num((long)bad); log_add(" differ from the pattern");
	if (bad) { log_add(" (the first at "); log_num((long)first); log_add(")"); }
	log_add(", CRC-32 "); log_hex(crc32(0, buf, got)); log_add(", driver errors $"); log_hex(errs);
	log_add(errs & 0x21 ? " (overrun)" : ""); log_add(errs & 0x40 ? " (framing)" : ""); log_end();
	memset(&pb, 0, sizeof pb); pb.ioRefNum = in; io_PBClose(&pb);
	memset(&pb, 0, sizeof pb); pb.ioRefNum = out; io_PBClose(&pb);
}

int main(void)
{
	buf = mac_newptr(FILE_LEN);
	if (!buf) { say("no memory for the 64 KiB buffer"); return 0; }
	say("start");
	if (name_has("Sound")) { tone_make(); tone_play(); }
	if (!name_has("NoFloppy")) floppy_test();
	if (!name_has("NoSerial")) serial_test();
	if (tone_chan) io_SndDisposeChannel(tone_chan, 1);
	say("done");
	return 0;
}
