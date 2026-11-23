/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/edid_test.c — the kernel's EDID code (emu68/src/psmac/psmac_edid.c) compiled on the host and
 * checked: the CVT clocks against VESA's figures, the built-in sample's parse and judgement, the
 * suggestion, the aurora68-display.txt texts and their reading back through config.txt's include.
 * Run by make -C mac check. With a file argument it parses that EDID (raw blocks, 128 bytes each) and
 * prints what PiDisplay would show, which is how a user's EDID (sent as a file) can be looked at.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "psmac_edid.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void show(const struct psmb_dispinfo *di)
{
    printf("monitor: \"%s\" maker %s (%s) product %04x serial %u \"%s\" week %u year %u EDID %x.%x, %u x %u mm\n",
           di->name, di->maker, di->maker_name, (unsigned)di->product, (unsigned)di->serial, di->serial_text, di->week, di->year,
           di->edid_version >> 8, di->edid_version & 255, di->size_w_mm, di->size_h_mm);
    printf("range: %u-%u Hz, %u-%u kHz, %u kHz max; conn %x tmds %u; audio formats %x lpcm %u ch rates %x bits %x speakers %x\n",
           di->min_vhz, di->max_vhz, di->min_hkhz, di->max_hkhz, (unsigned)di->max_pixclk, (unsigned)di->conn, (unsigned)di->tmds_max,
           (unsigned)di->audio_formats, (unsigned)di->lpcm_channels, (unsigned)di->lpcm_rates, (unsigned)di->lpcm_bits, (unsigned)di->speakers);
    for (int i = 0; i < di->nmodes; i++) {
        const struct psmb_dispmode *m = &di->mode[i];
        printf("  %2d %4ux%-4u %3u.%02u Hz %7u kHz %u:%u %-18s cfg %u/%-3u%s%s%s%s%s%s%s%s%s",
               i, m->width, m->height, m->refresh / 100, m->refresh % 100, (unsigned)m->pixclk, m->aspect_w, m->aspect_h,
               edid_source_name(m->source), m->cfg_group, m->cfg_mode,
               m->flags & PSMB_DM_PREFERRED ? " preferred" : "", m->flags & PSMB_DM_NATIVE ? " native" : "",
               m->flags & PSMB_DM_USABLE ? " usable" : "", m->flags & PSMB_DM_TOOFAST ? " too-fast" : "",
               m->flags & PSMB_DM_SHAPE ? " shape" : "", m->flags & PSMB_DM_INTERLACED ? " interlaced" : "",
               m->flags & PSMB_DM_MACSIZE ? " mac-size" : "", m->flags & PSMB_DM_CURRENT ? " CURRENT" : "",
               m->flags & PSMB_DM_SUGGESTED ? " SUGGESTED" : "");
        if (m->flags & PSMB_DM_ALT) printf(" -> %u Hz %u kHz", m->alt_refresh / 100, (unsigned)m->alt_pixclk);
        printf("\n");
    }
    printf("suggested %d: %s\n", di->suggested == 0xffff ? -1 : di->suggested, di->reason);
}

static int find(const struct psmb_dispinfo *di, int w, int h, int hz)
{
    for (int i = 0; i < di->nmodes; i++)
        if (di->mode[i].width == w && di->mode[i].height == h && (di->mode[i].refresh + 50) / 100 == hz) return i;
    return -1;
}

int main(int argc, char **argv)
{
    static struct psmb_dispinfo di;
    _Static_assert(sizeof(struct psmb_dispmode) == 32, "dispmode");
    printf("sizeof(struct psmb_dispinfo) = %zu\n", sizeof di);
    if (argc > 1) {
        static uint8_t b[128 * 8]; FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        int n = (int)fread(b, 1, sizeof b, f) / 128; fclose(f);
        edid_parse(b, n, 0, &di); edid_suggest(&di); show(&di);
        return 0;
    }
    /* CVT against VESA's figures */
    CHECK(edid_cvt_khz(1920, 1080, 60, 0) == 173000, "1080p60 CVT %u", edid_cvt_khz(1920, 1080, 60, 0));
    CHECK(edid_cvt_khz(2560, 1440, 60, 1) == 241500, "1440p60 RB %u", edid_cvt_khz(2560, 1440, 60, 1));
    CHECK(edid_cvt_khz(1920, 1200, 60, 1) == 154000, "1200p60 RB %u", edid_cvt_khz(1920, 1200, 60, 1));
    CHECK(edid_cvt_khz(1024, 768, 60, 0) == 63500, "768p60 CVT %u", edid_cvt_khz(1024, 768, 60, 0));
    printf("CVT 2560x1440: 60 RB %u, 50 RB %u, 30 RB %u kHz\n", edid_cvt_khz(2560, 1440, 60, 1), edid_cvt_khz(2560, 1440, 50, 1), edid_cvt_khz(2560, 1440, 30, 1));

    uint8_t e[256];
    int n = edid_sample(e);
    for (int k = 0; k < n; k++) { uint8_t s = 0; for (int i = 0; i < 128; i++) s += e[128 * k + i]; CHECK(s == 0, "sample block %d checksum", k); }
    FILE *o = fopen("build/edid-sample.bin", "wb"); if (o) { fwrite(e, 1, 256, o); fclose(o); }
    memset(&di, 0, sizeof di);
    di.cur_w = 1920; di.cur_h = 1080;
    edid_parse(e, n, 0, &di);
    CHECK(di.edid_status == PSMB_ES_OK, "status %u", (unsigned)di.edid_status);
    CHECK(!strcmp(di.name, "Sample 27Q") && !strcmp(di.maker, "ZZQ") && !strcmp(di.serial_text, "A68-0001"), "names");
    CHECK(di.year == 2026 && di.week == 46 && di.serial == 12345678, "date, serial");
    CHECK(di.conn & PSMB_CN_HDMI, "HDMI");
    CHECK(di.lpcm_channels == 2 && di.lpcm_rates == 7 && di.speakers == 1, "audio");
    CHECK(di.max_pixclk == 250000 && di.min_vhz == 24, "range");
    int p = find(&di, 2560, 1440, 60);
    CHECK(p >= 0 && (di.mode[p].flags & (PSMB_DM_PREFERRED | PSMB_DM_NATIVE | PSMB_DM_TOOFAST | PSMB_DM_ALT)) == (PSMB_DM_PREFERRED | PSMB_DM_NATIVE | PSMB_DM_TOOFAST | PSMB_DM_ALT)
          && di.mode[p].alt_refresh == 3000 && di.mode[p].alt_pixclk == 119000, "2560x1440 preferred, too fast, 30 Hz alternative");
    int q = find(&di, 1920, 1080, 60);
    CHECK(q >= 0 && (di.mode[q].flags & PSMB_DM_USABLE) && di.mode[q].cfg_group == 1 && di.mode[q].cfg_mode == 16, "1080p60 usable, VIC 16");
    int r = find(&di, 1600, 1200, 60);
    CHECK(r >= 0 && (di.mode[r].flags & PSMB_DM_SHAPE) && (di.mode[r].flags & PSMB_DM_USABLE) && di.mode[r].cfg_mode == 51, "1600x1200 4:3 wrong shape, DMT 51");
    int u = find(&di, 3840, 2160, 30);
    CHECK(u >= 0 && (di.mode[u].flags & PSMB_DM_TOOFAST) && !(di.mode[u].flags & PSMB_DM_ALT), "2160p30 too fast, no alternative");
    int w = find(&di, 1920, 1200, 60);
    CHECK(w >= 0 && (di.mode[w].flags & PSMB_DM_ALT) && di.mode[w].alt_refresh == 6000 && di.mode[w].alt_pixclk == 154000, "1920x1200: 60 Hz RB");
    static const char config[] = "# config.txt\n[pi3]\ngpu_mem=64\nhdmi_group=1\nhdmi_mode=16\n[all]\ninclude aurora68-display.txt\n";
    struct edid_file f = { config, sizeof config - 1, 0, 0 };
    int conf = 0, rot = edid_current(&di, &f, &conf);
    edid_suggest(&di);
    show(&di);
    CHECK(rot == 0 && !conf && di.cur_refresh == 6000 && (di.cfg & PSMB_CF_INCLUDE) && !(di.cfg & PSMB_CF_OVERRIDE), "current from config.txt");
    CHECK(q >= 0 && (di.mode[q].flags & PSMB_DM_CURRENT), "1080p60 current");
    CHECK(di.suggested == p, "suggested the native size with its 30 Hz");
    /* the file for the suggestion, and reading it back */
    static char t[1024], t2[1024];
    uint32_t tn = edid_display_text(&di, p, 1, 90, 1, t, sizeof t);
    printf("---- aurora68-display.txt (%u bytes)\n%s----\n", tn, t);
    CHECK(strstr(t, "hdmi_cvt=2560 1440 30 3 0 0 1\n") && strstr(t, "hdmi_group=2\nhdmi_mode=87\n") && strstr(t, "#aurora68 confirm=2560x1440@30\n")
          && strstr(t, "#aurora68 rotate=90\n") && strstr(t, "max_framebuffer_width=2560\n") && strstr(t, "hdmi_drive=2\n"), "file text");
    struct psmb_dispinfo d2; memcpy(&d2, &di, sizeof d2);
    for (int i = 0; i < d2.nmodes; i++) d2.mode[i].flags &= ~PSMB_DM_CURRENT;
    d2.cfg = 0; d2.cur_w = 2560; d2.cur_h = 1440;
    struct edid_file f2 = { config, sizeof config - 1, t, tn };
    rot = edid_current(&d2, &f2, &conf);
    CHECK(rot == 90 && conf && d2.cur_refresh == 3000 && d2.cur_source == PSMB_SRC_FILE && d2.confirm_w == 2560 && d2.confirm_refresh == 3000, "read back: rot %d conf %d hz %u", rot, conf, d2.cur_refresh);
    CHECK(d2.mode[p].flags & PSMB_DM_CURRENT, "the 30 Hz alternative marked current on the 2560x1440 entry");
    uint32_t t2n = edid_display_unmark(t, tn, t2, sizeof t2);
    CHECK(!strstr(t2, "confirm=") && strstr(t2, "hdmi_cvt=") && t2n == tn - (uint32_t)strlen("#aurora68 confirm=2560x1440@30\n"), "unmark");
    t2n = edid_display_rotate(t2, t2n, 270, t, sizeof t);
    CHECK(strstr(t, "#aurora68 rotate=270\n") && !strstr(t, "rotate=90"), "rotate");
    /* an hdmi_mode after the include wins, and is reported */
    static const char config2[] = "include aurora68-display.txt\nhdmi_mode=82\n";
    struct edid_file f3 = { config2, sizeof config2 - 1, t, t2n };
    d2.cfg = 0; edid_current(&d2, &f3, &conf);
    CHECK((d2.cfg & PSMB_CF_OVERRIDE) && d2.cur_source == PSMB_SRC_CONFIG, "override");
    /* 1920x1080 by its VIC, and 1600x1200 by DMT */
    edid_display_text(&di, q, 0, 0, 1, t, sizeof t);
    CHECK(strstr(t, "hdmi_group=1\nhdmi_mode=16\n") != 0, "VIC file");
    edid_display_text(&di, w, 1, 0, 1, t, sizeof t);
    CHECK(strstr(t, "hdmi_group=2\nhdmi_mode=69\n") != 0, "1920x1200 RB by DMT 69");

    /* ---- Setup and Recovery's Display tab (docs/08 item 111) ---- */
    /* a mode by its size and refresh, as the page and aurora68.py name it */
    int alt = -1;
    CHECK(edid_find_mode(&di, 2560, 1440, 0, &alt) == p && alt == 1, "2560x1440: its 30 Hz alternative");
    CHECK(edid_find_mode(&di, 2560, 1440, 30, &alt) == p && alt == 1, "2560x1440@30");
    CHECK(edid_find_mode(&di, 2560, 1440, 60, &alt) == -1, "2560x1440@60 cannot be chosen");
    CHECK(edid_find_mode(&di, 1920, 1080, 60, &alt) == q && alt == 0, "1920x1080@60 itself");
    CHECK(edid_find_mode(&di, 3840, 2160, 0, &alt) == -1, "2160p: nothing fits");
    CHECK(edid_find_mode(&di, 1600, 1200, 0, &alt) == r && alt == 0, "1600x1200: the wrong shape, still a choice");
    CHECK(edid_find_mode(&di, 1234, 567, 0, &alt) == -1, "no such size");
    for (int i = 0; i < di.nmodes; i++)
        CHECK(edid_mode_choosable(&di.mode[i]) == (((di.mode[i].flags & (PSMB_DM_USABLE | PSMB_DM_ALT)) != 0) && !(di.mode[i].flags & (PSMB_DM_MACSIZE | PSMB_DM_INTERLACED))), "choosable %d", i);
    /* "Automatic": hdmi_group=0, read back as the firmware's choice from the file */
    tn = edid_display_auto(&di, 180, 1, t, sizeof t);
    printf("---- aurora68-display.txt, Automatic (%u bytes)\n%s----\n", tn, t);
    CHECK(strstr(t, "\nhdmi_group=0\n") && strstr(t, "#aurora68 rotate=180\n") && strstr(t, "#aurora68 confirm=auto\n") && strstr(t, "hdmi_drive=2\n")
          && strstr(t, "its preferred mode 2560x1440") && !strstr(t, "hdmi_mode="), "auto text");
    memcpy(&d2, &di, sizeof d2);
    for (int i = 0; i < d2.nmodes; i++) d2.mode[i].flags &= ~PSMB_DM_CURRENT;
    d2.cfg = 0; d2.confirm_w = 99;
    struct edid_file f4 = { config, sizeof config - 1, t, tn };
    rot = edid_current(&d2, &f4, &conf);
    CHECK(rot == 180 && conf && d2.confirm_w == 0 && d2.cur_source == PSMB_SRC_FILE && d2.cur_refresh == 0 && !strncmp(d2.cur_text, "hdmi_group=0", 12),
          "auto read back: rot %d conf %d source %u '%s'", rot, conf, d2.cur_source, d2.cur_text);
    { uint32_t sw = 1, sh = 1; CHECK(!edid_config_size(&f4, &sw, &sh) && !sw && !sh, "auto: no size for mac.hdmisim"); }
    tn = edid_display_auto(0, 0, 0, t, sizeof t);
    CHECK(strstr(t, "\nhdmi_group=0\n") && !strstr(t, "confirm=") && !strstr(t, "hdmi_drive") && strstr(t, "#aurora68 rotate=0\n"), "auto without an EDID");
    /* config.txt's include line, added or moved to the end */
    static char c1[512], c2[512];
    static const char noinc[] = "gpu_mem=64\nhdmi_group=2\n\nhdmi_mode=82";
    uint32_t cn = edid_config_include(noinc, sizeof noinc - 1, c1, sizeof c1);
    printf("---- config.txt with the include line (%u bytes)\n%s----\n", cn, c1);
    CHECK(cn && !strncmp(c1, "gpu_mem=64\nhdmi_group=2\n\nhdmi_mode=82\n#", 38) && !strstr(c1, "[all]")
          && cn > 29 && !strcmp(c1 + cn - 29, "include aurora68-display.txt\n"), "include added at the end");
    struct edid_file f5 = { c1, cn, 0, 0 };
    d2.cfg = 0; edid_current(&d2, &f5, &conf);
    CHECK((d2.cfg & PSMB_CF_INCLUDE) && !(d2.cfg & PSMB_CF_OVERRIDE), "the added line is read as the include");
    uint32_t cn2 = edid_config_include(c1, cn, c2, sizeof c2);
    CHECK(cn2 == cn && !memcmp(c1, c2, cn), "adding it twice changes nothing");
    cn = edid_config_include(config2, sizeof config2 - 1, c1, sizeof c1);
    struct edid_file f6 = { c1, cn, 0, 0 };
    d2.cfg = 0; edid_current(&d2, &f6, &conf);
    CHECK(cn && !strncmp(c1, "hdmi_mode=82\n", 13) && (d2.cfg & PSMB_CF_INCLUDE) && !(d2.cfg & PSMB_CF_OVERRIDE), "include moved after the overriding hdmi_mode");
    static const char sect[] = "[pi4]\nhdmi_mode:0=4\n[pi3]\nhdmi_mode=16\n";
    cn = edid_config_include(sect, sizeof sect - 1, c1, sizeof c1);
    CHECK(cn && strstr(c1, "hdmi_mode=16\n[all]\n# ") && strstr(c1, "\ninclude aurora68-display.txt\n"), "[all] before the line when the last section is another");
    static const char sect2[] = "[pi3]\nhdmi_mode=16\n[ALL]\ngpu_mem=64\nINCLUDE aurora68-display.txt\n";
    cn = edid_config_include(sect2, sizeof sect2 - 1, c1, sizeof c1);
    /* the kernel reads "include" in lower case only (edid_current), so an upper-case line is not taken for one and stays */
    CHECK(cn && strstr(c1, "[pi3]\nhdmi_mode=16\n[ALL]\ngpu_mem=64\nINCLUDE aurora68-display.txt\n# ") && !strstr(c1, "[all]"), "no second [all] after [ALL]");
    CHECK(edid_config_include(sect, sizeof sect - 1, c1, 40) == 0, "too long for the buffer: 0");
    /* no EDID: the standard list */
    static struct psmb_dispinfo sd;
    int sn = edid_standard_modes(&sd, 0);
    show(&sd);
    CHECK(sn >= 15 && sd.suggested == 0xffff && sd.pi_pixclk == EDID_PI3_PIXCLK_KHZ, "standard list: %d modes", sn);
    int shapes = 0, natives = 0;
    for (int i = 0; i < sd.nmodes; i++) { shapes += !!(sd.mode[i].flags & PSMB_DM_SHAPE); natives += !!(sd.mode[i].flags & PSMB_DM_NATIVE); }
    CHECK(!shapes && !natives, "no shape and no native size without a panel");
    int s1 = edid_find_mode(&sd, 2560, 1440, 0, &alt);
    CHECK(s1 >= 0 && alt == 1 && sd.mode[s1].alt_refresh == 3000 && sd.mode[s1].alt_pixclk == 119000, "standard 2560x1440: 30 Hz reduced");
    tn = edid_display_text(&sd, s1, 1, 0, 1, t, sizeof t);
    CHECK(strstr(t, "hdmi_cvt=2560 1440 30 3 0 0 1\n") && !strstr(t, "hdmi_drive"), "standard 2560x1440 file");
    int s2 = edid_find_mode(&sd, 1920, 1200, 60, &alt);
    edid_display_text(&sd, s2, alt, 0, 1, t, sizeof t);
    CHECK(s2 >= 0 && alt == 1 && strstr(t, "hdmi_group=2\nhdmi_mode=69\n"), "standard 1920x1200: DMT 69");
    int s3 = edid_find_mode(&sd, 1920, 1080, 60, &alt);
    edid_display_text(&sd, s3, alt, 0, 1, t, sizeof t);
    CHECK(s3 >= 0 && alt == 0 && strstr(t, "hdmi_group=2\nhdmi_mode=82\n") && !strcmp(edid_source_name(sd.mode[s3].source), "standard mode"), "standard 1080p: DMT 82");
    int s4 = edid_find_mode(&sd, 1024, 768, 60, &alt);
    edid_display_text(&sd, s4, alt, 90, 1, t, sizeof t);
    CHECK(s4 >= 0 && strstr(t, "hdmi_group=2\nhdmi_mode=16\n") && strstr(t, "#aurora68 rotate=90\n"), "standard 1024x768: DMT 16");
    CHECK(edid_find_mode(&sd, 640, 480, 60, &alt) >= 0 && edid_find_mode(&sd, 640, 480, 75, &alt) == -1, "640x480: 60 Hz only");
    printf("%s: %d failure%s\n", fails ? "FAILED" : "edid_test passed", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
