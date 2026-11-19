/*
 * psmac_boot.c — the boot timeline: one "[BOOT] t=N.NNNs what" line per phase. MPL-2.0.
 *
 * The Pi is the Macintosh SE's processor: from the moment the SE is switched on nothing happens on
 * the Mac until the Pi's firmware has loaded, the kernel has set itself up and the 68k is released.
 * The user asked for that to be as short as possible (19 November 2026), and the first thing that
 * needed was a clock on every step.
 *
 * What t counts from. The BCM2835 family's system timer (the 1 MHz counter at peripheral offset
 * 0x3000) is started by the SoC's reset and is never reset by the firmware: bootcode.bin and
 * start.elf use it for their own delays. So on real hardware t is the time since power-on (more
 * exactly, since the SoC came out of reset), and the first line, "kernel entry", already holds the
 * firmware's whole part: the boot ROM, bootcode.bin, start.elf, config.txt, the device tree, the
 * kernel and the initramfs read from the card. Under QEMU the same timer starts when QEMU starts, so
 * there t at kernel entry is close to zero and says nothing about a real firmware.
 *
 * How it is read. The ARM's own generic timer (CNTPCT_EL0) is one instruction away on every core,
 * where the system timer is a peripheral that is not mapped until platform_init has run. So each
 * mark reads only the generic timer, and the two are tied together once, the first time the
 * peripherals are mapped: the system timer's value at kernel entry is its value then, minus the
 * generic timer's ticks since entry. Marks made before the console exists are kept and printed
 * once it does.
 *
 * The 68k's own phases (the ROM's checksum and RAM tests) are taken from the program counter the
 * service core already samples for its heartbeat (psmac_bus.c), so the JIT does no extra work;
 * the price is that a phase's end is the last sample seen inside it, good to a millisecond or so.
 */
#include <stdint.h>
#include <stdarg.h>
#include "support.h"
#include "psmac.h"

#define SYSTIMER_CLO  (PSMAC_PERIPH_HI + 0x3004u)
#define SYSTIMER_CHI  (PSMAC_PERIPH_HI + 0x3008u)

static inline uint64_t gt_now(void) { uint64_t v; __asm__ volatile("isb; mrs %0, CNTPCT_EL0" : "=r"(v)); return v; }
static inline uint64_t gt_frq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v & 0xffffffffu; }

static uint64_t gt_entry;                  /* the generic timer at kernel entry */
static int64_t st_entry_us = -1;           /* the system timer at kernel entry (-1: not tied yet) */
static int console_up;

#define EARLY_MAX 10
static struct { uint64_t at; char text[88]; } early[EARLY_MAX];
static int nearly;

/* first thing in boot() (start.c) */
void psmac_boot_entry(void) { gt_entry = gt_now(); }

/* once the peripherals are mapped (start.c, after platform_init): tie the two timers together */
void psmac_boot_peripherals(void)
{
    uint32_t hi = rd32le(SYSTIMER_CHI), lo = rd32le(SYSTIMER_CLO);
    if (hi != rd32le(SYSTIMER_CHI)) { hi = rd32le(SYSTIMER_CHI); lo = rd32le(SYSTIMER_CLO); }
    uint64_t gt = gt_now(), st = (uint64_t)hi << 32 | lo;
    st_entry_us = (int64_t)st - (int64_t)((gt - gt_entry) * 1000000u / gt_frq());
    if (st_entry_us < 0) st_entry_us = 0;
}

/* microseconds since power-on (or since kernel entry, before the timers are tied) */
static uint64_t t_us(uint64_t gt) { return (st_entry_us > 0 ? (uint64_t)st_entry_us : 0) + (gt - gt_entry) * 1000000u / gt_frq(); }
uint64_t psmac_boot_now_us(void) { return t_us(gt_now()); }

struct sbuf { char *s; unsigned n, cap; };
static void sb_putc(void *d, char c) { struct sbuf *b = d; if (b->n + 1 < b->cap) b->s[b->n++] = c; }

static void print_line(uint64_t gt, const char *text)
{
    uint64_t us = t_us(gt);
    kprintf("[BOOT] t=%u.%03us %s\n", (uint32_t)(us / 1000000u), (uint32_t)(us / 1000u % 1000u), text);
}

static void mark_at(uint64_t gt, const char *fmt, va_list ap)
{
    char text[88]; struct sbuf b = { text, 0, sizeof text };
    vkprintf_pc(sb_putc, &b, fmt, ap);
    text[b.n] = 0;
    if (!console_up) {
        if (nearly < EARLY_MAX) { early[nearly].at = gt; for (unsigned i = 0; i <= b.n; i++) early[nearly].text[i] = text[i]; nearly++; }
        return;
    }
    print_line(gt, text);
}

void psmac_boot_mark(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); mark_at(gt_now(), fmt, ap); va_end(ap);
}

/* the console is up (start.c, after setup_serial): what was marked before it, then a word on t */
void psmac_boot_console(void)
{
    console_up = 1;
    for (int i = 0; i < nearly; i++) print_line(early[i].at, early[i].text);
    nearly = 0;
    if (st_entry_us > 0)
        kprintf("[BOOT] (t counts from the SoC's reset, i.e. power-on: the system timer read %u.%03u s at kernel entry, which is the Pi firmware's part)\n",
                (uint32_t)(st_entry_us / 1000000), (uint32_t)(st_entry_us / 1000 % 1000));
    else
        kprintf("[BOOT] (t counts from kernel entry: the system timer was not readable)\n");
}

/* ---- the 68k's phases, from the service core's PC samples (psmac_bus.c) ---------------------- */
/* IIci ROM 368CADFE (docs/08 item 28, docs/03): StartTest calls the ROM checksum (test 1) at
   $40844690 and, after the startup chime, the RAM test at $408446BC once per memory chunk; both
   return through A6. The ranges are the two routines' code. */
#define ROM_CKSUM_LO  0x40844690u
#define ROM_CKSUM_HI  0x408446BCu
#define ROM_RAMT_LO   0x408446BCu
#define ROM_RAMT_HI   0x408447BAu

struct span { uint64_t first, last; int state; };     /* state 0 not seen, 1 inside, 2 reported */
static struct span cksum, ramtest;

static void span_sample(struct span *s, int inside, uint64_t now, const char *start, const char *end)
{
    if (inside) {
        if (s->state == 0) { s->state = 1; s->first = now; psmac_boot_mark("%s", start); }
        if (s->state == 1) s->last = now;
        return;
    }
    /* outside: the phase is over once it has not been seen for a fifth of a second (the RAM test
       leaves its routine between memory chunks) */
    if (s->state == 1 && (now - s->last) * 5 > gt_frq()) {
        s->state = 2;
        char text[88]; struct sbuf b = { text, 0, sizeof text };
        kprintf_pc(sb_putc, &b, "%s (%u ms; the last PC sample inside it)", end, (uint32_t)((s->last - s->first) * 1000u / gt_frq()));
        text[b.n] = 0;
        print_line(s->last, text);
    }
}

/* every change of the sampled 68k PC (core 2) */
void psmac_boot_pc(uint32_t pc)
{
    uint64_t now = gt_now();
    span_sample(&cksum, pc >= ROM_CKSUM_LO && pc < ROM_CKSUM_HI, now, "68k: ROM checksum test running ($40844690)", "68k: ROM checksum test done");
    span_sample(&ramtest, pc >= ROM_RAMT_LO && pc < ROM_RAMT_HI, now, "68k: RAM test running ($408446BC)", "68k: RAM test done");
}

/* the first log line the Mac side sends (psmac_mbox.c): a driver's Open, then the INITs */
void psmac_boot_mac_line(const char *text)
{
    static int first, init;
    if (!first) { first = 1; psmac_boot_mark("Mac: first log line: %s", text); }
    if (!init) {
        for (const char *p = text; *p; p++)
            if (p[0] == 'I' && p[1] == 'N' && p[2] == 'I' && p[3] == 'T') { init = 1; psmac_boot_mark("Mac: first INIT's log line: %s", text); break; }
    }
}

/* the Finder: the Process Manager names the frontmost application in CurApName ($910), a Pascal
   string; the service core looks ten times a second until it says Finder (psmac_bus.c) */
void psmac_boot_curapname(const uint8_t *s)
{
    static int seen;
    if (seen || s[0] != 6) return;
    if (s[1] == 'F' && s[2] == 'i' && s[3] == 'n' && s[4] == 'd' && s[5] == 'e' && s[6] == 'r') { seen = 1; psmac_boot_mark("Mac: the Finder is running (CurApName)"); }
}
