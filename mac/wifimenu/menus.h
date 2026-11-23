/* mac/wifimenu/menus.h - what the menu bar's extras share (wifimenu.c, btmenu.c, clock.c, bar.c). MPL-2.0. */
#ifndef MENUS_H
#define MENUS_H
#define BOARD         0x20000            /* the network board in the card window */
volatile unsigned char *win(void);
unsigned long rd32(unsigned long off);
void wr32(unsigned long off, unsigned long v);
void add(char **p, const char *s);
unsigned char *pcat(unsigned char *ps, const char *c);
void put_item(unsigned char **p, short t, short l, short b, short r, unsigned char kind, const char *text);
void flash(void *dlg, int item);
void put_icon(unsigned char **p, short t, short l, short id);
void **mac_newhandle(long size);
void pas_useritem(void), pas_dlgfilter(void);
void mlog(const char *t);                 /* a line in the Pi's log ([MAC]) */
extern int dlg_mode;
#define LM_MBARHEIGHT (*(volatile short *)0xBAA)
#define LM_CURMAP     (*(volatile short *)0xA5A)
#define LM_SYSZONE    (*(void **volatile)0x2A6)
#define LM_THEZONE    (*(void **volatile)0x118)
void *mac_deref(void *h);                 /* a handle's block, the master pointer stripped in 24-bit mode */

/* bar.c: where the extras go. Each is drawn only where the program's menus leave room; a rectangle
   whose left is not below its right is not drawn, and takes no clicks. */
extern Rect icon_r, hit_r;                /* the WiFi icon and its press area */
extern Rect bt_r, bt_hit;                 /* the Bluetooth icon */
extern Rect clk_r, clk_hit;               /* the clock */
extern int clk_level;                     /* 2 the long form, 1 the short one, 0 not shown */
void draw_extras(int from_bar);           /* from_bar: just after the system's _DrawMenuBar */
void *wm_begin(void **save);              /* the Window Manager's port, set, its clipping the menu bar */
void wm_end(void *save);
int on_extra(const Rect *hit, long pt);
int menu_down(void);                      /* a menu is down (anyone's): nothing is drawn in the bar */
/* wifimenu.c */
extern int shown;
int picture(void);
void wifi_draw(void *wm);
int c_menuselect(long pt);
/* btmenu.c */
extern int bt_shown;
void bt_make_icons(void);
int bt_picture(void);
void bt_draw(void *wm);
int bt_menuselect(long pt);
int bt_pair_filter(void *dlg, EventRecord *ev, short *item);
/* clock.c */
void clock_init(void);
int clock_width(int level);               /* the clock's width in the bar, the port's font the system font */
void clock_draw(void *wm, short mbh);
int clock_due(void);                      /* the time shown has moved on */
int clock_menuselect(long pt);
void clock_idle(void);                    /* between events: a control panel to open in the Finder */
#endif
