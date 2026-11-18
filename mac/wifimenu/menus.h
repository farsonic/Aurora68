/* mac/wifimenu/menus.h - what the WiFi and Bluetooth menu extras share (wifimenu.c, btmenu.c). MPL-2.0. */
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
extern int dlg_mode;
extern Rect icon_r;                       /* the WiFi icon: the Bluetooth one goes to its left */
#define LM_MBARHEIGHT (*(volatile short *)0xBAA)
/* btmenu.c */
extern int bt_shown;
void bt_make_icons(void);
int bt_picture(void);
void bt_draw(void *wm, short mbh);
int bt_menuselect(long pt);
int bt_pair_filter(void *dlg, EventRecord *ev, short *item);
#endif
