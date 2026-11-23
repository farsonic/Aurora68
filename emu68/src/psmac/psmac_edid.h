/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_edid.h — the HDMI monitor's EDID, parsed and judged for a Pi 3 (psmac_edid.c). Pure C with no
 * kernel calls, so that tools/edid_test.c compiles the same code on the host (make -C mac check).
 */
#ifndef PSMAC_EDID_H
#define PSMAC_EDID_H
#include <stdint.h>
#include "psmb.h"

/* The pixel clock a Pi 0-3's HDMI sends, kHz. Raspberry Pi's documentation (the legacy config.txt
   options, "hdmi_mode") names 1920 x 1200 at 60 Hz with reduced blanking, 154 MHz, as the highest
   mode of every model before the Pi 4; the firmware's own mode list for group 2 has 1600 x 1200 at
   60 Hz (DMT mode 51, 162 MHz) without reduced blanking, which the 400 MHz-ish HDMI PHY of the
   VideoCore IV is generally reported to send. So 162 MHz is the line: what is under it is judged
   usable, what is over it too fast. mac.hdmiclock=MHZ moves it (a Pi 3 with core_freq raised, say). */
#define EDID_PI3_PIXCLK_KHZ  162000u

struct edid_file {                       /* the inputs of a judgement besides the EDID: what the card says */
    const char *config;  uint32_t config_len;      /* config.txt (0: not read) */
    const char *display; uint32_t display_len;     /* aurora68-display.txt (0: not on the card) */
};

/* blocks: n * 128 bytes. Fills everything in di that comes from the EDID (and the judgement of each
   mode against pi_khz); the kernel adds the current state. Returns the number of modes. */
int edid_parse(const uint8_t *blocks, int n, uint32_t pi_khz, struct psmb_dispinfo *di);
/* the current HDMI mode from config.txt and aurora68-display.txt (the one config.txt includes): fills
   cur_refresh, cur_source, cur_text, cfg's INCLUDE and OVERRIDE; marks the CURRENT mode at cur_w x cur_h
   (which the kernel has from the firmware's frame buffer). Returns the rotation the file keeps (0, 90,
   180, 270) and in *confirm whether it waits to be confirmed. */
int edid_current(struct psmb_dispinfo *di, const struct edid_file *f, int *confirm);
/* the size of the mode config.txt and the file it includes set (0 if none): the testbed's mac.hdmisim */
int edid_config_size(const struct edid_file *f, uint32_t *w, uint32_t *h);
/* the suggestion: the panel's own size if the Pi can send it (at a lower refresh if it must), else the
   largest mode of the panel's shape; sets SUGGESTED, suggested and reason */
void edid_suggest(struct psmb_dispinfo *di);
/* aurora68-display.txt for mode m (alt: its reduced-blanking alternative) and a rotation, with the mark
   that it waits to be confirmed (confirm) or without it: the text into out (cap bytes), its length */
uint32_t edid_display_text(const struct psmb_dispinfo *di, int m, int alt, int rotation, int confirm, char *out, uint32_t cap);
/* the same file with its confirm mark taken out (KEEP), into out */
uint32_t edid_display_unmark(const char *in, uint32_t n, char *out, uint32_t cap);
/* the same file with its rotation line replaced */
uint32_t edid_display_rotate(const char *in, uint32_t n, int rotation, char *out, uint32_t cap);
/* Setup and Recovery's Display tab (docs/08 item 111): Aurora68 Display's rule for a mode that can be chosen */
int edid_mode_choosable(const struct psmb_dispmode *m);
/* no EDID: the standard modes the file can name (every 60 Hz DMT mode and 2560x1440), judged against pi_khz */
int edid_standard_modes(struct psmb_dispinfo *di, uint32_t pi_khz);
/* the mode of w x h at hz whole hertz (0: its highest the Pi sends) that can be chosen: its index and in *alt
   whether that is its reduced-blanking alternative; -1 if none */
int edid_find_mode(const struct psmb_dispinfo *di, uint32_t w, uint32_t h, uint32_t hz, int *alt);
/* aurora68-display.txt for "Automatic" (hdmi_group=0: the firmware's choice from the EDID); di may be 0 */
uint32_t edid_display_auto(const struct psmb_dispinfo *di, int rotation, int confirm, char *out, uint32_t cap);
/* config.txt with "include aurora68-display.txt" at its end (added, or moved there): the length, 0 if it does not fit */
uint32_t edid_config_include(const char *in, uint32_t n, char *out, uint32_t cap);
/* the built-in sample: a made-up 27-inch 2560 x 1440 HDMI monitor with sound, two blocks (256 bytes) */
int edid_sample(uint8_t out[256]);
/* VESA CVT 1.1 pixel clock in kHz for w x h at hz, with reduced blanking (rb) or without */
uint32_t edid_cvt_khz(uint32_t w, uint32_t h, uint32_t hz, int rb);
/* one line for the log: "2560x1440 at 60.00 Hz, 241.50 MHz" style pieces */
const char *edid_source_name(uint16_t source);

#endif
