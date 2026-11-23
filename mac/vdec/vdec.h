/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/vdec/vdec.h - what the Aurora68 Video Decoder's components share: the card's driver, the log, a few
 * string helpers, and the register-based Toolbox calls in entry.s (23 November 2026, docs/08 item 113).
 * vdec.c is the five decompressors ('imdc'), import.c the MPEG-1 movie import ('eat ').
 */
#ifndef VDEC_H
#define VDEC_H
#include "../../emu68/src/psmac/psmac_video.h"

#define TYPE(a, b, c, d) ((long)(a) << 24 | (long)(b) << 16 | (long)(c) << 8 | (long)(d))
#define LM_MMU32BIT (*(volatile unsigned char *)0xCB2)
#define LM_TICKS    (*(volatile unsigned long *)0x16A)
#define LM_MEMERR   (*(volatile short *)0x220)
#define badComponentSelector 0x80008002L

void *memset(void *d, int c, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n);

/* entry.s: the Component Manager */
void cm_set_storage(long ci, long storage);
long cm_find_next(long comp, void *desc);
long cm_open(long comp);
short cm_get_info(long comp, void *desc);
void cm_close(long ci);
long cm_delegate(void *params, long ci);
void call_completion(void *proc, int err, int flags, long refcon);
/* entry.s: the Device, File and Memory Managers (register-based: a parameter block in A0, the result in D0) */
short dm_open(void *pb), dm_control(void *pb), dm_status(void *pb);
short fm_read(void *pb), fm_write(void *pb), fm_close(void *pb), fm_geteof(void *pb), fm_seteof(void *pb), fm_getcatinfo(void *pb);
void *mm_newptr_clear(long size);
void *mm_newptr(long size);
void mm_disposeptr(void *p);
long mm_handlesize(void *h);
void *mm_newhandle(long size);
void mm_disposehandle(void *h);
short mm_sethandlesize(void *h, long size);
void mm_hlock(void *h), mm_hunlock(void *h);
void *mm_strip(void *p);
unsigned long tm_micro(void);

/* the card's driver (vdec.c) */
int drv_open(void);
int drv_enabled(void);
short drv_frame(struct a68v_freq *r);
extern short drv_err;

/* the log, through slot 9's INFO service: "[MAC] A68 Video Decoder: text" (vdec.c) */
void vlog(const char *text);
char *put(char *p, const char *s);
char *num(char *p, unsigned long v);
char *fourcc(char *p, long t);

/* the import (import.c): every call to an 'eat ' instance */
struct imp;
long imp_open(long self, struct imp **out);
long imp_dispatch(unsigned char *params, struct imp *in, short what);

/* the File Manager's records (Inside Macintosh: Files), 2-byte aligned as m68k GCC lays them out */
typedef struct { short vRefNum; long parID; unsigned char name[64]; } FSSpec;
typedef struct {
	void *qLink; short qType, ioTrap; void *ioCmdAddr, *ioCompletion; short ioResult;
	unsigned char *ioNamePtr; short ioVRefNum, ioRefNum; char ioVersNum, ioPermssn;
	long ioMisc; void *ioBuffer; long ioReqCount, ioActCount; short ioPosMode; long ioPosOffset;
} IOParam;
typedef struct {
	void *qLink; short qType, ioTrap; void *ioCmdAddr, *ioCompletion; short ioResult;
	unsigned char *ioNamePtr; short ioVRefNum, ioFRefNum; char ioFVersNum, filler1;
	short ioFDirIndex; char ioFlAttrib, ioACUser;
	char ioFlFndrInfo[16];
	long ioDirID;                    /* a folder's own ID comes back here (ioDrDirID) */
	short ioFlStBlk; long ioFlLgLen, ioFlPyLen; short ioFlRStBlk; long ioFlRLgLen, ioFlRPyLen;
	unsigned long ioFlCrDat, ioFlMdDat, ioFlBkDat;
	char ioFlXFndrInfo[16];
	long ioFlParID, ioFlClpSiz;
} CInfoPB;
_Static_assert(sizeof(FSSpec) == 70, "FSSpec");
_Static_assert(__builtin_offsetof(IOParam, ioPosOffset) == 46 && sizeof(IOParam) == 50, "IOParam");
_Static_assert(__builtin_offsetof(CInfoPB, ioDirID) == 48 && __builtin_offsetof(CInfoPB, ioFlMdDat) == 76 &&
               sizeof(CInfoPB) == 108, "CInfoPB");
#endif
