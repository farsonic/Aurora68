/*
 * psmac_crash.c — what the 68k was doing when the System's error dialog came up. MPL-2.0.
 *
 * "Sorry, a system error occurred" tells the user an error number and nothing else, and the Mac is
 * gone once they press Restart. The kernel outlives that restart, and so does its log (PiLog shows
 * the log's last 64 KB in the card window), so the kernel keeps the evidence: written 19 November
 * 2026, after Internet Explorer left idle for an hour on the user's Pi 3B came back to "bad F-Line
 * instruction", the error seen before in long sessions (docs/08 items 42 and 44).
 *
 * Two kinds of event are recorded, both from M68K_Exception (M68k_ExceptionEntry.c), where Emu68
 * raises every exception the translated code takes; the normal path of the JIT is not touched:
 *
 *  - an unexpected exception: bus and address errors, illegal instructions, zero divide, CHK,
 *    TRAPV, privilege violations, trace, F-line, coprocessor and format errors, uninitialised and
 *    spurious interrupts, the FPU's exceptions. Not interrupts and TRAP #n, which are normal, and
 *    not A-line, which is every Toolbox call. A normal boot and a normal session raise none of
 *    these, so each one gets a full block: what and where (ROM, system heap, the application's
 *    heap or stack, a card's space), the application (CurApName, $910) and its A5, the registers,
 *    the instruction words around the PC and the top of the stack, copied at once before the
 *    Mac's handler moves anything, and the last 68k PCs the service core sampled.
 *  - _SysError ($A9C9), recognised among the A-line traps by its trap word (one 16-bit read per
 *    Toolbox call, beside the exception entry's own cost), for the errors the System or a program
 *    raises itself: out of memory, the stack in the heap, a bad launch and so on. The ROM's own
 *    exception handlers reach the dialog without the trap (a CrashTest program's F-line showed
 *    no _SysError), which is why the exception itself is the record that matters. "Welcome to
 *    Macintosh" (40) and the shutdown and disk alerts (20000 up) are _SysError calls too and are
 *    left out, and so is 30 (dsReinsert): the File Manager's "Please insert the disk" for an offline
 *    volume, which a USB stick ejected or pulled out brings (psmac_usbdisk.c; a line of its own).
 *
 * The JIT core only fills the records and counts; all formatting and printing is on the service
 * core (psmac_crash_poll), so a crash costs the 68k a few dozen memory reads. At most eight blocks
 * are printed per power-on, then one-line notes for 40 more: a Mac that bombs in a loop must not
 * bury the first one.
 */
#include <stdint.h>
#include "support.h"

uint32_t psmac_mac_rd32(uint32_t logical, int *ok);     /* psmac_bus.c: RAM or ROM through the 68k's map */
void psmac_pcring_tail(unsigned n);                     /* psmac_bus.c: the last n sampled PCs */
void sprintf(char *buf, const char *format, ...);       /* support.c */

#define CRASH_BLOCKS_MAX 8
#define EXC_NOTES_MAX 40

struct crash_rec {
    uint32_t vec, pc, sr, ea, fault;
    uint32_t r[16];                         /* D0-D7, A0-A7; A7 as it was when the event came */
    uint32_t usp, isp;
    uint32_t code[12]; int code_ok;         /* PC-16 to PC+31: 24 instruction words */
    uint32_t stack[16]; int stack_ok;       /* the 16 longs at A7 */
};
static struct crash_rec exc, syserr;        /* the last unexpected exception; the last _SysError */
static int before_valid;                    /* an exception came since the previous _SysError */
static volatile uint32_t exc_seq, sys_seq;  /* bumped by the JIT core after a record is complete */

static void copy_longs(uint32_t *dst, uint32_t from, int n, int *ok)
{
    *ok = 1;
    for (int i = 0; i < n; i++) {
        int k; dst[i] = psmac_mac_rd32(from + 4 * i, &k);
        if (!k) { dst[i] = 0; *ok = 0; }
    }
}

static void fill(struct crash_rec *c, uint32_t vec, uint32_t pc, uint32_t sr, const uint32_t *regs,
                 uint32_t usp, uint32_t isp, uint32_t ea, uint32_t fault)
{
    c->vec = vec; c->pc = pc; c->sr = sr; c->ea = ea; c->fault = fault;
    for (int i = 0; i < 16; i++) c->r[i] = regs[i];
    c->usp = usp; c->isp = isp;
    copy_longs(c->code, (pc & ~1u) - 16, 12, &c->code_ok);
    copy_longs(c->stack, regs[15] & ~1u, 16, &c->stack_ok);
}

/* the JIT core, from M68K_Exception (M68k_ExceptionEntry.c), which has already chosen the
   exceptions worth recording: the unexpected ones and _SysError. vec is the vector offset. */
struct psmac_crash_args { uint32_t vec, pc, sr, ea, fault, usp, isp, regs[16]; };
volatile uint32_t psmac_fpu_traps, psmac_fpu_trap_pc;   /* mac.fpu=none: FPU instructions trapped (M68k_ExceptionEntry.c) */

void psmac_crash_exception(const struct psmac_crash_args *a)
{
    if (a->vec == 0x28) {                   /* _SysError */
        static uint32_t exc_seq_then;
        fill(&syserr, a->vec, a->pc, a->sr, a->regs, a->usp, a->isp, 0, 0);
        before_valid = exc_seq != exc_seq_then;
        exc_seq_then = exc_seq;
        __atomic_add_fetch(&sys_seq, 1, __ATOMIC_RELEASE);
        return;
    }
    fill(&exc, a->vec, a->pc, a->sr, a->regs, a->usp, a->isp, a->ea, a->fault);
    __atomic_add_fetch(&exc_seq, 1, __ATOMIC_RELEASE);
    extern volatile uint32_t psmac_exc_flag;    /* psmac_bus.c dumps the PC trail once, after the first */
    if (!psmac_exc_flag) psmac_exc_flag = a->vec | 0x10000;
}

static const char *vector_name(uint32_t vec)
{
    switch (vec) {
    case 0x08: return "bus error";
    case 0x0c: return "address error";
    case 0x10: return "illegal instruction";
    case 0x14: return "zero divide";
    case 0x18: return "CHK";
    case 0x1c: return "TRAPV";
    case 0x20: return "privilege violation";
    case 0x24: return "trace";
    case 0x28: return "A-line";
    case 0x2c: return "F-line";
    case 0x34: return "coprocessor protocol violation";
    case 0x38: return "format error";
    case 0x3c: return "uninitialised interrupt";
    case 0x60: return "spurious interrupt";
    default:   return vec >= 0xc0 && vec <= 0xd8 ? "FPU exception" : "exception";
    }
}

/* Inside Macintosh, "System Error Handler": the numbers the dialog shows */
static const char *syserr_name(int16_t id)
{
    switch (id) {
    case 1:  return "bus error";
    case 2:  return "address error";
    case 3:  return "illegal instruction";
    case 4:  return "zero divide";
    case 5:  return "CHK";
    case 6:  return "TRAPV";
    case 7:  return "privilege violation";
    case 8:  return "trace";
    case 9:  return "bad A-line instruction";
    case 10: return "bad F-line instruction";
    case 11: return "miscellaneous hardware exception";
    case 12: return "unimplemented trap";
    case 13: return "spurious interrupt";
    case 14: return "I/O system error";
    case 15: return "segment loader error";
    case 16: return "floating point error";
    case 25: return "out of memory";
    case 26: return "bad program launch";
    case 27: return "file system map trashed";
    case 28: return "stack has moved into application heap";
    case 51: return "unserviceable slot interrupt";
    default: return "";
    }
}

static uint32_t rd(uint32_t a, int *ok) { return psmac_mac_rd32(a, ok); }

/* for the remote's state (psmac_remote.c, docs/13): how many records came, and the last of each */
void psmac_crash_last(uint32_t *exc_n, uint32_t *exc_vec, uint32_t *exc_pc, const char **exc_what,
                      uint32_t *sys_n, int16_t *sys_id, const char **sys_what)
{
    *exc_n = __atomic_load_n(&exc_seq, __ATOMIC_ACQUIRE); *exc_vec = exc.vec; *exc_pc = exc.pc; *exc_what = vector_name(exc.vec);
    *sys_n = __atomic_load_n(&sys_seq, __ATOMIC_ACQUIRE); *sys_id = (int16_t)syserr.r[0]; *sys_what = syserr_name((int16_t)syserr.r[0]);
}
const char *psmac_crash_syserr_name(int16_t id) { return syserr_name(id); }


/* where an address lies, in the Macintosh's terms, from the Memory Manager's low-memory globals */
static const char *region(uint32_t a)
{
    int ok;
    if (a >= 0x40000000u && a < 0x50000000u) return "ROM";
    if (a >= 0x50000000u && a < 0x60000000u) return "I/O space";
    if (a >= 0xf0000000u) return "a NuBus card's space";
    if (a < 0x2800) return "low memory";
    uint32_t sz = rd(0x2a6, &ok), sz_end = ok ? rd(sz, &ok) : 0;
    if (ok && a >= sz && a < sz_end) return "the system heap";
    uint32_t az = rd(0x2aa, &ok), az_end = ok ? rd(az, &ok) : 0;
    if (ok && a >= az && a < az_end) return "the application's heap";
    uint32_t stack_base = rd(0x908, &ok);
    if (ok && az_end && a >= az_end && a < stack_base) return "the application's stack";
    uint32_t a5 = rd(0x904, &ok);
    if (ok && a >= stack_base && a < a5 + 0x8000) return "the application's A5 world";
    return "RAM outside the current application (another partition, or the Process Manager's heap)";
}

static void print_regs(const char *tag, const struct crash_rec *c)
{
    kprintf("[CRASH] %s D0-7 %08x %08x %08x %08x %08x %08x %08x %08x\n", tag,
            c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
    kprintf("[CRASH] %s A0-7 %08x %08x %08x %08x %08x %08x %08x %08x\n", tag,
            c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13], c->r[14], c->r[15]);
    kprintf("[CRASH] %s SR %04x USP %08x SSP %08x\n", tag, c->sr & 0xffff, c->usp, (c->sr & 0x2000) ? c->r[15] : c->isp);
    if (c->code_ok) {
        kprintf("[CRASH] %s code at PC-16:", tag);
        for (int i = 0; i < 12; i++)
            kprintf("%s%04x %04x", i == 4 ? " |" : " ", c->code[i] >> 16, c->code[i] & 0xffff);
        kprintf("\n");
    } else kprintf("[CRASH] %s code at PC: not readable\n", tag);
    if (c->stack_ok) {
        kprintf("[CRASH] %s stack at A7:", tag);
        for (int i = 0; i < 16; i++) kprintf("%s %08x", i == 8 ? "\n[CRASH]                " : "", c->stack[i]);
        kprintf("\n");
    } else kprintf("[CRASH] %s stack at A7: not readable\n", tag);
}

/* the application's name (CurApName, a Pascal string at $910) and its A5, for a block's first line */
static void print_header(const char *what, unsigned n)
{
    int ok; char name[33]; uint32_t w[8];
    copy_longs(w, 0x910, 8, &ok);
    unsigned len = ok ? (w[0] >> 24) : 0;
    if (len > 31) len = 0;                      /* not set yet (early in the boot) */
    for (unsigned i = 0; i < len; i++) {
        char c = (char)(w[(i + 1) / 4] >> (24 - 8 * ((i + 1) % 4)));
        name[i] = c >= 0x20 && c < 0x7f ? c : '?';
    }
    name[len] = 0;
    uint32_t a5 = rd(0x904, &ok);
    kprintf("[CRASH] %s in \"%s\", A5 %08x (crash record %u of at most %u)\n", what, name, a5, n, CRASH_BLOCKS_MAX);
}

/* the record and what led to it onto the card (aurora68.log, psmac_card.c), so it outlives a power cut */
static void save_log(void)
{
    extern int psmac_card_save_log(int);
    if (psmac_card_save_log(1) == 0) kprintf("[CRASH] the log goes to aurora68.log on the SD card\n");
}

/* the service core, every turn of its loop */
void psmac_crash_poll(void)
{
    static uint32_t exc_seen, sys_seen, blocks, notes;
    char what[96];
    uint32_t e = __atomic_load_n(&exc_seq, __ATOMIC_ACQUIRE);
    if (e != exc_seen) {
        uint32_t more = e - exc_seen - 1;
        exc_seen = e;
        if (blocks < CRASH_BLOCKS_MAX) {
            blocks++;
            sprintf(what, "%s (vector %03x) at PC %08x", vector_name(exc.vec), exc.vec, exc.pc);
            print_header(what, blocks);
            kprintf("[CRASH] the PC is in %s%s\n", region(exc.pc), more ? "; other exceptions came since the last record" : "");
            if (exc.vec == 0x08 || exc.vec == 0x0c) kprintf("[CRASH] access address %08x (fault %08x)\n", exc.ea, exc.fault);
            print_regs(" ", &exc);
            kprintf("[CRASH] last 68k PCs the service core saw (oldest first):\n");
            psmac_pcring_tail(32);
            save_log();
        } else if (notes < EXC_NOTES_MAX) {
            notes++;
            kprintf("[PSMAC:EXC] vector %03x (%s) at PC %08x SR %04x ea %08x fault %08x\n", exc.vec, vector_name(exc.vec),
                    exc.pc, exc.sr & 0xffff, exc.ea, exc.fault);
        }
    }
    uint32_t s = __atomic_load_n(&sys_seq, __ATOMIC_ACQUIRE);
    if (s == sys_seen) return;
    sys_seen = s;
    int16_t id = (int16_t)syserr.r[0];
    if (id == 20000 || id == 20001) {                    /* dsShutDownOrRestart, dsSwitchOffOrRestart: "you may now switch off" */
        extern void psmac_card_flush_soon(const char *);
        psmac_card_flush_soon("the Mac has shut down");  /* the disk images' last writes on the card before the power goes (psmac_card.c) */
    }
    if (id <= 0 || id == 40 || id >= 20000) return;     /* "Welcome to Macintosh", the shutdown and disk alerts */
    if (id == 30) {                                      /* dsReinsert: the disk-switch dialog, not a crash */
        kprintf("[PSMAC:DISK] the Mac asks for a disk that is offline (\"Please insert the disk\", system error 30): put the USB stick back in, or press Command-period\n");
        return;
    }
    if (blocks >= CRASH_BLOCKS_MAX) return;
    blocks++;
    sprintf(what, "System error %d (%s) called at PC %08x", id, syserr_name(id), syserr.pc);
    { extern void psmac_splash_alert(int, const char *, ...); psmac_splash_alert(0, "The Mac stopped: system error %d (%s)", id, syserr_name(id)); }
    print_header(what, blocks);
    kprintf("[CRASH] the call is in %s; %s\n", region(syserr.pc), before_valid ? "the exception recorded before it came first"
            : "no unexpected exception came before it (the System or the program asked for the dialog)");
    print_regs("_SysError:", &syserr);
    kprintf("[CRASH] last 68k PCs the service core saw (oldest first):\n");
    psmac_pcring_tail(32);
    save_log();
}
