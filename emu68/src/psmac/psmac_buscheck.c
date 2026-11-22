/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_buscheck.c — the bus check on a compact Mac: mac.buscheck=1, before the 68k starts.
 *
 * Backstory (22 November 2026, docs/08 item 108). A tester's 512Ke on a socketed PiStorm drew its
 * screen right, but its reads came back wrong: 6,011 of 6,605 rows of the screen read back with wrong
 * words, the mouse's Y1 line chattered, the keyboard gave phantom keys. One log could not say whether
 * that was timing or a joint, RAM or the chips, random or the same each time, or whether a retry would
 * cure it. So the kernel can now stop before the Mac starts and measure the bus itself, the same way on
 * every machine, and put the numbers where a tester can photograph them: on the compact Mac's own
 * screen (written by the Pi, and the tester's writes work), on HDMI, in the log, and in buscheck.txt in
 * the card's root. The tests and their reading are psmac_buscheck_core.c (shared with the host test,
 * tools/buscheck_test.c); this file is the kernel's half: when to run, the bus, the font, the places the
 * report goes.
 *
 *   mac.buscheck=1      once, then the kernel waits (the Mac does not start in that boot; power off)
 *   mac.buscheck=loop   again and again until power-off, the screen and the file renewed each run
 *   mac.buscheck=force  the same on a transport that is not a PiStorm (the MAME testbed's bridge), and
 *                       force,loop
 * It runs on the SE, the Plus, the 512Ke, the 512K and the 128K (mac.board=se, plus, 512ke, 512k, 128k)
 * over the classic PiStorm's GPIO; it refuses politely on the solo board (there is no logic board) and,
 * unless forced, over the testbed's bridge, whose cycles are MAME's and not a bus.
 *
 * psmac_buscheck_run() is the whole check for any caller that owns the transport (the service core)
 * with the 68k stopped: the recovery mode can call it with a callback for each line of the report.
 */
#include <stdint.h>
#include "support.h"
#include "version.h"
#include "psmac.h"
#include "psmac_buscheck.h"

uint32_t psmac_wire_read(uint32_t addr, int size);
void psmac_wire_write(uint32_t addr, int size, uint32_t data, int posted);
uint32_t psmac_gpio_timeouts(void);
void psmac_gpio_clock(uint32_t *src_mhz, uint32_t *div, int *pi4);
int psmac_gpio_cpld(uint32_t *id, uint32_t *uc);   /* psmac_gpio.c: the JTAG IDCODE and usercode read at start */
const char *psmac_cpld_name(uint32_t uc);
int psmac_bus_uses_gpio(void);                      /* psmac_bus.c: the classic PiStorm's transport, not the bridge */
uint32_t psmac_se_ram_bytes(void);                  /* psmac_board_se.c */
int psmac_se_ram_from_option(void);
uint32_t psmac_se_fb_base(void);
const char *psmac_option(const char *key);
int psmac_card_put_file(const char *name, const char *text, uint32_t len);
int psmac_card_files_state(void);
void psmac_console_page(const char (*lines)[BC_COLS + 1], int n);   /* psmac_display.c: the HDMI screen, held */
extern int psmac_board_se, psmac_board_solo, psmac_board_plus, psmac_board_orig;
extern const uint8_t topaz8_chardata[]; extern const uint32_t topaz8_charloc[]; extern const int modulo;   /* Emu68's own 8 by 8 font */

static int bc_mode;                                 /* 0 off, 1 once, 2 loop */
static int bc_force, bc_parsed;
static volatile int bc_holding;                     /* the check has the board: the 68k must not start */
static uint32_t bc_runs;
static uint64_t bc_next;

static void parse(void)
{
    if (bc_parsed) return;
    bc_parsed = 1;
    const char *v = psmac_option("mac.buscheck");
    if (!v || !v[0] || v[0] == '0' || (v[0] == 'o' && v[1] == 'f')) return;
    bc_mode = 1;
    for (const char *p = v; *p; p++) {
        if (p[0] == 'l' && p[1] == 'o' && p[2] == 'o' && p[3] == 'p') bc_mode = 2;
        if (p[0] == 'f' && p[1] == 'o' && p[2] == 'r' && p[3] == 'c' && p[4] == 'e') bc_force = 1;
    }
}
/* why not, or NULL when it may run */
static const char *refusal(void)
{
    if (psmac_board_solo) return "the solo board has no Macintosh logic board behind the Pi: there is no bus to check";
    if (!psmac_board_se) return "it is for the compact Macs only (mac.board=se, plus, 512ke, 512k or 128k)";
    if (!psmac_bus_uses_gpio() && !bc_force) return "this is the testbed's bridge, not a PiStorm: its cycles are MAME's (mac.buscheck=force runs it anyway)";
    return 0;
}
int psmac_buscheck_wanted(void) { parse(); return bc_mode != 0; }
/* core 0, before it releases the 68k: the check will hold the board (decided from the options alone, so
   that it can be asked before the service core has started) */
int psmac_buscheck_will_hold(void) { parse(); return bc_mode && !refusal(); }
int psmac_buscheck_holding(void) { return bc_holding; }

/* ---- the bus, for the core ---- */
static inline uint64_t cnt(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t frq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static uint32_t b_rd(void *c, uint32_t a, int size) { (void)c; return psmac_wire_read(a, size); }
static void b_wr(void *c, uint32_t a, int size, uint32_t v) { (void)c; psmac_wire_write(a, size, v, 0); }
static uint64_t b_ticks(void *c) { (void)c; return cnt(); }
static uint32_t b_faults(void *c) { (void)c; return psmac_gpio_timeouts(); }
void psmac_gpio_scc_unspaced(int on);
static void b_unspaced(void *c, int on) { (void)c; psmac_gpio_scc_unspaced(on); }

/* ---- the compact Mac's screen: 512 x 342, one bit a pixel, 1 is black ---- */
static uint8_t page[342 * 64];
static void draw(const char (*lines)[BC_COLS + 1], int n)
{
    for (uint32_t i = 0; i < sizeof page; i++) page[i] = 0;
    for (int row = 0; row < n && row < BC_ROWS; row++) {
        int invert = row == 0 || (lines[row][0] == 'S' && lines[row][1] == 'U' && lines[row][2] == 'M');   /* the title and SUMMARY bars */
        int ended = 0;
        for (int col = 0; col < BC_COLS; col++) {
            uint8_t c = ended ? 0 : (uint8_t)lines[row][col];
            if (!c) { ended = 1; c = ' '; }
            const uint8_t *data = &topaz8_chardata[(topaz8_charloc[(c >= 32 && c < 128) ? c - 32 : 0] >> 16) >> 3];
            for (int y = 0; y < 8; y++) {
                uint8_t bits = c > 32 ? data[y * modulo] : 0;
                page[(3 + row * 8 + y) * 64 + col] = invert ? (uint8_t)~bits : bits;   /* 3 rows of margin: 42 x 8 = 336 of 342 */
            }
        }
    }
    uint32_t base = psmac_se_fb_base();
    for (uint32_t i = 0; i < sizeof page; i += 2) psmac_wire_write(base + i, 2, (uint32_t)page[i] << 8 | page[i + 1], 1);
}

static const char *board_name(void)
{
    static const char *const orig[] = { "Macintosh Plus", "Macintosh 128K", "Macintosh 512K", "Macintosh 512Ke" };
    return psmac_board_plus ? orig[psmac_board_orig & 3] : "Macintosh SE";
}

static struct bc_results res;
static char lines[BC_ROWS][BC_COLS + 1];
static char text[4096];                             /* one cluster of any card the firmware boots (psmac_card.c) */

int psmac_buscheck_run(void (*report)(const char *line, void *arg), void *arg)
{
    static char link[96], version[64];
    uint32_t src = 0, div = 0; int pi4 = 0;
    if (psmac_bus_uses_gpio()) {
        psmac_gpio_clock(&src, &div, &pi4);
        if (src && div) bc_fmt(link, sizeof link, "GPIO, %s timings, CPLD clock %u MHz (PLLC %u / %u)", pi4 ? "Pi 4" : "Pi 3", src / div, src, div);
        else bc_fmt(link, sizeof link, "GPIO, %s timings, CPLD clock 200 MHz (PLLC unread, / %u)", pi4 ? "Pi 4" : "Pi 3", div);
    } else bc_fmt(link, sizeof link, "the MAME testbed's bridge (mac.buscheck=force): not a real bus");
    {   /* the build, so that photographs can be matched to kernels */
        char sha[8]; const char *g = GIT_SHA; int i = 0;
        for (; i < 7 && g[i]; i++) sha[i] = g[i];
        sha[i] = 0;
        bc_fmt(version, sizeof version, "%s %s %s", sha, __DATE__, __TIME__);
    }
    static char cpld[80];
    if (psmac_bus_uses_gpio()) {
        uint32_t id, uc;
        if (psmac_gpio_cpld(&id, &uc)) bc_fmt(cpld, sizeof cpld, "%08X %s", uc, psmac_cpld_name(uc));
        else bc_fmt(cpld, sizeof cpld, "usercode not read (mac.jtag=off)");
    } else bc_fmt(cpld, sizeof cpld, "none (the testbed's bridge)");
    struct bc_bus bus = { 0, b_rd, b_wr, b_ticks, frq(), psmac_bus_uses_gpio() ? b_faults : 0, 0, psmac_bus_uses_gpio() ? b_unspaced : 0 };
    uint32_t ram = psmac_se_ram_bytes();
    struct bc_config cfg = { ram, board_name(), psmac_se_ram_from_option() ? "mac.seram" : psmac_board_orig ? "the board" : "assumed: mac.seram=",
                             link, version, psmac_board_plus, ++bc_runs, 0x1d872b41u + bc_runs,
                             !psmac_board_plus ? 256u : (psmac_board_orig == 1 || psmac_board_orig == 2) ? 64u : 128u,   /* the SE's 256 KB, the Plus's and 512Ke's 128, the 128K's and 512K's 64 */
                             psmac_se_fb_base(), ram - 0x300u, psmac_board_plus,
                             psmac_bus_uses_gpio() ? 10000u : 500u,   /* the testbed's bridge is some tens of microseconds a cycle */
                             cpld };
    kprintf("[BUSCHECK] run %u: %s, %u KB of RAM, %s; the tests take a moment\n", bc_runs, cfg.board, cfg.ram_size >> 10, link);
    bc_run(&bus, &cfg, &res);
    int n = bc_report(&cfg, &res, lines, text, sizeof text);
    draw((const char (*)[BC_COLS + 1])lines, n);
    if (report) for (int i = 0; i < n; i++) report(lines[i], arg);
    else psmac_console_page((const char (*)[BC_COLS + 1])lines, n);
    /* the log, a line at a time */
    {
        char l[256]; uint32_t k = 0;
        for (uint32_t i = 0; text[i]; i++) {
            if (text[i] == '\n' || k == sizeof l - 1) { l[k] = 0; kprintf("[BUSCHECK] %s\n", l); k = 0; if (text[i] == '\n') continue; }
            l[k++] = text[i];
        }
        if (k) { l[k] = 0; kprintf("[BUSCHECK] %s\n", l); }
    }
    uint32_t len = 0; while (text[len]) len++;
    {   /* the card's writer keeps a small file in its first cluster: on a card with clusters under 4 KB (a small
           FAT16 one) the details are cut at a line's end */
        extern uint32_t psmac_card_cluster_bytes(void);
        uint32_t cl = psmac_card_cluster_bytes();
        if (cl && len > cl) { len = cl; while (len && text[len - 1] != '\n') len--; }
    }
    int st = psmac_card_files_state() == 1 ? -2 : psmac_card_put_file("buscheck.txt", text, len);
    kprintf("[BUSCHECK] verdict: %s | %s\n", bc_headline(&cfg, &res), st == 0 ? "buscheck.txt queued for the card" : st == -1 ? "no writable card: buscheck.txt not saved" : "buscheck.txt not saved this run (the card is busy)");
    return res.verdict;
}

/* the service core, in psmac_se_start: once the GPIO transport is up and before the 68k is released.
   Returns 1 when the check has the board (the 68k is not to start), 0 to boot as usual. */
int psmac_buscheck_boot(void)
{
    parse();
    if (!bc_mode) return 0;
    const char *why = refusal();
    if (why) { kprintf("[BUSCHECK] mac.buscheck: not run, %s; booting as usual\n", why); return 0; }
    bc_holding = 1;
    __asm__ volatile("dmb ish" ::: "memory");
    kprintf("[BUSCHECK] mac.buscheck=%s: the Mac will not start in this boot; the report goes to its own screen, HDMI, this log and buscheck.txt\n",
            bc_mode == 2 ? (bc_force ? "force,loop" : "loop") : (bc_force ? "force" : "1"));
    psmac_buscheck_run(0, 0);
    bc_next = cnt() + frq() * 10;
    return 1;
}
/* the service core's loop while the check holds the board: mac.buscheck=loop runs again at once; =1 only
   writes the report to the screen again every ten seconds (a stray word on the glass goes) */
void psmac_buscheck_service(void)
{
    if (!bc_holding) return;
    if (bc_mode == 2) { psmac_buscheck_run(0, 0); return; }
    if (cnt() < bc_next) return;
    bc_next = cnt() + frq() * 10;
    draw((const char (*)[BC_COLS + 1])lines, BC_ROWS);
}
