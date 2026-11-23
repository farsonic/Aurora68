/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_hdmi.c — the HDMI monitor and its mode, for PiDisplay on the Mac (docs/10, "PiDisplay").
 *
 * Backstory. Until 21 November 2026 the HDMI screen's mode was whatever config.txt said, and choosing
 * one meant taking the SD card out, reading about pixel clocks and editing config.txt on another
 * computer. The user's SE has two screens, its own and a Dell UP2716D on HDMI (2560 x 1440), and they
 * asked for a Mac program to choose the HDMI resolution and rotation and to show what the monitor
 * says about itself. This file is the kernel's half:
 *
 *   - At start (core 0) it reads the monitor's EDID from the firmware (psmac_vc_edid_block, the
 *     mailbox's "get EDID block", block after block), has psmac_edid.c parse and judge it, and logs
 *     a summary ([PSMAC:EDID] lines). QEMU has no monitor: mac.edidtest=sample gives the built-in
 *     made-up one, mac.edidtest=FILE (semihosting) any EDID file.
 *   - It never edits config.txt. The user adds "include aurora68-display.txt" to config.txt once, at
 *     its end, and the kernel writes only that small file in the card's root (psmac_card.c: the old
 *     one kept as aurora68-display.bak): hdmi_group, hdmi_mode or hdmi_cvt or hdmi_timings,
 *     max_framebuffer_width and _height, hdmi_drive, and two comment lines of its own the firmware
 *     skips, "#aurora68 rotate=N" and "#aurora68 confirm=WxH@R". The firmware reads config.txt only
 *     when the Pi starts, so a new mode means a restart of the whole Pi: PiDisplay asks for it
 *     (PSMB_INFO_DISPRESTART) and restarts the Mac through the Shutdown Manager, the 68k's RESET
 *     comes here (psmac_hdmi_bus_reset), and once the card has nothing left to write core 3 pulls the
 *     Pi's watchdog, as upstream PiStorm's housekeeper does when the Amiga's reset is held. The SE is
 *     reset too: the kernel's start pulses the motherboard's RESET (psmac_gpio.c).
 *   - After that restart the confirm mark is in the file (read on core 0 before the 68k starts,
 *     psmac_hdmi_early). PiScrn asks "Keep this resolution?" on the SE's own screen, which works
 *     whatever the monitor makes of the new mode, with a 15-second countdown. Keep takes the mark out;
 *     Revert, or no answer, puts the .bak back and restarts the Pi again. Should nothing on the Mac ask
 *     at all (no PiScrn), the kernel reverts by itself mac.displaywait= seconds after its start (300).
 *   - Rotation (0, 90, 180, 270) is the kernel's own: the conversion turns the picture as it copies
 *     the Mac's frame into the VideoCore's (psmac_display.c), and the Mac's screen is H x W at 90 and
 *     270. It is kept in the file and needs only a restart of the Mac: at the 68k's RESET the HDMI
 *     card's declaration ROM is rewritten for the turned size (psmac_bus.c, psmac_declrom_rescreen)
 *     before the Slot Manager reads it again.
 *
 * Core 0 runs psmac_hdmi_early, psmac_hdmi_init and psmac_hdmi_bus_reset; core 3 the mailbox commands
 * and psmac_hdmi_poll. Written for PiDisplay, 21 November 2026.
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"
#include "psmb.h"
#include "psmac_edid.h"

extern const char *psmac_option(const char *key);
extern int psmac_vc_edid_block(uint32_t block, uint8_t out[128]);
extern int psmac_card_early(void), psmac_card_writable(void), psmac_card_idle(void), psmac_card_files_state(void);
extern int32_t psmac_card_read_file(const char *name, uint8_t *buf, uint32_t max);
extern int psmac_card_put_file(const char *name, const char *text, uint32_t len);
extern int64_t psmac_sh_open(const char *name, uint64_t mode);
extern int psmac_sh_read(int64_t fd, void *buf, uint64_t len);
extern int64_t psmac_sh_flen(int64_t fd);
extern int psmac_sh_available(void);
extern int psmac_board_se, psmac_board_solo;
extern uint32_t psmac_vc_setup(uint32_t w, uint32_t h, uint32_t depth, void **fb, uint32_t *pitch_out);
extern void *framebuffer;
extern uint32_t pitch, fb_width, fb_height;

static inline uint64_t now_t(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t hz_t(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static uint32_t secs(void) { return (uint32_t)(now_t() / hz_t()); }

static struct psmb_dispinfo di __attribute__((aligned(16)));
static uint8_t edid[128 * 8];
static int nblocks;
/* config.txt whole: the release's own is 5.3 KB, and until 22 November 2026 this was 4 KB, so on a card with it
   psmac_card_read_file refused the file as too long and Aurora68 Display said "config.txt was not read" (found by
   Setup and Recovery's Display tab in the testbed, docs/08 item 111). 16 KB, as recovery's own reads of it. */
static char cfg_text[16384], disp_text[1536], bak_text[1536], new_text[1536];
static int32_t cfg_len = -1, disp_len = -1, bak_len = -1;
static int rotation, rotation_next, confirm_mark;
static int early_ok;                         /* the card was looked at before the 68k started */
static uint32_t wait_s = 300, start_s;
static uint32_t ask_deadline;                /* seconds: asking, and no answer by then means revert */
static volatile int op;                      /* the card job in progress: 0 none, 1 apply, 2 keep, 3 revert, 4 rotate */
static volatile int restart_armed, restart_due;
static uint32_t due_at;

static void msg(const char *s) { uint32_t i = 0; while (s[i] && i + 1 < sizeof di.message) { di.message[i] = s[i]; i++; } di.message[i] = 0; }
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

int psmac_hdmi_rotation(void) { return rotation; }

/* ---- core 0, before the 68k starts: config.txt and the kernel's file ---- */
/* the initramfs is a container (cpio "newc") rather than a bare ROM: the Pi booted the way it does on
   hardware, and the card is there to be read (a bare-ROM testbed run has none, and would wait for it) */
static int is_container(const uint8_t *p, uint32_t n) { return n > 6 && p[0] == '0' && p[1] == '7' && p[2] == '0' && p[3] == '7' && p[4] == '0' && p[5] == '1'; }
void psmac_hdmi_early(const void *initramfs, uint32_t len)
{
    start_s = secs();
    const char *w = psmac_option("mac.displaywait");
    if (w) { uint32_t v = 0; while (*w >= '0' && *w <= '9') v = v * 10 + (uint32_t)(*w++ - '0'); wait_s = v; }
    /* mac.cardearly=1 (the testbed): the card is looked at even after a boot from a bare ROM, as the solo
       board's runs with host files through semihosting do */
    const char *force = psmac_option("mac.cardearly");
    if ((!is_container((const uint8_t *)initramfs, len) && !(force && force[0] == '1')) || psmac_card_early()) return;
    early_ok = 1;
    cfg_len = psmac_card_read_file("config.txt", (uint8_t *)cfg_text, sizeof cfg_text - 1);
    disp_len = psmac_card_read_file("aurora68-display.txt", (uint8_t *)disp_text, sizeof disp_text - 1);
    bak_len = psmac_card_read_file("aurora68-display.bak", (uint8_t *)bak_text, sizeof bak_text - 1);
    struct edid_file f = { cfg_len >= 0 ? cfg_text : 0, cfg_len >= 0 ? (uint32_t)cfg_len : 0, disp_len >= 0 ? disp_text : 0, disp_len >= 0 ? (uint32_t)disp_len : 0 };
    static struct psmb_dispinfo scratch;
    rotation = edid_current(&scratch, &f, &confirm_mark);
    rotation_next = rotation;
    kprintf("[PSMAC:EDID] the card: config.txt %s%s, aurora68-display.txt %s, .bak %s; rotation %d%s\n",
            cfg_len >= 0 ? "read" : "not found", scratch.cfg & PSMB_CF_INCLUDE ? (scratch.cfg & PSMB_CF_OVERRIDE ? " (it includes aurora68-display.txt, but sets an hdmi_ mode after the include line, which wins)" : " (it includes aurora68-display.txt)") : cfg_len >= 0 ? " (no \"include aurora68-display.txt\" line: PiDisplay's choice is not used)" : "",
            disp_len >= 0 ? "read" : "not there yet", bak_len >= 0 ? "there" : "not there", rotation, confirm_mark ? "; a new mode waits to be confirmed" : "");
    /* The testbed: nothing reads config.txt before the kernel, so mac.hdmisim=1 does what the firmware
       would have, a frame buffer of the mode config.txt and its include set (QEMU's VideoCore makes one
       of any size it is asked for). A Pi restart then shows the new size as on hardware. */
    const char *sim = psmac_option("mac.hdmisim");
    uint32_t sw, sh;
    if (sim && sim[0] == '1' && edid_config_size(&f, &sw, &sh) && (sw != fb_width || sh != fb_height)) {
        void *fb = 0; uint32_t p = 0;
        uint32_t d = psmac_vc_setup(sw, sh, 16, &fb, &p);
        if (fb && p) { framebuffer = fb; pitch = p; fb_width = sw; fb_height = sh; }
        kprintf("[PSMAC:EDID] mac.hdmisim=1: the firmware's frame buffer made %ux%u (depth %u), as config.txt's HDMI mode would on a Pi\n", sw, sh, d);
    }
}

/* ---- core 0, as the display starts: the EDID ---- */
static void put_mhz(char *o, uint32_t khz) { uint32_t n = 0, v = khz / 1000, f = khz % 1000 / 10; char t[12]; int i = 0; do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v); while (i) o[n++] = t[--i]; o[n++] = '.'; o[n++] = (char)('0' + f / 10); o[n++] = (char)('0' + f % 10); o[n] = 0; }
static void put_hz(char *o, uint32_t c) { uint32_t n = 0, v = c / 100; char t[8]; int i = 0; do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v); while (i) o[n++] = t[--i]; if (c % 100) { o[n++] = '.'; o[n++] = (char)('0' + c % 100 / 10); if (c % 10) o[n++] = (char)('0' + c % 10); } o[n] = 0; }
static uint32_t isqrt(uint32_t v) { uint32_t r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }
static void log_summary(void)
{
    char a[16], b[16];
    if (!(di.flags & PSMB_DI_EDID)) {
        kprintf("[PSMAC:EDID] no EDID from the firmware (%s); Aurora68 Display shows the current mode only (mac.edidtest=sample in the testbed)\n",
                di.edid_status == PSMB_ES_HEADER ? "its first block is not an EDID" : "no monitor, or a firmware without the tag");
        return;
    }
    uint32_t diag10 = isqrt((uint32_t)di.size_w_mm * di.size_w_mm + (uint32_t)di.size_h_mm * di.size_h_mm) * 100 / 254;   /* tenths of an inch */
    kprintf("[PSMAC:EDID] %s: \"%s\", %s%s%s, product %04x, serial %u%s%s, week %u of %u, EDID %u.%u, %u blocks%s; %ux%u mm (%u.%u\")\n",
            di.flags & PSMB_DI_SAMPLE ? "the sample (mac.edidtest)" : "the monitor", di.name[0] ? di.name : "(no name)", di.maker,
            di.maker_name[0] ? ", " : "", di.maker_name, (unsigned)di.product, (unsigned)di.serial, di.serial_text[0] ? " / " : "", di.serial_text,
            di.week, di.year, di.edid_version >> 8, di.edid_version & 255, (unsigned)di.edid_blocks,
            di.edid_status == PSMB_ES_CHECKSUM ? " (a checksum is wrong)" : "", di.size_w_mm, di.size_h_mm, diag10 / 10, diag10 % 10);
    kprintf("[PSMAC:EDID] %s%s; sound: %s%s, %u channels, rates %x, bits %x, speakers %x; range %u-%u Hz, %u-%u kHz, %u MHz; TMDS %u MHz\n",
            di.conn & PSMB_CN_HDMI ? "HDMI" : "DVI (no HDMI vendor block: no sound)", di.conn & PSMB_CN_DC30 ? ", deep colour" : "",
            di.audio_formats & 2 ? "LPCM" : "none", di.audio_formats & ~2u ? " and compressed formats" : "", (unsigned)di.lpcm_channels,
            (unsigned)di.lpcm_rates, (unsigned)di.lpcm_bits, (unsigned)di.speakers, di.min_vhz, di.max_vhz, di.min_hkhz, di.max_hkhz,
            (unsigned)di.max_pixclk / 1000, (unsigned)di.tmds_max / 1000);
    int usable = 0, fast = 0, alts = 0, shape = 0;
    for (int i = 0; i < di.nmodes; i++) {
        const struct psmb_dispmode *m = &di.mode[i];
        usable += !!(m->flags & PSMB_DM_USABLE); fast += !!(m->flags & PSMB_DM_TOOFAST); alts += !!(m->flags & PSMB_DM_ALT); shape += !!(m->flags & PSMB_DM_SHAPE);
        put_hz(a, m->refresh); put_mhz(b, m->pixclk);
        char c[16] = "", d[16] = "";
        if (m->flags & PSMB_DM_ALT) { put_hz(c, m->alt_refresh); put_mhz(d, m->alt_pixclk); }
        kprintf("[PSMAC:EDID]   %ux%u%s at %s Hz, %s MHz (%s): %s%s%s%s%s%s%s%s%s\n", m->width, m->height, m->flags & PSMB_DM_INTERLACED ? "i" : "", a, b,
                edid_source_name(m->source), m->flags & PSMB_DM_USABLE ? "usable" : "too fast", m->flags & PSMB_DM_ALT ? ", " : "", c,
                m->flags & PSMB_DM_ALT ? " Hz with reduced blanking fits (" : "", d, m->flags & PSMB_DM_ALT ? " MHz)" : "",
                m->flags & PSMB_DM_SHAPE ? ", not the panel's shape" : "", m->flags & PSMB_DM_PREFERRED ? ", preferred" : "",
                m->flags & PSMB_DM_CURRENT ? ", in use" : "");
    }
    kprintf("[PSMAC:EDID] %u modes: %d usable by a Pi 3 (%u MHz), %d too fast (%d with a slower alternative), %d not the panel's shape\n",
            di.nmodes, usable, (unsigned)di.pi_pixclk / 1000, fast, alts, shape);
    if (di.suggested != 0xffff) {
        const struct psmb_dispmode *m = &di.mode[di.suggested];
        put_hz(a, m->flags & PSMB_DM_USABLE ? m->refresh : m->alt_refresh);
        kprintf("[PSMAC:EDID] suggested: %ux%u at %s Hz: %s\n", m->width, m->height, a, di.reason);
    }
    put_hz(a, di.cur_refresh);
    kprintf("[PSMAC:EDID] in use: %ux%u%s%s Hz (%s, %s)\n", di.cur_w, di.cur_h, di.cur_refresh ? " at " : "", di.cur_refresh ? a : ", refresh not known",
            di.cur_source == PSMB_SRC_FILE ? "aurora68-display.txt" : di.cur_source == PSMB_SRC_CONFIG ? "config.txt" : "the firmware's own choice", di.cur_text);
}
void psmac_hdmi_init(uint32_t vc_w, uint32_t vc_h)
{
    char how[64]; int have = 0;
    for (unsigned i = 0; i < sizeof di; i++) ((uint8_t *)&di)[i] = 0;
    di.magic = PSMB_DISPINFO_MAGIC; di.version = 1; di.size = sizeof di;
    di.suggested = 0xffff;
    const char *t = psmac_option("mac.edidtest");
    if (t && t[0] && !(t[0] == 's' && t[1] == 'a')) {
        uint32_t n = 0; while (t[n] && t[n] != ' ' && n + 1 < sizeof how) { how[n] = t[n]; n++; } how[n] = 0;
        int64_t fd = psmac_sh_available() ? psmac_sh_open(how, 1) : -1;
        int64_t l = fd >= 0 ? psmac_sh_flen(fd) : -1;
        if (l >= 128 && l <= (int64_t)sizeof edid && !psmac_sh_read(fd, edid, (uint64_t)l)) { nblocks = (int)(l / 128); have = 1; di.flags |= PSMB_DI_SAMPLE; }
        else kprintf("[PSMAC:EDID] mac.edidtest=%s: not read (semihosting, 128 to 1024 bytes)\n", how);
    } else if (t) { nblocks = edid_sample(edid); have = 1; di.flags |= PSMB_DI_SAMPLE; }
    if (!have) {
        /* the firmware's copy: block 0, then as many extension blocks as block 0's byte 126 says, while they come */
        uint64_t t0 = now_t();
        nblocks = 0;
        if (psmac_vc_edid_block(0, edid)) {
            nblocks = 1;
            int want = edid[126] + 1;
            if (want > 8) want = 8;
            while (nblocks < want && psmac_vc_edid_block((uint32_t)nblocks, edid + 128 * nblocks)) nblocks++;
        }
        kprintf("[PSMAC:EDID] the firmware's EDID: %d block%s in %u us\n", nblocks, nblocks == 1 ? "" : "s", (uint32_t)((now_t() - t0) * 1000000u / hz_t()));
    }
    { const char *c = psmac_option("mac.hdmiclock"); uint32_t mhz = 0; if (c) while (*c >= '0' && *c <= '9') mhz = mhz * 10 + (uint32_t)(*c++ - '0');
      edid_parse(edid, nblocks, mhz >= 25 && mhz <= 600 ? mhz * 1000 : 0, &di); }
    if (!nblocks) di.edid_status = PSMB_ES_NONE;
    di.cur_w = (uint16_t)vc_w; di.cur_h = (uint16_t)vc_h;
    struct edid_file f = { cfg_len >= 0 ? cfg_text : 0, cfg_len >= 0 ? (uint32_t)cfg_len : 0, disp_len >= 0 ? disp_text : 0, disp_len >= 0 ? (uint32_t)disp_len : 0 };
    int conf = 0;
    edid_current(&di, &f, &conf);
    edid_suggest(&di);
    if (confirm_mark) di.confirm_state = PSMB_CS_PENDING;
    log_summary();
}

/* ---- core 3: the Mac's questions ---- */
static void fill_dynamic(void)
{
    extern int psmac_display_hdmi_screen(uint32_t *w, uint32_t *h, uint32_t *vc_depth, uint32_t *frame_us);
    uint32_t w = 0, h = 0, d = 0, us = 0;
    psmac_display_hdmi_screen(&w, &h, &d, &us);
    di.mac_w = (uint16_t)w; di.mac_h = (uint16_t)h; di.vc_depth = (uint16_t)d; di.frame_us = (uint16_t)(us > 65535 ? 65535 : us);
    di.rotation = (uint16_t)rotation; di.rotation_next = (uint16_t)rotation_next;
    di.board = (uint16_t)(psmac_board_solo ? 1 : psmac_board_se ? 2 : 0);
    di.cfg &= ~(PSMB_CF_WRITABLE | PSMB_CF_FILE | PSMB_CF_BAK);
    if (psmac_card_writable()) di.cfg |= PSMB_CF_WRITABLE;
    if (disp_len >= 0) di.cfg |= PSMB_CF_FILE;
    if (bak_len >= 0) di.cfg |= PSMB_CF_BAK;
    di.restart_armed = (uint32_t)restart_armed;
    uint32_t now = secs();
    di.confirm_secs = di.confirm_state == PSMB_CS_ASKING && ask_deadline > now ? ask_deadline - now : 0;
}
static const char default_text[] = "# aurora68-display.txt: no HDMI mode chosen in Aurora68 Display yet; config.txt's own lines apply.\n";
/* Aurora68 Setup and Recovery's Display tab (psmac_recovery.c, docs/08 item 111): the same answer as the Mac's
   PSMB_INFO_DISPINFO, read on core 3, and the text that stands for "no file yet" in a .bak */
const struct psmb_dispinfo *psmac_hdmi_info(void) { fill_dynamic(); return &di; }
const char *psmac_hdmi_default_text(void) { return default_text; }
static int queue(const char *name, const char *text, uint32_t n)
{
    int r = psmac_card_put_file(name, text, n);
    if (r) msg(r == -1 ? "no writable SD card" : "the card writer is busy");
    return r;
}
int psmac_hdmi_cmd(uint16_t cmd, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t *result, const void **out, uint32_t *outlen)
{
    *out = 0; *outlen = 0; *result = 0;
    switch (cmd) {
    case PSMB_INFO_DISPINFO:
        fill_dynamic();
        *out = &di; *outlen = sizeof di; *result = sizeof di;
        return 0;
    case PSMB_INFO_DISPSET: {
        if (a0 >= di.nmodes || (a1 && !(di.mode[a0].flags & PSMB_DM_ALT)) || (a2 != 0 && a2 != 90 && a2 != 180 && a2 != 270)) return PSMB_E_RANGE;
        if (op) return PSMB_E_BUSY;
        if (!psmac_card_writable()) { msg("no writable SD card"); return PSMB_E_RO; }
        uint32_t n = edid_display_text(&di, (int)a0, (int)a1, (int)a2, 1, new_text, sizeof new_text);
        const char *old = disp_len >= 0 ? disp_text : default_text;
        uint32_t on = disp_len >= 0 ? (uint32_t)disp_len : sizeof default_text - 1;
        if (queue("aurora68-display.bak", old, on) || queue("aurora68-display.txt", new_text, n)) { di.apply_state = 3; return PSMB_E_BUSY; }
        op = 1; di.apply_state = 1; rotation_next = (int)a2;
        const struct psmb_dispmode *m = &di.mode[a0];
        kprintf("[PSMAC:EDID] Aurora68 Display chose %ux%u at %u Hz%s, rotation %u: aurora68-display.txt is being written (the old one as .bak)\n",
                m->width, m->height, (a1 ? m->alt_refresh : m->refresh) / 100u, a1 ? " (reduced blanking)" : "", a2);
        msg("writing aurora68-display.txt");
        return 0; }
    case PSMB_INFO_DISPROTATE: {
        if (a0 != 0 && a0 != 90 && a0 != 180 && a0 != 270) return PSMB_E_RANGE;
        if (op) return PSMB_E_BUSY;
        if (!psmac_card_writable()) { msg("no writable SD card: the rotation lasts until the Pi restarts"); rotation_next = (int)a0; return PSMB_E_RO; }
        uint32_t n = disp_len >= 0 ? edid_display_rotate(disp_text, (uint32_t)disp_len, (int)a0, new_text, sizeof new_text)
                                   : edid_display_rotate(default_text, sizeof default_text - 1, (int)a0, new_text, sizeof new_text);
        if (queue("aurora68-display.txt", new_text, n)) return PSMB_E_BUSY;
        op = 4; di.apply_state = 1; rotation_next = (int)a0;
        kprintf("[PSMAC:EDID] rotation %u asked: kept in aurora68-display.txt, in force from the Mac's next restart\n", a0);
        return 0; }
    case PSMB_INFO_DISPCONFIRM:
        if (a0 == PSMB_DC_ASKING) {
            if (di.confirm_state == PSMB_CS_PENDING || di.confirm_state == PSMB_CS_ASKING) {
                di.confirm_state = PSMB_CS_ASKING; ask_deadline = secs() + 25;          /* the Mac counts 15 */
                kprintf("[PSMAC:EDID] the Mac asks whether to keep %ux%u: without an answer in 25 s the old mode comes back\n", di.cur_w, di.cur_h);
            }
            *result = di.confirm_state;
            return 0;
        }
        if (di.confirm_state != PSMB_CS_PENDING && di.confirm_state != PSMB_CS_ASKING) { *result = di.confirm_state; return 0; }
        if (op) return PSMB_E_BUSY;
        if (a0 == PSMB_DC_KEEP) {
            uint32_t n = edid_display_unmark(disp_text, (uint32_t)(disp_len > 0 ? disp_len : 0), new_text, sizeof new_text);
            if (queue("aurora68-display.txt", new_text, n)) return PSMB_E_BUSY;
            op = 2; di.confirm_state = PSMB_CS_KEPT;
            kprintf("[PSMAC:EDID] the new mode is kept (%ux%u)\n", di.cur_w, di.cur_h);
        } else {
            uint32_t n = bak_len >= 0 ? edid_display_unmark(bak_text, (uint32_t)bak_len, new_text, sizeof new_text) : 0;
            if (bak_len < 0) { for (n = 0; default_text[n]; n++) new_text[n] = default_text[n]; }
            if (queue("aurora68-display.txt", new_text, n)) return PSMB_E_BUSY;
            op = 3; di.confirm_state = PSMB_CS_REVERTING;
            kprintf("[PSMAC:EDID] the new mode is not kept: aurora68-display.bak goes back, then the Pi restarts\n");
        }
        *result = di.confirm_state;
        return 0;
    case PSMB_INFO_DISPRESTART:
        restart_armed = 1;
        kprintf("[PSMAC:EDID] the Pi restarts at the Mac's restart, once the card is written\n");
        return 0;
    }
    return PSMB_E_BADCMD;
}

/* the Pi's own restart: its watchdog, set to run out at once with a full reset, and the partition to
   boot from set to 0 in PM_RSTS, as upstream Emu68's housekeeper does it (ps_classic_protocol.c) */
#define PM_RSTC (PSMAC_PERIPH_HI + 0x10001cu)
#define PM_RSTS (PSMAC_PERIPH_HI + 0x100020u)
#define PM_WDOG (PSMAC_PERIPH_HI + 0x100024u)
static void pi_restart(const char *why)
{
    kprintf("[PSMAC:EDID] the whole Pi restarts now (its watchdog): %s\n", why);
    uint32_t r = rd32le(PM_RSTS) & ~0xfffffaaau;
    wr32le(PM_RSTS, 0x5a000000u | r);
    wr32le(PM_WDOG, 0x5a000000u | 10u);
    wr32le(PM_RSTC, 0x5a000020u);
    for (;;) __asm__ volatile("wfe");
}

void psmac_hdmi_poll(void)
{
    static unsigned turn;
    if (++turn & 63) return;                           /* a look every 64 turns of core 3's loop is plenty */
    if (op) {
        int st = psmac_card_files_state();
        if (st == 2 || st == 3) {
            int was = op; op = 0;
            if (st == 3) {
                msg("the card could not be written"); di.apply_state = 3;
                if (was == 3) { restart_due = 1; due_at = secs(); }      /* a revert restarts even so: the old file may be half there */
                return;
            }
            if (was == 1) {                                /* what was aurora68-display.txt is now the .bak */
                const char *o = disp_len >= 0 ? disp_text : default_text;
                uint32_t on = disp_len >= 0 ? (uint32_t)disp_len : sizeof default_text - 1;
                for (uint32_t i = 0; i < on; i++) bak_text[i] = o[i];
                bak_text[on] = 0; bak_len = (int32_t)on;
            }
            uint32_t n = slen(new_text);
            for (uint32_t i = 0; i <= n && i < sizeof disp_text; i++) disp_text[i] = new_text[i];
            disp_len = (int32_t)n;
            di.apply_state = 2;
            msg(was == 1 ? "aurora68-display.txt written" : was == 2 ? "kept" : was == 3 ? "the old mode written back" : "rotation saved");
            if (was == 3) { restart_due = 1; due_at = secs(); }
        }
    }
    /* A recovery start (psmac_recovery.c): the 68k never runs, so nothing on the Mac can ask whether to keep a new
       mode, and the countdown below would put the .bak back and restart the Pi under the recovery page. The mark
       stays in the file for the next normal start, which asks. */
    extern int psmac_recovery_on(void);
    if (psmac_recovery_on()) return;
    uint32_t now = secs();
    if (di.confirm_state == PSMB_CS_ASKING && now >= ask_deadline && !op) {
        kprintf("[PSMAC:EDID] no answer from the Mac: the old mode comes back\n");
        uint32_t a; const void *o; uint32_t l; psmac_hdmi_cmd(PSMB_INFO_DISPCONFIRM, PSMB_DC_REVERT, 0, 0, &a, &o, &l);
    }
    if (di.confirm_state == PSMB_CS_PENDING && wait_s && now - start_s >= wait_s && !op) {
        kprintf("[PSMAC:EDID] the new mode was not confirmed within %u s of the start (mac.displaywait; is Aurora68 Screen Setup installed?): the old mode comes back\n", wait_s);
        uint32_t a; const void *o; uint32_t l; psmac_hdmi_cmd(PSMB_INFO_DISPCONFIRM, PSMB_DC_REVERT, 0, 0, &a, &o, &l);
    }
    if (restart_due == 1 && !op) {
        /* the log up to here into aurora68.log first (PiLog's Save to Card), so that what led to the restart
           can be read on another computer; the restart waits for it like any other write */
        extern int psmac_card_save_log(int);
        kprintf("[PSMAC:EDID] the whole Pi restarts (its watchdog) once the card has nothing left to write; the log goes into aurora68.log first\n");
        psmac_card_save_log(0);
        restart_due = 2; due_at = now;
    }
    if (restart_due == 2 && (psmac_card_idle() || now - due_at >= 10))
        pi_restart(psmac_card_idle() ? "the card has nothing left to write" : "the card still busy after 10 s");
}

/* ---- core 0, at the 68k's RESET (psmac_bus.c ps_send_reset): the Mac restarts ---- */
void psmac_hdmi_bus_reset(void)
{
    if (restart_armed) {
        if (!restart_due) { kprintf("[PSMAC:EDID] the Mac restarted: the Pi follows once the card is written\n"); restart_due = 1; due_at = secs(); }
        return;
    }
    if (rotation_next == rotation) return;
    extern int psmac_display_rotate(int rot);
    if (psmac_display_rotate(rotation_next)) rotation = rotation_next;
}
