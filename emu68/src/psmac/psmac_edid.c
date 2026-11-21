/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_edid.c — the HDMI monitor's EDID: what it is, what it can show, and what a Pi 3 can send it.
 *
 * Backstory. On 20 November 2026 the user's SE got a Dell UP2716D as its second screen, 2560 by 1440,
 * and the size had to be typed into config.txt by hand, after finding out from the internet that a
 * Pi 3 cannot send that size at 60 Hz (docs/10, "2560 by 1440"). The monitor says all of it itself:
 * every HDMI monitor carries an EDID (VESA E-EDID 1.3/1.4, 128-byte blocks), the first block with
 * its maker, model, serial number, size, the timings it supports and its preferred one, and usually
 * a CEA-861 extension block with the HDMI vendor block, the TV modes (VICs) and its sound. The Pi's
 * firmware reads it over DDC at boot and hands it out through the mailbox (psmac_hdmi.c). This file
 * turns those bytes into the struct PiDisplay shows (psmb.h, struct psmb_dispinfo): the monitor's
 * details, every mode once, each judged against the Pi 3's pixel clock (EDID_PI3_PIXCLK_KHZ) as
 * usable, too fast (with a CVT reduced-blanking alternative at a lower refresh when one fits) or of
 * the wrong shape for the panel, and a suggestion with its reason. It also writes and reads
 * aurora68-display.txt, the small file config.txt includes (psmac_hdmi.c, docs/10 "PiDisplay").
 *
 * Pure C, no kernel calls and no floating point: tools/edid_test.c compiles this file on the host
 * and checks it against the built-in sample, the CVT figures VESA's spreadsheet gives (1920x1080 at
 * 60 Hz: 173.00 MHz; 2560x1440 at 60 Hz reduced: 241.50 MHz) and the file texts.
 *
 * Sources: VESA E-EDID 1.4 (the block layout, the detailed timing and display descriptors, the
 * established and standard timings), CEA-861-F (the extension block, data block tags, the short
 * audio and video descriptors, the VIC table, the HDMI vendor block with OUI 00-0C-03), VESA CVT 1.1
 * (the clock formulas), VESA DMT 1.13 (the mode numbers config.txt's hdmi_group=2 takes, and their
 * clocks). Written for PiDisplay, 21 November 2026.
 */
#include <stdint.h>
#include "psmac_edid.h"

/* ---- small helpers (no libc) ---- */
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void scopy(char *d, const char *s, uint32_t cap) { uint32_t i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }
struct buf { char *p; uint32_t n, cap; };
static void put(struct buf *b, const char *s) { while (*s && b->n + 1 < b->cap) b->p[b->n++] = *s++; b->p[b->n] = 0; }
static void putu(struct buf *b, uint32_t v)
{
    char t[12]; int i = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i && b->n + 1 < b->cap) b->p[b->n++] = t[--i];
    b->p[b->n] = 0;
}
/* centihertz as "60" or "59.94" */
static void puthz(struct buf *b, uint32_t c)
{
    putu(b, c / 100);
    if (c % 100) { put(b, "."); if (c % 100 < 10) put(b, "0"); putu(b, c % 100 % 10 ? c % 100 : c % 100 / 10); }
}
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* ---- what is known about modes by number ---- */
/* CEA-861-F's video identification codes the monitors of this project's world list: size, refresh in
   centihertz (the 60 Hz family at its nominal 60), pixel clock in kHz, interlace, picture aspect */
struct vic { uint8_t vic; uint16_t w, h, hz; uint32_t khz; uint8_t il; uint16_t aw, ah; };
static const struct vic vics[] = {
    {  1,  640,  480, 6000,  25175, 0,  4, 3 }, {  2,  720,  480, 6000,  27000, 0,  4, 3 }, {  3,  720,  480, 6000,  27000, 0, 16, 9 },
    {  4, 1280,  720, 6000,  74250, 0, 16, 9 }, {  5, 1920, 1080, 6000,  74250, 1, 16, 9 }, { 16, 1920, 1080, 6000, 148500, 0, 16, 9 },
    { 17,  720,  576, 5000,  27000, 0,  4, 3 }, { 18,  720,  576, 5000,  27000, 0, 16, 9 }, { 19, 1280,  720, 5000,  74250, 0, 16, 9 },
    { 20, 1920, 1080, 5000,  74250, 1, 16, 9 }, { 31, 1920, 1080, 5000, 148500, 0, 16, 9 }, { 32, 1920, 1080, 2400,  74250, 0, 16, 9 },
    { 33, 1920, 1080, 2500,  74250, 0, 16, 9 }, { 34, 1920, 1080, 3000,  74250, 0, 16, 9 }, { 63, 1920, 1080, 12000, 297000, 0, 16, 9 },
    { 64, 1920, 1080, 10000, 297000, 0, 16, 9 }, { 93, 3840, 2160, 2400, 297000, 0, 16, 9 }, { 94, 3840, 2160, 2500, 297000, 0, 16, 9 },
    { 95, 3840, 2160, 3000, 297000, 0, 16, 9 }, { 96, 3840, 2160, 5000, 594000, 0, 16, 9 }, { 97, 3840, 2160, 6000, 594000, 0, 16, 9 },
    { 98, 4096, 2160, 2400, 297000, 0, 256, 135 }, { 99, 4096, 2160, 2500, 297000, 0, 256, 135 }, { 100, 4096, 2160, 3000, 297000, 0, 256, 135 },
    { 101, 4096, 2160, 5000, 594000, 0, 256, 135 }, { 102, 4096, 2160, 6000, 594000, 0, 256, 135 },
};
/* VESA DMT modes by the number config.txt's hdmi_group=2 hdmi_mode= takes (the firmware's group 2 list
   follows DMT's IDs), with their clocks: the established and standard timings name sizes and refresh
   rates only, and DMT is where their timing is (E-EDID 1.4, "standard timings": DMT first, then CVT) */
struct dmt { uint8_t id; uint16_t w, h, hz; uint32_t khz; };
static const struct dmt dmts[] = {
    {  4,  640,  480, 6000,  25175 }, {  5,  640,  480, 7200,  31500 }, {  6,  640,  480, 7500,  31500 },
    {  8,  800,  600, 5600,  36000 }, {  9,  800,  600, 6000,  40000 }, { 10,  800,  600, 7200,  50000 }, { 11,  800,  600, 7500,  49500 },
    { 16, 1024,  768, 6000,  65000 }, { 17, 1024,  768, 7000,  75000 }, { 18, 1024,  768, 7500,  78750 }, { 21, 1152,  864, 7500, 108000 },
    { 23, 1280,  768, 6000,  79500 }, { 28, 1280,  800, 6000,  83500 }, { 32, 1280,  960, 6000, 108000 }, { 35, 1280, 1024, 6000, 108000 },
    { 36, 1280, 1024, 7500, 135000 }, { 39, 1360,  768, 6000,  85500 }, { 42, 1400, 1050, 6000, 121750 }, { 47, 1440,  900, 6000, 106500 },
    { 51, 1600, 1200, 6000, 162000 }, { 58, 1680, 1050, 6000, 146250 }, { 68, 1920, 1200, 6000, 193250 }, { 81, 1366,  768, 6000,  85500 },
    { 82, 1920, 1080, 6000, 148500 }, { 85, 1280,  720, 6000,  74250 },
};
/* the reduced-blanking DMT modes, for the current mode's refresh only (an hdmi_mode= of these) */
static const struct dmt dmts_rb[] = { { 69, 1920, 1200, 6000, 154000 }, { 83, 1600, 900, 6000, 108000 }, { 86, 1366, 768, 6000, 72000 } };

static const struct dmt *dmt_find(uint32_t w, uint32_t h, uint32_t hz)
{
    for (unsigned i = 0; i < sizeof dmts / sizeof dmts[0]; i++)
        if (dmts[i].w == w && dmts[i].h == h && (dmts[i].hz > hz ? dmts[i].hz - hz : hz - dmts[i].hz) < 50) return &dmts[i];
    return 0;
}
static const struct vic *vic_find(uint32_t v)
{
    for (unsigned i = 0; i < sizeof vics / sizeof vics[0]; i++) if (vics[i].vic == v) return &vics[i];
    return 0;
}

/* ---- VESA CVT 1.1 ----
   The figures config.txt's hdmi_cvt makes a timing from. Normal blanking: C' = 30, M' = 300 (from
   C 40, M 600, K 128, J 20), a 550 us minimum vertical sync and back porch, 3 lines of front porch,
   cells of 8 pixels, the clock in 0.25 MHz steps. Reduced blanking (CVT-RB, version 1): 160 pixels
   of horizontal blanking, a 460 us minimum vertical blanking, 3 lines front porch, 6 back porch. The
   vertical sync's width says the aspect ratio: 4 lines 4:3, 5 16:9, 6 16:10, 7 5:4 and 15:9, 10 other.
   Integers throughout: periods in picoseconds. Checked: 1920x1080 at 60 Hz 173.00 MHz, 2560x1440 at
   60 Hz reduced 241.50 MHz, 1920x1200 at 60 Hz reduced 154.00 MHz (DMT mode 69's own figure). */
static uint32_t cvt_vsync(uint32_t w, uint32_t h)
{
    if (w * 3 == h * 4) return 4;
    if (w * 9 == h * 16 || (w * 9 > h * 16 - h / 8 && w * 9 < h * 16 + h / 8)) return 5;     /* 1366 x 768 counts as 16:9 */
    if (w * 10 == h * 16) return 6;
    if (w * 4 == h * 5 || w * 9 == h * 15) return 7;
    return 10;
}
uint32_t edid_cvt_khz(uint32_t w, uint32_t h, uint32_t hz, int rb)
{
    if (!w || !h || !hz) return 0;
    uint64_t W = w / 8 * 8, H = h, frame_ps = 1000000000000ull / hz, vs = cvt_vsync(w, h), khz;
    if (rb) {
        uint64_t hp = (frame_ps - 460000000ull) / H;
        uint64_t vbi = 460000000ull / hp + 1;
        if (vbi < 3 + vs + 6) vbi = 3 + vs + 6;
        khz = (uint64_t)hz * (H + vbi) * (W + 160) / 1000;
    } else {
        uint64_t hp = (frame_ps - 550000000ull) / (H + 3);
        int64_t duty = 30000 - (int64_t)(300 * hp / 1000000);       /* thousandths of a per cent */
        if (duty < 20000) duty = 20000;
        uint64_t hblank = W * (uint64_t)duty / (uint64_t)(100000 - duty) / 16 * 16;
        khz = (W + hblank) * 1000000000ull / hp;
    }
    return (uint32_t)(khz / 250 * 250);
}

/* ---- shapes ---- */
static void aspect(uint32_t w, uint32_t h, uint16_t *aw, uint16_t *ah)
{
    static const uint16_t snap[][2] = { { 4, 3 }, { 5, 4 }, { 3, 2 }, { 16, 10 }, { 15, 9 }, { 16, 9 }, { 64, 27 }, { 256, 135 } };
    for (unsigned i = 0; i < sizeof snap / sizeof snap[0]; i++) {
        uint64_t a = (uint64_t)w * snap[i][1] * 1000, b = (uint64_t)h * snap[i][0] * 1000;
        if (a > b * 985 / 1000 && a < b * 1015 / 1000) { *aw = snap[i][0]; *ah = snap[i][1]; return; }   /* within 1.5% */
    }
    uint32_t a = w, b = h;
    while (b) { uint32_t t = a % b; a = b; b = t; }
    *aw = (uint16_t)(a ? w / a : w); *ah = (uint16_t)(a ? h / a : h);
}
/* config.txt's aspect codes (hdmi_cvt, hdmi_timings): 1 4:3, 2 14:9, 3 16:9, 4 5:4, 5 16:10, 6 15:9, 7 21:9, 8 64:27 */
static int aspect_code(uint16_t aw, uint16_t ah)
{
    if (aw == 4 && ah == 3) return 1;
    if (aw == 14 && ah == 9) return 2;
    if (aw == 16 && ah == 9) return 3;
    if (aw == 5 && ah == 4) return 4;
    if (aw == 16 && ah == 10) return 5;
    if (aw == 15 && ah == 9) return 6;
    if (aw == 21 && ah == 9) return 7;
    if (aw == 64 && ah == 27) return 8;
    return 3;
}

/* ---- the mode list ---- */
/* what PiDisplay does not need but the file does: the detailed timing behind a mode, its VIC, its DMT ID */
static struct { uint8_t dtd[18]; uint8_t has_dtd, vic, dmt, rb_dmt; } ext[PSMB_DISP_MAXMODES];
static uint32_t pi_limit;

static int add_mode(struct psmb_dispinfo *di, uint32_t w, uint32_t h, uint32_t hz, uint32_t khz, uint16_t source, int il,
                    uint16_t aw, uint16_t ah, const uint8_t *dtd, int preferred)
{
    if (!w || !h || !hz) return -1;
    for (int i = 0; i < di->nmodes; i++) {
        struct psmb_dispmode *m = &di->mode[i];
        uint32_t d = m->refresh > hz ? m->refresh - hz : hz - m->refresh;
        if (m->width == w && m->height == h && d < 50 && !!(m->flags & PSMB_DM_INTERLACED) == !!il) {
            /* the same mode named twice (a detailed timing and its VIC, say): one entry, the most exact timing */
            if (preferred) m->flags |= PSMB_DM_PREFERRED;
            if (dtd && !ext[i].has_dtd) { for (int k = 0; k < 18; k++) ext[i].dtd[k] = dtd[k]; ext[i].has_dtd = 1; m->pixclk = khz; }
            if ((source >> 8) == PSMB_DS_CEA && !ext[i].vic) ext[i].vic = (uint8_t)source;
            return i;
        }
    }
    if (di->nmodes >= PSMB_DISP_MAXMODES) return -1;
    int i = di->nmodes++;
    struct psmb_dispmode *m = &di->mode[i];
    m->width = (uint16_t)w; m->height = (uint16_t)h; m->refresh = (uint16_t)hz; m->pixclk = khz; m->source = source;
    m->flags = (uint16_t)((il ? PSMB_DM_INTERLACED : 0) | (preferred ? PSMB_DM_PREFERRED : 0));
    if (aw && ah) { m->aspect_w = aw; m->aspect_h = ah; } else aspect(w, h, &m->aspect_w, &m->aspect_h);
    ext[i].has_dtd = 0; ext[i].vic = (source >> 8) == PSMB_DS_CEA ? (uint8_t)source : 0; ext[i].dmt = 0; ext[i].rb_dmt = 0;
    if (dtd) { for (int k = 0; k < 18; k++) ext[i].dtd[k] = dtd[k]; ext[i].has_dtd = 1; }
    return i;
}

/* an 18-byte detailed timing descriptor */
static int dtd_mode(struct psmb_dispinfo *di, const uint8_t *d, int n, int preferred)
{
    uint32_t khz = (uint32_t)(d[0] | d[1] << 8) * 10;
    uint32_t ha = d[2] | (d[4] >> 4) << 8, hb = d[3] | (d[4] & 15) << 8;
    uint32_t va = d[5] | (d[7] >> 4) << 8, vb = d[6] | (d[7] & 15) << 8;
    int il = (d[17] & 0x80) != 0;
    if (!khz || !ha || !va) return -1;
    uint64_t tot = (uint64_t)(ha + hb) * (va + vb);
    uint32_t chz = (uint32_t)((uint64_t)khz * 100000 / tot);       /* centihertz: kHz * 1000 * 100 / pixels */
    if (il) chz *= 2;                                              /* a field each, two to a frame */
    if (il) va *= 2;
    if (!di->size_w_mm) { di->size_w_mm = (uint16_t)(d[12] | (d[14] >> 4) << 8); di->size_h_mm = (uint16_t)(d[13] | (d[14] & 15) << 8); }
    /* 59.94 and 60.00 are both "60" to the user and to config.txt; keep two decimals where they are real (29.97, 23.98) */
    return add_mode(di, ha, va, chz, khz, (uint16_t)(PSMB_DS_DETAILED << 8 | n), il, 0, 0, d, preferred);
}

/* a display descriptor's text: up to 13 bytes, ended by a line feed */
static void desc_text(char *out, uint32_t cap, const uint8_t *d)
{
    uint32_t n = 0;
    for (int i = 5; i < 18 && d[i] != 0x0a && n + 1 < cap; i++) out[n++] = d[i] >= 32 && d[i] < 127 ? (char)d[i] : '?';
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

static void descriptor(struct psmb_dispinfo *di, const uint8_t *d, int n, int *first_dtd)
{
    if (d[0] || d[1]) { dtd_mode(di, d, n, *first_dtd); *first_dtd = 0; return; }
    switch (d[3]) {
    case 0xfc: desc_text(di->name, sizeof di->name, d); break;
    case 0xff: desc_text(di->serial_text, sizeof di->serial_text, d); break;
    case 0xfd:                                                     /* range limits: EDID 1.4's offset flags add 255 */
        di->min_vhz = (uint16_t)(d[5] + (d[4] & 1 ? 255 : 0)); di->max_vhz = (uint16_t)(d[6] + (d[4] & 2 ? 255 : 0));
        di->min_hkhz = (uint16_t)(d[7] + (d[4] & 4 ? 255 : 0)); di->max_hkhz = (uint16_t)(d[8] + (d[4] & 8 ? 255 : 0));
        di->max_pixclk = (uint32_t)d[9] * 10000;
        break;
    }
}

/* the PNP IDs of makers likely to be on a desk (the UEFI forum's list); others show as the ID alone */
static const char *const makers[][2] = {
    { "DEL", "Dell" }, { "SAM", "Samsung" }, { "GSM", "LG" }, { "ACI", "ASUS" }, { "AUS", "ASUS" }, { "BNQ", "BenQ" },
    { "HWP", "HP" }, { "LEN", "Lenovo" }, { "PHL", "Philips" }, { "VSC", "ViewSonic" }, { "ACR", "Acer" }, { "AOC", "AOC" },
    { "NEC", "NEC" }, { "EIZ", "Eizo" }, { "ENC", "Eizo" }, { "APP", "Apple" }, { "SNY", "Sony" }, { "MEI", "Panasonic" },
    { "IVM", "Iiyama" }, { "SHP", "Sharp" }, { "TSB", "Toshiba" }, { "VIZ", "Vizio" }, { "HSD", "HannStar" }, { "MSI", "MSI" },
    { "GBT", "Gigabyte" }, { "HIQ", "Hyundai ImageQuest" }, { "FUS", "Fujitsu" }, { "SGI", "SGI" }, { "WAC", "Wacom" },
    { "RPI", "Raspberry Pi" }, { "ZZQ", "a made-up sample" },
};

/* a CEA-861 extension block */
static void cea_block(struct psmb_dispinfo *di, const uint8_t *b)
{
    uint32_t d = b[2];
    di->conn |= PSMB_CN_CEA;
    if (b[1] >= 2) {
        if (b[3] & 0x40) di->conn |= PSMB_CN_BASICAUDIO;
        if (b[3] & 0x20) di->conn |= PSMB_CN_YCC444;
        if (b[3] & 0x10) di->conn |= PSMB_CN_YCC422;
    }
    if (d < 4 || d > 127) d = 4;
    for (uint32_t p = 4; p < d; ) {
        uint32_t tag = b[p] >> 5, len = b[p] & 31;
        const uint8_t *x = b + p + 1;
        if (p + 1 + len > d) break;
        if (tag == 1) {                                            /* audio: short audio descriptors, three bytes each */
            for (uint32_t i = 0; i + 3 <= len; i += 3) {
                uint32_t fmt = (x[i] >> 3) & 15;
                di->audio_formats |= 1u << fmt;
                if (fmt == 1) {
                    uint32_t ch = (x[i] & 7) + 1u;
                    if (ch > di->lpcm_channels) di->lpcm_channels = ch;
                    di->lpcm_rates |= x[i + 1] & 0x7f; di->lpcm_bits |= x[i + 2] & 7;
                }
            }
        } else if (tag == 2) {                                     /* video: VICs, 129-192 the native ones of 1-64 */
            for (uint32_t i = 0; i < len; i++) {
                uint32_t v = x[i];
                int native = 0;
                if (v >= 129 && v <= 192) { v -= 128; native = 1; }
                const struct vic *t = vic_find(v);
                if (t) add_mode(di, t->w, t->h, t->hz, t->khz, (uint16_t)(PSMB_DS_CEA << 8 | v), t->il, t->aw, t->ah, 0, 0);
                (void)native;
            }
        } else if (tag == 3 && len >= 5 && x[0] == 0x03 && x[1] == 0x0c && x[2] == 0x00) {   /* HDMI, IEEE OUI 00-0C-03 */
            di->conn |= PSMB_CN_HDMI;
            if (len >= 6) {
                if (x[5] & 0x40) di->conn |= PSMB_CN_DC48;
                if (x[5] & 0x20) di->conn |= PSMB_CN_DC36;
                if (x[5] & 0x10) di->conn |= PSMB_CN_DC30;
            }
            if (len >= 7 && x[6]) di->tmds_max = (uint32_t)x[6] * 5000;
        } else if (tag == 4 && len >= 1) di->speakers = x[0];
        p += 1 + len;
    }
    int first = 0;
    for (uint32_t p = d; p + 18 <= 127 && (b[p] || b[p + 1]); p += 18) descriptor(di, b + p, (int)(p - d) / 18 + 5, &first);
}

/* every mode judged: the Mac's sizes, the Pi's clock (and the monitor's own), the panel's shape */
static void judge(struct psmb_dispinfo *di)
{
    uint32_t limit = pi_limit;
    if (di->max_pixclk && di->max_pixclk < limit) limit = di->max_pixclk;
    /* the panel's shape: its size in millimetres, else its preferred mode */
    uint32_t pw = di->size_w_mm, ph = di->size_h_mm, nw = 0, nh = 0;
    for (int i = 0; i < di->nmodes; i++) if (di->mode[i].flags & PSMB_DM_PREFERRED) { nw = di->mode[i].width; nh = di->mode[i].height; break; }
    if (!nw && di->nmodes) { nw = di->mode[0].width; nh = di->mode[0].height; }
    if (!pw || !ph || pw * 10 < ph || ph * 10 < pw) { pw = nw; ph = nh; }     /* a projector says 0, some TVs say nonsense */
    for (int i = 0; i < di->nmodes; i++) {
        struct psmb_dispmode *m = &di->mode[i];
        uint32_t hz = (m->refresh + 50) / 100;
        if (m->width == nw && m->height == nh) m->flags |= PSMB_DM_NATIVE;
        if (m->width < PSMB_SCREEN_MIN_W || m->width > PSMB_SCREEN_MAX_W || m->height < PSMB_SCREEN_MIN_H || m->height > PSMB_SCREEN_MAX_H)
            m->flags |= PSMB_DM_MACSIZE;
        if (pw && ph) {
            /* the picture's shape against the panel's, in thousandths: more than 4% apart is a stretch */
            uint64_t a = (uint64_t)m->aspect_w * ph * 1000, b = (uint64_t)m->aspect_h * pw * 1000;
            if (a > b * 104 / 100 || a < b * 96 / 100) m->flags |= PSMB_DM_SHAPE;
        }
        if (m->pixclk <= limit) { m->flags |= PSMB_DM_USABLE; continue; }
        m->flags |= PSMB_DM_TOOFAST;
        if (m->flags & PSMB_DM_INTERLACED) continue;
        /* the same size with reduced blanking: at its own refresh, else 50, else 30 Hz, if the monitor goes that low */
        static const uint32_t tries[] = { 0, 50, 30 };
        for (unsigned k = 0; k < 3; k++) {
            uint32_t r = tries[k] ? tries[k] : hz;
            if (tries[k] && tries[k] >= hz) continue;
            if (di->min_vhz && r < di->min_vhz) continue;
            uint32_t c = edid_cvt_khz(m->width, m->height, r, 1);
            if (c && c <= limit) { m->flags |= PSMB_DM_ALT; m->alt_refresh = (uint16_t)(r * 100); m->alt_pixclk = c; break; }
        }
    }
    /* config.txt's numbers for each mode itself: a VIC the Pi 3's firmware knows (group 1), a DMT ID
       (group 2), else group 2's custom mode 87 with hdmi_timings or hdmi_cvt */
    for (int i = 0; i < di->nmodes; i++) {
        struct psmb_dispmode *m = &di->mode[i];
        const struct dmt *t = dmt_find(m->width, m->height, m->refresh);
        ext[i].dmt = t && !(m->flags & PSMB_DM_INTERLACED) ? t->id : 0;
        if (ext[i].vic && ext[i].vic <= 64) { m->cfg_group = 1; m->cfg_mode = ext[i].vic; }
        else if (ext[i].dmt) { m->cfg_group = 2; m->cfg_mode = ext[i].dmt; }
        else { m->cfg_group = 2; m->cfg_mode = 87; }
        if (!ext[i].has_dtd && (m->source >> 8) != PSMB_DS_CEA && t) m->pixclk = t->khz;
    }
}

/* largest first, then the higher refresh */
static void sort_modes(struct psmb_dispinfo *di)
{
    for (int i = 1; i < di->nmodes; i++)
        for (int j = i; j > 0; j--) {
            struct psmb_dispmode *a = &di->mode[j - 1], *b = &di->mode[j];
            uint32_t pa = (uint32_t)a->width * a->height, pb = (uint32_t)b->width * b->height;
            if (pb < pa || (pb == pa && (b->width < a->width || (b->width == a->width && b->refresh <= a->refresh)))) break;
            struct psmb_dispmode t = *a; *a = *b; *b = t;
            uint8_t te[sizeof ext[0]]; uint8_t *ea = (uint8_t *)&ext[j - 1], *eb = (uint8_t *)&ext[j];
            for (unsigned k = 0; k < sizeof ext[0]; k++) { te[k] = ea[k]; ea[k] = eb[k]; eb[k] = te[k]; }
        }
}

int edid_parse(const uint8_t *b, int n, uint32_t pi_khz, struct psmb_dispinfo *di)
{
    pi_limit = pi_khz ? pi_khz : EDID_PI3_PIXCLK_KHZ;
    di->pi_pixclk = pi_limit;
    di->nmodes = 0; di->suggested = 0xffff;
    static const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    if (n < 1) { di->edid_status = PSMB_ES_NONE; return 0; }
    for (int i = 0; i < 8; i++) if (b[i] != hdr[i]) { di->edid_status = PSMB_ES_HEADER; return 0; }
    di->edid_blocks = (uint32_t)n; di->edid_status = PSMB_ES_OK;
    for (int k = 0; k < n; k++) { uint8_t s = 0; for (int i = 0; i < 128; i++) s = (uint8_t)(s + b[128 * k + i]); if (s) di->edid_status = PSMB_ES_CHECKSUM; }
    di->flags |= PSMB_DI_EDID;
    uint32_t mf = (uint32_t)b[8] << 8 | b[9];
    di->maker[0] = (char)('@' + ((mf >> 10) & 31)); di->maker[1] = (char)('@' + ((mf >> 5) & 31)); di->maker[2] = (char)('@' + (mf & 31)); di->maker[3] = 0;
    for (unsigned i = 0; i < sizeof makers / sizeof makers[0]; i++)
        if (makers[i][0][0] == di->maker[0] && makers[i][0][1] == di->maker[1] && makers[i][0][2] == di->maker[2]) scopy(di->maker_name, makers[i][1], sizeof di->maker_name);
    di->product = (uint32_t)(b[10] | b[11] << 8);
    di->serial = (uint32_t)b[12] | (uint32_t)b[13] << 8 | (uint32_t)b[14] << 16 | (uint32_t)b[15] << 24;
    di->week = b[16]; di->year = (uint16_t)(1990 + b[17]);
    di->edid_version = (uint16_t)(b[18] << 8 | b[19]);
    di->input = b[20];
    if (b[20] & 0x80) di->conn |= PSMB_CN_DIGITAL;
    di->size_w_mm = (uint16_t)(b[21] * 10); di->size_h_mm = (uint16_t)(b[22] * 10);
    /* the detailed timings first: the first is the preferred mode (EDID 1.3 says so, 1.4 always) */
    int first = 1;
    for (int i = 0; i < 4; i++) descriptor(di, b + 54 + 18 * i, i + 1, &first);
    /* established timings I and II (bytes 35-37) */
    static const struct { uint16_t w, h, hz; uint32_t khz; } est[17] = {
        { 800, 600, 6000, 40000 }, { 800, 600, 5600, 36000 }, { 640, 480, 7500, 31500 }, { 640, 480, 7200, 31500 },
        { 640, 480, 6700, 30240 }, { 640, 480, 6000, 25175 }, { 720, 400, 8800, 35500 }, { 720, 400, 7000, 28322 },
        { 1280, 1024, 7500, 135000 }, { 1024, 768, 7500, 78750 }, { 1024, 768, 7000, 75000 }, { 1024, 768, 6000, 65000 },
        { 1024, 768, 8700, 44900 }, { 832, 624, 7500, 57284 }, { 800, 600, 7500, 49500 }, { 800, 600, 7200, 50000 },
        { 1152, 870, 7500, 100000 } };
    uint32_t bits = (uint32_t)b[35] | (uint32_t)b[36] << 8 | (uint32_t)(b[37] & 0x80) << 9;
    for (int i = 0; i < 17; i++)
        if (bits & (1u << i)) add_mode(di, est[i].w, est[i].h, est[i].hz, est[i].khz, (uint16_t)(PSMB_DS_ESTABLISHED << 8 | i), i == 12, 0, 0, 0, 0);
    /* standard timings (bytes 38-53): width / 8 - 31, the aspect in the top two bits, the refresh - 60 */
    for (int i = 0; i < 8; i++) {
        const uint8_t *s = b + 38 + 2 * i;
        if ((s[0] == 1 && s[1] == 1) || !s[0]) continue;
        uint32_t w = (s[0] + 31u) * 8, hz = (s[1] & 63) + 60u, h;
        switch (s[1] >> 6) {
        case 0: h = di->edid_version < 0x0103 ? w : w * 10 / 16; break;       /* 1:1 before EDID 1.3 */
        case 1: h = w * 3 / 4; break;
        case 2: h = w * 4 / 5; break;
        default: h = w * 9 / 16; break;
        }
        const struct dmt *t = dmt_find(w, h, hz * 100);
        add_mode(di, w, h, hz * 100, t ? t->khz : edid_cvt_khz(w, h, hz, 0), (uint16_t)(PSMB_DS_STANDARD << 8 | i), 0, 0, 0, 0, 0);
    }
    for (int k = 1; k < n; k++) if (b[128 * k] == 0x02) cea_block(di, b + 128 * k);
    judge(di);
    sort_modes(di);
    return di->nmodes;
}

/* ---- config.txt and aurora68-display.txt ---- */
struct hdmicfg { int group, mode, have_cvt, have_tim, from_file; uint32_t cvt[3], tim_rate, tim_w, tim_h; char text[64]; };
static int key_is(const char *s, uint32_t n, const char *k)
{
    uint32_t m = slen(k), i = 0;
    while (i < m && i < n && s[i] == k[i]) i++;
    return i == m && (i == n || s[i] == ':' || s[i] == '=');      /* hdmi_mode:0= is the Pi 4's per-port spelling */
}
static uint32_t numbers(const char *s, uint32_t n, uint32_t *out, int max)
{
    int k = 0; uint32_t i = 0;
    while (i < n && k < max) {
        while (i < n && is_space(s[i])) i++;
        if (i >= n || s[i] < '0' || s[i] > '9') break;
        uint32_t v = 0; while (i < n && s[i] >= '0' && s[i] <= '9') v = v * 10 + (uint32_t)(s[i++] - '0');
        out[k++] = v;
    }
    return (uint32_t)k;
}
static int file_lines(const char *t, uint32_t n, struct hdmicfg *c, int in_file, const struct edid_file *f, struct psmb_dispinfo *di,
                      int *rot, int *confirm, int depth);
static void one_line(const char *s, uint32_t n, struct hdmicfg *c, int in_file, const struct edid_file *f, struct psmb_dispinfo *di,
                     int *rot, int *confirm, int depth)
{
    while (n && is_space(*s)) { s++; n--; }
    while (n && is_space(s[n - 1])) n--;
    if (in_file && n > 10 && s[0] == '#') {
        /* the kernel's own lines in its file: "#aurora68 rotate=90", "#aurora68 confirm=2560x1440@30" */
        const char *k = "#aurora68 "; uint32_t m = 0;
        while (m < 10 && s[m] == k[m]) m++;
        if (m == 10) {
            if (n > 17 && s[10] == 'r' && s[11] == 'o' && s[16] == '=') { uint32_t v = 0; numbers(s + 17, n - 17, &v, 1); if (v == 90 || v == 180 || v == 270) *rot = (int)v; }
            if (n > 18 && s[10] == 'c' && s[11] == 'o' && s[17] == '=') {
                uint32_t v[3] = { 0, 0, 0 }; uint32_t i = 18, k2 = 0;
                while (i < n && k2 < 3) { uint32_t x = 0; while (i < n && s[i] >= '0' && s[i] <= '9') x = x * 10 + (uint32_t)(s[i++] - '0'); v[k2++] = x; if (i < n) i++; }
                *confirm = 1; di->confirm_w = (uint16_t)v[0]; di->confirm_h = (uint16_t)v[1]; di->confirm_refresh = (uint16_t)(v[2] * 100);
            }
        }
        return;
    }
    if (!n || s[0] == '#' || s[0] == '[') return;
    if (!in_file && depth == 0 && n > 8 && s[0] == 'i' && s[1] == 'n' && s[2] == 'c' && s[3] == 'l' && s[4] == 'u' && s[5] == 'd' && s[6] == 'e' && is_space(s[7])) {
        uint32_t i = 8; while (i < n && is_space(s[i])) i++;
        const char *want = "aurora68-display.txt"; uint32_t m = 0;
        while (i + m < n && want[m] && (s[i + m] == want[m] || s[i + m] == want[m] - 32)) m++;
        if (!want[m] && i + m == n) {
            di->cfg |= PSMB_CF_INCLUDE;
            if (f->display) file_lines(f->display, f->display_len, c, 1, f, di, rot, confirm, depth + 1);
        }
        return;
    }
    uint32_t e = 0; while (e < n && s[e] != '=') e++;
    if (e >= n) return;
    const char *v = s + e + 1; uint32_t vn = n - e - 1, x[17];
    int hdmi = 0;
    if (key_is(s, e, "hdmi_group")) { if (numbers(v, vn, x, 1)) c->group = (int)x[0]; hdmi = 1; }
    else if (key_is(s, e, "hdmi_mode")) { if (numbers(v, vn, x, 1)) c->mode = (int)x[0]; hdmi = 1; }
    else if (key_is(s, e, "hdmi_cvt")) { if (numbers(v, vn, x, 3) >= 3) { c->have_cvt = 1; c->cvt[0] = x[0]; c->cvt[1] = x[1]; c->cvt[2] = x[2]; } hdmi = 1; }
    else if (key_is(s, e, "hdmi_timings")) { if (numbers(v, vn, x, 17) >= 14) { c->have_tim = 1; c->tim_w = x[0]; c->tim_h = x[5]; c->tim_rate = x[13]; } hdmi = 1; }
    if (hdmi) {
        c->from_file = in_file;
        if (!in_file && (di->cfg & PSMB_CF_INCLUDE)) di->cfg |= PSMB_CF_OVERRIDE;
    }
}
static int file_lines(const char *t, uint32_t n, struct hdmicfg *c, int in_file, const struct edid_file *f, struct psmb_dispinfo *di,
                      int *rot, int *confirm, int depth)
{
    uint32_t i = 0;
    while (i < n) {
        uint32_t s = i; while (i < n && t[i] != '\n') i++;
        one_line(t + s, i - s, c, in_file, f, di, rot, confirm, depth);
        i++;
    }
    return 0;
}

int edid_current(struct psmb_dispinfo *di, const struct edid_file *f, int *confirm)
{
    struct hdmicfg c = { 0 };
    int rot = 0, conf = 0;
    if (f->config) { di->cfg |= PSMB_CF_CONFIG; file_lines(f->config, f->config_len, &c, 0, f, di, &rot, &conf, 0); }
    if (f->display && !(di->cfg & PSMB_CF_INCLUDE)) {
        /* the firmware does not read the file (no include line), but its rotation and mark are the kernel's own */
        struct hdmicfg c2 = { 0 };
        file_lines(f->display, f->display_len, &c2, 1, f, di, &rot, &conf, 1);
    }
    uint32_t hz = 0;
    struct buf b = { di->cur_text, 0, sizeof di->cur_text };
    di->cur_text[0] = 0;
    if (c.group == 1 && c.mode) { const struct vic *t = vic_find((uint32_t)c.mode); if (t) hz = t->hz; }
    else if (c.group == 2 && c.mode == 87) hz = c.have_tim ? c.tim_rate * 100 : c.have_cvt ? c.cvt[2] * 100 : 0;
    else if (c.group == 2 && c.mode) {
        for (unsigned i = 0; i < sizeof dmts / sizeof dmts[0]; i++) if (dmts[i].id == c.mode) hz = dmts[i].hz;
        for (unsigned i = 0; i < sizeof dmts_rb / sizeof dmts_rb[0]; i++) if (dmts_rb[i].id == c.mode) hz = dmts_rb[i].hz;
    }
    if (c.group || c.mode) {
        put(&b, "hdmi_group="); putu(&b, (uint32_t)c.group); put(&b, " hdmi_mode="); putu(&b, (uint32_t)c.mode);
        if (c.group == 2 && c.mode == 87 && c.have_cvt) { put(&b, " hdmi_cvt="); putu(&b, c.cvt[0]); put(&b, " "); putu(&b, c.cvt[1]); put(&b, " "); putu(&b, c.cvt[2]); }
        else if (c.group == 2 && c.mode == 87 && c.have_tim) { put(&b, " hdmi_timings="); putu(&b, c.tim_w); put(&b, " ... "); putu(&b, c.tim_h); put(&b, " ..."); }
        di->cur_source = (uint16_t)(c.from_file ? PSMB_SRC_FILE : PSMB_SRC_CONFIG);
    } else { put(&b, "no hdmi_mode: the firmware's choice"); di->cur_source = PSMB_SRC_UNKNOWN; }
    di->cur_refresh = (uint16_t)hz;
    /* the list's entry for it: the mode itself at that refresh, or the one whose alternative it is */
    int best = -1;
    for (int i = 0; i < di->nmodes; i++) {
        struct psmb_dispmode *m = &di->mode[i];
        if (m->width != di->cur_w || m->height != di->cur_h) continue;
        uint32_t d = m->refresh > hz ? m->refresh - hz : hz - m->refresh, da = m->alt_refresh > hz ? m->alt_refresh - hz : hz - m->alt_refresh;
        if (hz && d < 50) { best = i; break; }
        if (hz && (m->flags & PSMB_DM_ALT) && da < 50) { best = i; break; }
        if (!hz && best < 0 && (m->flags & PSMB_DM_USABLE)) best = i;
    }
    if (best >= 0) di->mode[best].flags |= PSMB_DM_CURRENT;
    *confirm = conf;
    return rot;
}

/* the size of the HDMI mode config.txt (with the file it includes) sets: what the firmware's frame
   buffer would be. For the testbed (mac.hdmisim=1, psmac_hdmi.c), where nothing reads config.txt. */
int edid_config_size(const struct edid_file *f, uint32_t *w, uint32_t *h)
{
    static struct psmb_dispinfo scratch;
    struct hdmicfg c = { 0 };
    int rot = 0, conf = 0;
    scratch.cfg = 0;
    if (f->config) file_lines(f->config, f->config_len, &c, 0, f, &scratch, &rot, &conf, 0);
    else if (f->display) file_lines(f->display, f->display_len, &c, 1, f, &scratch, &rot, &conf, 1);
    *w = *h = 0;
    if (c.group == 1) { const struct vic *t = vic_find((uint32_t)c.mode); if (t) { *w = t->w; *h = t->h; } }
    else if (c.group == 2 && c.mode == 87) { if (c.have_tim) { *w = c.tim_w; *h = c.tim_h; } else if (c.have_cvt) { *w = c.cvt[0]; *h = c.cvt[1]; } }
    else if (c.group == 2) {
        for (unsigned i = 0; i < sizeof dmts / sizeof dmts[0]; i++) if (dmts[i].id == c.mode) { *w = dmts[i].w; *h = dmts[i].h; }
        for (unsigned i = 0; i < sizeof dmts_rb / sizeof dmts_rb[0]; i++) if (dmts_rb[i].id == c.mode) { *w = dmts_rb[i].w; *h = dmts_rb[i].h; }
    }
    return *w && *h;
}

void edid_suggest(struct psmb_dispinfo *di)
{
    struct buf b = { di->reason, 0, sizeof di->reason };
    int pick = -1, how = 0;
    di->reason[0] = 0;
    for (int i = 0; i < di->nmodes && pick < 0; i++) {
        const struct psmb_dispmode *m = &di->mode[i];
        if ((m->flags & PSMB_DM_NATIVE) && !(m->flags & (PSMB_DM_MACSIZE | PSMB_DM_INTERLACED)) && (m->flags & (PSMB_DM_USABLE | PSMB_DM_ALT))) {
            /* the native size: the highest usable refresh of it, else its alternative */
            pick = i; how = (m->flags & PSMB_DM_USABLE) ? 1 : 2;
            for (int j = i + 1; j < di->nmodes; j++)
                if (di->mode[j].width == m->width && di->mode[j].height == m->height && (di->mode[j].flags & PSMB_DM_USABLE) && how == 2) { pick = j; how = 1; }
        }
    }
    for (int i = 0; i < di->nmodes && pick < 0; i++) {
        const struct psmb_dispmode *m = &di->mode[i];
        if ((m->flags & PSMB_DM_USABLE) && !(m->flags & (PSMB_DM_SHAPE | PSMB_DM_MACSIZE | PSMB_DM_INTERLACED))) { pick = i; how = 3; }
    }
    for (int i = 0; i < di->nmodes && pick < 0; i++)
        if ((di->mode[i].flags & PSMB_DM_USABLE) && !(di->mode[i].flags & (PSMB_DM_MACSIZE | PSMB_DM_INTERLACED))) { pick = i; how = 4; }
    if (pick < 0) { put(&b, "No mode of this monitor fits the Pi 3 and the Mac"); return; }
    const struct psmb_dispmode *m = &di->mode[pick];
    di->suggested = (uint16_t)pick;
    di->mode[pick].flags |= PSMB_DM_SUGGESTED;
    if (how == 1) { put(&b, "The panel's own size, at "); puthz(&b, m->refresh); put(&b, " Hz: every pixel is a pixel of the panel"); }
    else if (how == 2) {
        put(&b, "The panel's own size; "); puthz(&b, m->refresh); put(&b, " Hz needs "); putu(&b, m->pixclk / 1000);
        put(&b, " MHz, the Pi 3 sends "); putu(&b, di->pi_pixclk / 1000); put(&b, ", so "); puthz(&b, m->alt_refresh); put(&b, " Hz");
    } else if (how == 3) put(&b, "The largest size of the panel's shape the Pi 3 can send (the monitor scales it)");
    else put(&b, "The largest size the Pi 3 can send; not the panel's shape, so the picture is stretched");
}

static void put_dtd_timings(struct buf *b, const uint8_t *d, int code)
{
    uint32_t khz = (uint32_t)(d[0] | d[1] << 8) * 10;
    uint32_t ha = d[2] | (d[4] >> 4) << 8, hb = d[3] | (d[4] & 15) << 8, va = d[5] | (d[7] >> 4) << 8, vb = d[6] | (d[7] & 15) << 8;
    uint32_t hfp = d[8] | ((d[11] >> 6) & 3) << 8, hs = d[9] | ((d[11] >> 4) & 3) << 8;
    uint32_t vfp = (d[10] >> 4) | ((d[11] >> 2) & 3) << 4, vs = (d[10] & 15) | (d[11] & 3) << 4;
    int il = (d[17] & 0x80) != 0, hpol = (d[17] & 0x18) == 0x18 ? (d[17] >> 1) & 1 : 1, vpol = (d[17] & 0x18) == 0x18 ? (d[17] >> 2) & 1 : 1;
    uint64_t tot = (uint64_t)(ha + hb) * (va + vb);
    uint32_t rate = (uint32_t)(((uint64_t)khz * 1000 + tot / 2) / tot);
    /* hdmi_timings=<h_active> <h_sync_pol> <h_front_porch> <h_sync> <h_back_porch> <v_active> <v_sync_pol>
       <v_front_porch> <v_sync> <v_back_porch> <v_sync_offset_a> <v_sync_offset_b> <pixel_rep> <frame_rate>
       <interlaced> <pixel_freq> <aspect>, the polarities 1 for positive */
    put(b, "hdmi_timings="); putu(b, ha); put(b, " "); putu(b, (uint32_t)hpol); put(b, " "); putu(b, hfp); put(b, " "); putu(b, hs);
    put(b, " "); putu(b, hb - hfp - hs); put(b, " "); putu(b, va); put(b, " "); putu(b, (uint32_t)vpol); put(b, " "); putu(b, vfp);
    put(b, " "); putu(b, vs); put(b, " "); putu(b, vb - vfp - vs); put(b, " 0 0 0 "); putu(b, rate); put(b, " "); putu(b, (uint32_t)il);
    put(b, " "); putu(b, khz * 1000); put(b, " "); putu(b, (uint32_t)code); put(b, "\n");
}

uint32_t edid_display_text(const struct psmb_dispinfo *di, int mi, int alt, int rotation, int confirm, char *out, uint32_t cap)
{
    struct buf b = { out, 0, cap };
    const struct psmb_dispmode *m = &di->mode[mi];
    uint32_t hz = alt ? m->alt_refresh : m->refresh, khz = alt ? m->alt_pixclk : m->pixclk;
    out[0] = 0;
    put(&b, "# aurora68-display.txt: the HDMI mode chosen in Aurora68 Display, written by the Aurora68 kernel.\n"
            "# config.txt reads it through the line \"include aurora68-display.txt\", at the end of\n"
            "# config.txt (an hdmi_ line after it would win). The previous file is aurora68-display.bak.\n");
    put(&b, "# "); put(&b, di->name[0] ? di->name : "the monitor"); if (di->maker[0]) { put(&b, " ("); put(&b, di->maker); put(&b, ")"); }
    put(&b, ": "); putu(&b, m->width); put(&b, "x"); putu(&b, m->height); put(&b, " at "); puthz(&b, hz); put(&b, " Hz, ");
    putu(&b, khz / 1000); put(&b, "."); { uint32_t f = khz % 1000 / 10; if (f < 10) put(&b, "0"); putu(&b, f); }
    put(&b, alt ? " MHz, CVT reduced blanking\n" : " MHz\n");
    const struct dmt *rbd = 0;
    for (unsigned i = 0; alt && i < sizeof dmts_rb / sizeof dmts_rb[0]; i++)
        if (dmts_rb[i].w == m->width && dmts_rb[i].h == m->height && dmts_rb[i].hz == hz) rbd = &dmts_rb[i];
    if (rbd) { put(&b, "hdmi_group=2\nhdmi_mode="); putu(&b, rbd->id); put(&b, "\n"); }      /* the firmware's own reduced-blanking mode */
    else if (alt) {
        put(&b, "hdmi_group=2\nhdmi_mode=87\nhdmi_cvt="); putu(&b, m->width); put(&b, " "); putu(&b, m->height); put(&b, " ");
        putu(&b, (hz + 50) / 100); put(&b, " "); putu(&b, (uint32_t)aspect_code(m->aspect_w, m->aspect_h)); put(&b, " 0 0 1\n");
    } else if (m->cfg_group == 1) { put(&b, "hdmi_group=1\nhdmi_mode="); putu(&b, m->cfg_mode); put(&b, "\n"); }
    else if (m->cfg_mode != 87) { put(&b, "hdmi_group=2\nhdmi_mode="); putu(&b, m->cfg_mode); put(&b, "\n"); }
    else if (ext[mi].has_dtd) { put(&b, "hdmi_group=2\nhdmi_mode=87\n"); put_dtd_timings(&b, ext[mi].dtd, aspect_code(m->aspect_w, m->aspect_h)); }
    else {
        put(&b, "hdmi_group=2\nhdmi_mode=87\nhdmi_cvt="); putu(&b, m->width); put(&b, " "); putu(&b, m->height); put(&b, " ");
        putu(&b, (hz + 50) / 100); put(&b, " "); putu(&b, (uint32_t)aspect_code(m->aspect_w, m->aspect_h)); put(&b, " 0 0 0\n");
    }
    put(&b, "max_framebuffer_width="); putu(&b, m->width); put(&b, "\nmax_framebuffer_height="); putu(&b, m->height); put(&b, "\n");
    /* HDMI, not DVI, when the monitor has the HDMI vendor block: DVI carries no sound (docs/09) */
    if (di->flags & PSMB_DI_EDID) put(&b, di->conn & PSMB_CN_HDMI ? "hdmi_drive=2\n" : "hdmi_drive=1\n");
    put(&b, "#aurora68 rotate="); putu(&b, (uint32_t)rotation); put(&b, "\n");
    if (confirm) { put(&b, "#aurora68 confirm="); putu(&b, m->width); put(&b, "x"); putu(&b, m->height); put(&b, "@"); putu(&b, (hz + 50) / 100); put(&b, "\n"); }
    return b.n;
}

/* the lines of in, less those starting with drop (and with one line added at the end) */
static uint32_t filter_lines(const char *in, uint32_t n, const char *drop, const char *add, char *out, uint32_t cap)
{
    uint32_t o = 0, i = 0, dl = slen(drop);
    while (i < n) {
        uint32_t s = i; while (i < n && in[i] != '\n') i++;
        uint32_t k = 0; while (k < dl && s + k < i && in[s + k] == drop[k]) k++;
        if (k != dl) { for (uint32_t j = s; j < i && o + 2 < cap; j++) out[o++] = in[j]; if (o + 1 < cap) out[o++] = '\n'; }
        i++;
    }
    for (uint32_t j = 0; add && add[j] && o + 1 < cap; j++) out[o++] = add[j];
    out[o] = 0;
    return o;
}
uint32_t edid_display_unmark(const char *in, uint32_t n, char *out, uint32_t cap) { return filter_lines(in, n, "#aurora68 confirm=", 0, out, cap); }
uint32_t edid_display_rotate(const char *in, uint32_t n, int rotation, char *out, uint32_t cap)
{
    char line[32]; struct buf b = { line, 0, sizeof line };
    put(&b, "#aurora68 rotate="); putu(&b, (uint32_t)rotation); put(&b, "\n");
    return filter_lines(in, n, "#aurora68 rotate=", line, out, cap);
}

const char *edid_source_name(uint16_t s)
{
    switch (s >> 8) {
    case PSMB_DS_DETAILED: return "detailed timing";
    case PSMB_DS_ESTABLISHED: return "established timing";
    case PSMB_DS_STANDARD: return "standard timing";
    case PSMB_DS_CEA: return "CEA video code";
    }
    return "?";
}

/* ---- the sample ----------------------------------------------------------------------------------
   QEMU's VideoCore has no monitor behind it and answers the EDID tag with nothing. So the testbed
   gets this one (mac.edidtest=sample), made up here for the purpose and not copied from any real
   monitor: "Sample 27Q", maker ID ZZQ, a 27-inch 16:9 panel of 2560 x 1440 (60 x 34 cm) whose
   preferred mode is 2560 x 1440 at 60 Hz with CVT reduced blanking (241.5 MHz: too fast for a Pi 3),
   1920 x 1080 at 60 Hz as its second detailed timing, the usual established and standard timings
   (among them 4:3 and 5:4 ones, the wrong shape for the panel), range limits of 24-76 Hz and 250 MHz,
   and a CEA block with an HDMI vendor block, eleven VICs (3840 x 2160 at 30 Hz among them, too fast
   with no alternative), stereo LPCM at 32, 44.1 and 48 kHz and front speakers only. */
static void dtd(uint8_t *d, uint32_t khz, uint32_t ha, uint32_t hfp, uint32_t hs, uint32_t hbp, uint32_t va, uint32_t vfp, uint32_t vs, uint32_t vbp,
                uint32_t hmm, uint32_t vmm, int hpos, int vpos)
{
    uint32_t hb = hfp + hs + hbp, vb = vfp + vs + vbp, c = khz / 10;
    d[0] = (uint8_t)c; d[1] = (uint8_t)(c >> 8); d[2] = (uint8_t)ha; d[3] = (uint8_t)hb; d[4] = (uint8_t)((ha >> 8) << 4 | (hb >> 8));
    d[5] = (uint8_t)va; d[6] = (uint8_t)vb; d[7] = (uint8_t)((va >> 8) << 4 | (vb >> 8)); d[8] = (uint8_t)hfp; d[9] = (uint8_t)hs;
    d[10] = (uint8_t)((vfp & 15) << 4 | (vs & 15));
    d[11] = (uint8_t)(((hfp >> 8) & 3) << 6 | ((hs >> 8) & 3) << 4 | ((vfp >> 4) & 3) << 2 | ((vs >> 4) & 3));
    d[12] = (uint8_t)hmm; d[13] = (uint8_t)vmm; d[14] = (uint8_t)((hmm >> 8) << 4 | (vmm >> 8)); d[15] = 0; d[16] = 0;
    d[17] = (uint8_t)(0x18 | (vpos ? 4 : 0) | (hpos ? 2 : 0));
}
static void text_desc(uint8_t *d, uint8_t tag, const char *s)
{
    int i = 0;
    d[0] = d[1] = d[2] = 0; d[3] = tag; d[4] = 0;
    for (; s[i] && i < 13; i++) d[5 + i] = (uint8_t)s[i];
    if (i < 13) d[5 + i++] = 0x0a;
    for (; i < 13; i++) d[5 + i] = 0x20;
}
int edid_sample(uint8_t e[256])
{
    for (int i = 0; i < 256; i++) e[i] = 0;
    static const uint8_t head[20] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0,
                                      (('Z' - '@') << 2) | (('Z' - '@') >> 3), (uint8_t)((('Z' - '@') & 7) << 5 | ('Q' - '@')),
                                      0x17, 0x27, 0x4e, 0x61, 0xbc, 0x00, 46, 2026 - 1990, 1, 4 };
    for (int i = 0; i < 20; i++) e[i] = head[i];
    e[20] = 0xa2;                     /* digital, 8 bits a colour, HDMI-a */
    e[21] = 60; e[22] = 34;           /* cm */
    e[23] = 120;                      /* gamma 2.2 */
    e[24] = 0x06;                     /* sRGB, the preferred timing is the native one */
    static const uint8_t chroma[10] = { 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54 };   /* the sRGB primaries */
    for (int i = 0; i < 10; i++) e[25 + i] = chroma[i];
    e[35] = 0x21; e[36] = 0x08; e[37] = 0x00;                        /* 640x480 and 800x600 at 60; 1024x768 at 60 */
    static const uint8_t std[16] = { 0x81, 0x80, 0xa9, 0x40, 0xd1, 0x00, 0xb3, 0x00, 0x95, 0x00, 0x81, 0xc0, 0xd1, 0xc0, 0x01, 0x01 };
    for (int i = 0; i < 16; i++) e[38 + i] = std[i];                 /* 1280x1024, 1600x1200, 1920x1200, 1680x1050, 1440x900, 1280x720, 1920x1080, all 60 */
    dtd(e + 54, 241500, 2560, 48, 32, 80, 1440, 3, 5, 33, 597, 336, 1, 0);      /* CVT-RB 2560 x 1440 at 60 Hz */
    { uint8_t *d = e + 72; d[3] = 0xfd; d[5] = 24; d[6] = 76; d[7] = 30; d[8] = 90; d[9] = 25; d[10] = 0x01; d[11] = 0x0a;
      for (int i = 12; i < 18; i++) d[i] = 0x20; }
    text_desc(e + 90, 0xfc, "Sample 27Q");
    text_desc(e + 108, 0xff, "A68-0001");
    e[126] = 1;
    uint8_t s = 0; for (int i = 0; i < 127; i++) s = (uint8_t)(s + e[i]); e[127] = (uint8_t)(0x100 - s);
    uint8_t *c = e + 128;
    static const uint8_t cea[31] = { 0x02, 0x03, 31, 0x71,
        0x4a, 0x90, 0x04, 0x1f, 0x22, 0x03, 0x02, 0x01, 0x13, 0x05, 0x5f,                   /* video: 16 (native), 4, 31, 34, 3, 2, 1, 19, 5, 95 */
        0x23, 0x09, 0x07, 0x07,                                                             /* audio: LPCM, 2 channels, 32/44.1/48 kHz, 16/20/24 bits */
        0x83, 0x01, 0x00, 0x00,                                                             /* speakers: front left and right */
        0x67, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x38, 0x3c };                                  /* HDMI: 1.0.0.0, deep colour 30/36, Y444, 300 MHz TMDS */
    for (int i = 0; i < 31; i++) c[i] = cea[i];
    dtd(c + 31, 148500, 1920, 88, 44, 148, 1080, 4, 5, 36, 597, 336, 1, 1);    /* 1080p60 */
    s = 0; for (int i = 0; i < 127; i++) s = (uint8_t)(s + c[i]); c[127] = (uint8_t)(0x100 - s);
    return 2;
}
