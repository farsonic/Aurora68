/* mac/pidisplay/pdcommon.h - what PiDisplay and PiScrn share: the kernel's INFO service for the HDMI
 * monitor, and the "Keep this resolution?" question. MPL-2.0. */
#ifndef PDCOMMON_H
#define PDCOMMON_H
#include "toolbox.h"
#include "psmb.h"

_Static_assert(sizeof(struct psmb_dispinfo) == 1956 && __builtin_offsetof(struct psmb_dispinfo, mode) == 420, "struct psmb_dispinfo as the kernel lays it out");

/* the Aurora68 card's window (psmb.h), in the addressing mode in force, or 0 when this Mac has none */
volatile unsigned char *pd_window(void);
/* one INFO command (psmb.h PSMB_INFO_*), waited for; buf/len a Mac buffer the kernel reads or fills.
   Returns the status (0, or PSMB_E_*; -100 when the kernel did not answer) */
long pd_info(volatile unsigned char *w, int cmd, unsigned long a0, unsigned long a1, unsigned long a2,
             void *buf, unsigned long len, unsigned long *result);
/* the HDMI monitor and its settings (PSMB_INFO_DISPINFO): 1 when di was filled */
int pd_fetch(volatile unsigned char *w, struct psmb_dispinfo *di);
/* a line for the kernel's log, as "[MAC] text" */
void pd_log(volatile unsigned char *w, const char *text);

/* "Keep this resolution?": a modal dialog in the middle of the main screen (on the SE board the SE's
   own, which shows whatever the HDMI monitor makes of the new mode), Keep (the default: Return,
   Enter) and Revert (Escape, Command-period), counting down from 15 seconds, after which it answers
   Revert by itself. Tells the kernel ASKING when it appears and the answer when it goes. Returns 1
   for Keep. */
int pd_confirm(volatile unsigned char *w, const struct psmb_dispinfo *di);

/* small text helpers */
char *pd_put(char *p, const char *s);
char *pd_num(char *p, unsigned long v);
char *pd_hz(char *p, unsigned v);          /* centihertz as "60" or "59.94" */
char *pd_mhz(char *p, unsigned long khz);  /* "241.5" */

/* glue.s: Pascal callbacks of dialogs (c_useritem, c_dlgfilter, in the program), interrupts */
void pas_useritem(void), pas_dlgfilter(void);
unsigned short get_sr(void);
void set_sr(unsigned short sr);
void **mac_newhandle(long size);
/* the dialog's filter and user item, shared by PiDisplay's other dialogs (pdcommon.c sets the mode) */
extern int pd_dlg_mode;                    /* 0 the confirm question, 1 a two-button alert, 2 the About box */
void c_useritem(void *dlg, int item);
int c_dlgfilter(void *dlg, EventRecord *ev, short *item);
/* a DITL item into a handle being built: top, left, bottom, right, type, text */
void pd_item(unsigned char **p, int t, int l, int b, int r, int type, const char *text);
void pd_icon(unsigned char **p, int t, int l, int id);
/* a dialog of the guidelines' alert shape: centred across the main screen, a third of the way down */
void pd_centre(Rect *r, int w, int h);
/* two buttons and a message, item 1 the default (outlined): 1 for it, 2 for the other */
int pd_alert(const char *msg, const char *ok, const char *cancel, int icon);

#define LM_MAINDEVICE (*(char ***)0x8A4)
#ifndef LM_DEVICELIST
#define LM_DEVICELIST (*(char ***)0x8A8)
#endif
#define LM_CURAPNAME  ((unsigned char *)0x910)
#define GD_RECT(gdh)  ((Rect *)((char *)*(gdh) + 34))   /* GDevice gdRect */
#define GD_NEXT(gdh)  (*(char ***)((char *)*(gdh) + 30))
#define GD_FLAGS(gdh) (*(unsigned short *)((char *)*(gdh) + 20))

#endif
