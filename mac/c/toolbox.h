/* mac/c/toolbox.h - the classic Mac OS Toolbox for the C programs in mac/ (mac/c/README.md):
 * the few structures they use, low-memory globals, and the calls (generated from traps.txt into
 * build/toolbox_gen.h). Written for Aurora68 from Inside Macintosh; nothing is copied from
 * Apple's interfaces. MPL-2.0. */
#ifndef MAC_TOOLBOX_H
#define MAC_TOOLBOX_H

typedef struct { short top, left, bottom, right; } Rect;
typedef struct { short v, h; } Point;
/* a Point passed by value: v in the high word */
static inline long pt_long(Point p) { return ((long)(unsigned short)p.v << 16) | (unsigned short)p.h; }

typedef struct {
	short what;
	long message, when;
	Point where;
	short modifiers;
} EventRecord;

enum { nullEvent, mouseDown, mouseUp, keyDown, keyUp, autoKey, updateEvt, diskEvt, activateEvt,
       osEvt = 15 };
#define everyEvent   0xFFFF
#define cmdKey       0x0100
#define shiftKey     0x0200
#define optionKey    0x0800
#define activeFlag   0x0001
#define charCodeMask 0xFF
#define suspendResumeMessage 1
#define resumeFlag   1

enum { inDesk, inMenuBar, inSysWindow, inContent, inDrag, inGrow, inGoAway, inZoomIn, inZoomOut };
#define inUpButton   20
#define inDownButton 21
#define inPageUp     22
#define inPageDown   23
#define inThumb      129
#define documentProc 0
#define zoomDocProc  8
#define scrollBarProc 16
#define pushButProc  0
#define checkBoxProc 1
#define radioButProc 2

/* the GrafPort fields the programs read */
#define PORT_RECT(w)  ((Rect *)((char *)(w) + 16))
/* TextEdit's record (Inside Macintosh I, TextEdit) */
typedef struct {
	Rect destRect, viewRect, selRect;
	short lineHeight, fontAscent;
	Point selPoint;
	short selStart, selEnd, active;
	long wordBreak, clikLoop, clickTime;
	short clickLoc;
	long caretTime;
	short caretState, just, teLength;
	char **hText;
	short recalBack, recalLines, clikStuff, crOnly, txFont;
	char txFace, filler;
	short txMode, txSize;
	void *inPort;
	long highHook, caretHook;
	short nLines;
	short lineStarts[1];
} TERec;
typedef TERec **TEHandle;
/* m68k GCC aligns every field to 2 bytes, as the Toolbox does */
_Static_assert(__builtin_offsetof(TERec, nLines) == 94, "TERec");
_Static_assert(sizeof(EventRecord) == 16, "EventRecord");

/* low memory */
#define LM_TICKS      (*(volatile unsigned long *)0x16A)
#define LM_MMU32BIT   (*(volatile unsigned char *)0xCB2)
#define LM_GRAYRGN    (*(void ***)0x9EE)          /* the desktop region of every screen */
#define LM_TESCRPLEN  (*(volatile short *)0xAB0)
#define LM_TESCRPH    (*(char ***)0xAB4)
#define LM_SYSVERSION (*(volatile short *)0x15A)
/* a region's bounding box */
#define RGN_BBOX(h)   ((Rect *)((char *)*(h) + 2))

#define TYPE(a, b, c, d) (((long)(a) << 24) | ((long)(b) << 16) | ((long)(c) << 8) | (long)(d))

#include "toolbox_gen.h"

/* libc.c */
void *memcpy(void *d, const void *s, unsigned long n);
void *memmove(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
int memcmp(const void *a, const void *b, unsigned long n);
unsigned long strlen(const char *s);
/* a C string as a Pascal string in buf (at most 255 characters); returns buf */
unsigned char *pstr(unsigned char *buf, const char *c);

/* crt0.s: NewPtrClear in the application heap (NULL when there is no room) */
void *mac_newptr(long size);
#define inCheckBox   11
#define inButton     10
/* the File Manager's parameter block (Inside Macintosh IV, File Manager): the I/O calls' fields
   and, over the same bytes from offset 28, the file information calls' */
typedef struct {
	void *qLink;
	short qType, ioTrap;
	void *ioCmdAddr, *ioCompletion;
	short ioResult;
	const unsigned char *ioNamePtr;
	short ioVRefNum;
	short ioRefNum;                 /* ioFRefNum for the file information calls */
	char ioVersNum, ioPermssn;
	union {
		struct { long ioMisc; void *ioBuffer; long ioReqCount, ioActCount; short ioPosMode; long ioPosOffset; } io;
		struct { short ioFDirIndex; char ioFlAttrib, ioFlVersNum; long fdType, fdCreator; short fdFlags;
		         Point fdLocation; short fdFldr; long ioFlNum; short ioFlStBlk; long ioFlLgLen, ioFlPyLen;
		         short ioFlRStBlk; long ioFlRLgLen, ioFlRPyLen, ioFlCrDat, ioFlMdDat; } f;
	} u;
} ParamBlock;
_Static_assert(sizeof(ParamBlock) == 80 && __builtin_offsetof(ParamBlock, u.f.fdType) == 32, "ParamBlock");
#define fsWrPerm   2
#define fsFromStart 1
#define dupFNErr   (-48)
short mac_PBOpen(ParamBlock *pb), mac_PBClose(ParamBlock *pb), mac_PBWrite(ParamBlock *pb),
      mac_PBCreate(ParamBlock *pb), mac_PBGetFInfo(ParamBlock *pb), mac_PBSetFInfo(ParamBlock *pb),
      mac_PBSetEOF(ParamBlock *pb), mac_PBFlushVol(ParamBlock *pb);
/* Standard File's answer (SFPutFile, SFGetFile) */
typedef struct {
	char good, copy;
	long fType;
	short vRefNum, version;
	unsigned char fName[64];
} SFReply;
_Static_assert(__builtin_offsetof(SFReply, fName) == 10, "SFReply");
#define LM_TIME       (*(volatile unsigned long *)0x20C)  /* the clock: seconds since 1904, local time */
#define LM_KEYMAP     ((volatile unsigned char *)0x174)  /* the keys held: key code k is bit k % 8 of byte k / 8 */

/* crt0.s: a control's action procedure, calling c_ctl_action(ctl, part) of the program */
void pas_ctl_action(void);
void c_ctl_action(void *ctl, int part);

#endif
