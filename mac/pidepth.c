/* mac/pidepth.c - "Aurora68 Depth" (PiDepth until 21 November 2026): sets the main screen's depth
 * and quits. MPL-2.0.
 *
 * The depth is the last number in the program's own name: put a copy called "Aurora68 Depth 8" into the
 * System Folder's Startup Items and the Mac comes up in 256 colours (1, 2, 4, 8, 16 or 32; no
 * number: 8). For the testbed above all, whose screen otherwise starts in one bit per pixel, so
 * that colour drawing (RiptidePrefs on the Mac, Aurora68 Colour) can be checked without clicking through
 * the Monitors control panel. SetDepth (the Palette Manager's selector $13) asks the screen's
 * driver for the mode; the colour flag is set so the screen is colour, not grey. Built with the
 * C kit in mac/c (mac/c/README.md).
 *
 * With "HDMI" in the name ("Aurora68 Depth HDMI 16") it sets the depth of the second screen instead: the
 * first active screen that is not the main one, which on the SE board's two screens is the HDMI
 * screen (the SE's one-bit screen is the main one and has no other depth). Added on 20 November 2026
 * for the 2560 by 1440 screen (docs/10), whose depths were otherwise reachable only through
 * Monitors on the SE's 9-inch screen.
 */
#include "toolbox.h"

static short set_depth(void *gd, short depth)
{
	short r;
	/* FUNCTION SetDepth(gd: GDHandle; depth, whichFlags, flags: INTEGER): OSErr; selector $0013 with
	   ten bytes of arguments in D0, then _PaletteDispatch. whichFlags = flags = 1: gdDevType, colour */
	__asm__ volatile("clr.w -(%%sp)\n\t"
	                 "move.l %1,-(%%sp)\n\t"
	                 "move.w %2,-(%%sp)\n\t"
	                 "move.w #1,-(%%sp)\n\t"
	                 "move.w #1,-(%%sp)\n\t"
	                 "move.l #0x000A0013,%%d0\n\t"
	                 ".short 0xAAA2\n\t"
	                 "move.w (%%sp)+,%0"
	                 : "=d"(r) : "a"(gd), "d"(depth) : "d0", "d1", "d2", "a0", "a1", "memory", "cc");
	return r;
}

/* the first active screen device that is not the main screen, 0 if there is none (DeviceList,
   low memory $8A8; GDevice: gdFlags at 20, gdNextGD at 30; flag bits 15 screenActive, 13
   screenDevice, 11 mainScreen) */
static void *second_screen(void)
{
	char **gdh = *(char ***)0x8A8;
	for (int n = 0; gdh && n < 8; n++) {
		char *gd = *gdh;
		unsigned short f = *(unsigned short *)(gd + 20);
		if ((f & 0x8000) && (f & 0x2000) && !(f & 0x0800))
			return gdh;
		gdh = *(char ***)(gd + 30);
	}
	return 0;
}

int main(void)
{
	unsigned char name[256];
	short ref = 0, depth = 0, cur;
	void *param = 0, *gd;
	int i, hdmi = 0;
	GetAppParms(name, &ref, &param);
	for (i = 1; i + 3 <= name[0]; i++)
		if (name[i] == 'H' && name[i + 1] == 'D' && name[i + 2] == 'M' && name[i + 3] == 'I')
			hdmi = 1;
	/* the last number in the name, after the "68" of "Aurora68 Depth 16": the program's name since
	   21 November 2026 ("PiDepth 16", its name before, works the same) */
	i = 1;
	if (name[0] >= 8 && name[1] == 'A' && name[2] == 'u' && name[3] == 'r' && name[4] == 'o' &&
	    name[5] == 'r' && name[6] == 'a' && name[7] == '6' && name[8] == '8')
		i = 9;
	for (cur = 0; i <= name[0]; i++) {
		if (name[i] >= '0' && name[i] <= '9') {
			cur = (short)(cur * 10 + (name[i] - '0'));
			continue;
		}
		if (cur)
			depth = cur;
		cur = 0;
	}
	if (cur)
		depth = cur;
	if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 32)
		depth = 8;
	gd = hdmi ? second_screen() : *(void **)0x8A4;   /* the second screen, or MainDevice: the menu bar's screen */
	if (gd)
		set_depth(gd, depth);
	return 0;
}
