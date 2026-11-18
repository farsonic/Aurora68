/* mac/pidepth.c - "PiDepth": sets the main screen's depth and quits. MPL-2.0.
 *
 * The depth is the last number in the program's own name: put a copy called "PiDepth 8" into the
 * System Folder's Startup Items and the Mac comes up in 256 colours (1, 2, 4, 8, 16 or 32; no
 * number: 8). For the testbed above all, whose screen otherwise starts in one bit per pixel, so
 * that colour drawing (RiptidePrefs on the Mac, PiColor) can be checked without clicking through
 * the Monitors control panel. SetDepth (the Palette Manager's selector $13) asks the screen's
 * driver for the mode; the colour flag is set so the screen is colour, not grey. Built with the
 * C kit in mac/c (mac/c/README.md).
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

int main(void)
{
	unsigned char name[256];
	short ref = 0, depth = 0, cur;
	void *param = 0;
	int i;
	GetAppParms(name, &ref, &param);
	/* the last number in the name */
	for (i = 1, cur = 0; i <= name[0]; i++) {
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
	set_depth(*(void **)0x8A4, depth);       /* MainDevice: the menu bar's screen */
	return 0;
}
