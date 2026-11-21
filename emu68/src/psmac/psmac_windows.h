/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_windows.h - the Mac's windows as the kernel reads them from the Window Manager's records
 * (psmac_windows.c), for the remote control's /windows and /screenshot?window= (psmac_remote.c, docs/13).
 */
#ifndef PSMAC_WINDOWS_H
#define PSMAC_WINDOWS_H
#include <stdint.h>

#define PSMAC_WIN_MAX   128       /* windows in one answer, every layer together */
#define PSMAC_WIN_APPS  24        /* applications (layers) named in one answer */

struct psmac_window {
    uint32_t ptr;                 /* the WindowPtr: the window's id while it exists */
    int16_t  kind;                /* windowKind */
    uint8_t  visible, hilited;
    uint8_t  has_rect, has_content;
    int16_t  rect[4];             /* the structure region's bounding box, global: left, top, right, bottom */
    int16_t  content[4];          /* the content region's */
    int8_t   app;                 /* index into psmac_winlist.app, -1 if the owner is not known */
    uint8_t  title_len;
    uint8_t  title[255];          /* Mac Roman, as the Window Manager keeps it */
};
struct psmac_winlist {
    int n;                        /* windows in the answer, front to back */
    int consistent;               /* two readings in a row agreed */
    int truncated;                /* more windows than PSMAC_WIN_MAX, or a list that did not end */
    int layers;                   /* System 7's layers walked; 0 when the list was WindowList alone */
    const char *how;              /* how the list was found, for the answer's X-Aurora-Windows header */
    int napps;
    uint8_t app_len[PSMAC_WIN_APPS];
    uint8_t app[PSMAC_WIN_APPS][32];   /* the owners' names, Mac Roman */
};
/* the windows now, front to back; returns info->n */
int psmac_windows_read(struct psmac_window *w, int max, struct psmac_winlist *info);
/* Mac Roman to UTF-8; with json set, '"', '\\' and control characters escaped for a JSON string.
   Returns the bytes written to out (NUL-terminated within max). */
unsigned psmac_macroman_utf8(const uint8_t *in, unsigned n, char *out, unsigned max, int json);

#endif
