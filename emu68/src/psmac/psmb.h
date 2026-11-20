/*
 * psmb.h — PiStorm-SE mailbox ABI between the Macintosh (68k) and the Pi service core. MPL-2.0.
 *
 * The Pi appears to the Mac as a NuBus card in slot $9. The bottom 1 MiB of the slot's
 * 16 MiB slot space is ordinary shared RAM ("the window"), reachable at 0xF9000000 in 32-bit
 * mode and at 0x900000 through the IIci ROM's 24-bit map, so drivers need no mode switch.
 * The top 64 KiB of the slot space (0xF9FF0000) holds the declaration ROM the Slot Manager
 * reads at boot; it carries the Mac-side drivers. The second display's big frame buffer is
 * in the slot's 256 MiB super slot space at 0x90000000 (below).
 *
 * All fields are big-endian (68k order); the Pi side converts. Everything is polled: a
 * request is posted by filling the service's command block and incrementing req_seq; the
 * service core executes it and sets done_seq = req_seq (status/result in the block).
 * There are no interrupts in v1; drivers poll from their VBL/Time Manager task or spin
 * with a timeout.
 *
 * Buffer addresses handed to the Pi are 68k LOGICAL addresses; the service core translates
 * them with the MMU shim page by page (24- or 32-bit mode, VM off). Drivers that run under
 * Virtual Memory must LockMemory the buffer first.
 */
#ifndef PSMB_H
#define PSMB_H
#include <stdint.h>

#define PSMB_SLOT            9
#define PSMB_WIN_PHYS        0xF9000000u     /* 32-bit address of the window (24-bit: 0x900000) */
#define PSMB_WIN_SIZE        0x00100000u     /* 1 MiB */
#define PSMB_DECLROM_PHYS    0xF9FF0000u     /* declaration ROM, must end at the top of slot space */
#define PSMB_DECLROM_SIZE    0x00010000u

#define PSMB_MAGIC           0x50534D42u     /* 'PSMB' */
#define PSMB_VERSION         1

/* window layout (offsets from PSMB_WIN_PHYS) */
#define PSMB_OFF_HEADER      0x0000
#define PSMB_OFF_CALIB       0x0080          /* 4 x { u16 low-memory address, u16 value }: the ROM's timing
                                               calibration results, supplied by the Pi (rom/patches/iici.json) */
#define PSMB_OFF_SERVICES    0x0100          /* PSMB_MAX_SERVICES x struct psmb_service */
#define PSMB_OFF_CMD         0x1000          /* PSMB_MAX_SERVICES x struct psmb_cmd (one slot each) */
#define PSMB_OFF_BLKDEV      0x2000          /* PSMB_MAX_BLKDEV x struct psmb_blkdev */
#define PSMB_OFF_NETRX       0x4000          /* net receive ring: header + PSMB_NETRX_SLOTS frames */
#define PSMB_OFF_LOG         0x60000         /* struct psmb_log: the kernel's log for the Mac (below) */
#define PSMB_OFF_LOGDATA     0x61000         /* its 64 KiB ring */
#define PSMB_LOG_SIZE        0x10000
#define PSMB_OFF_DISPLAY     0x58000         /* struct psmb_display: the second screen (below) */
#define PSMB_OFF_BOUNCE      0x80000         /* 512 KiB: the second screen's frame buffer (640 x 480 at 8 bits is 300 KiB) */
#define PSMB_BOUNCE_SIZE     0x80000
#define PSMB_OFF_DISPLAY2    0x59000         /* struct psmb_display of the SE board's HDMI screen (below, "Two screens") */

/* ---- the kernel's log (mac/pilog.c) ------------------------------------------------------------
   Every character of the kernel's log (kprintf: the UART and the HDMI console get the same text)
   also goes into a 64 KiB ring in the window, from boot: what was written before the window
   existed is kept in the kernel and copied in when the window is made. head counts the bytes
   written for ever; the byte numbered n is at data_off + n % size. A reader keeps its own count,
   takes what is between it and head, and starts again from head - size when it fell behind by
   more than the ring holds. The header is big-endian: Emu68 is built big-endian, like the 68k. */
#define PSMB_LOG_MAGIC       0x4136384Cu     /* 'A68L' */
struct psmb_log {
    uint32_t magic, version;
    uint32_t size, data_off;                 /* PSMB_LOG_SIZE, PSMB_OFF_LOGDATA */
    uint32_t head;                           /* Pi: bytes written since boot */
    uint32_t pad[3];
};

/* ---- the second display (docs/10) -------------------------------------------------------------
   The Mac sees a NuBus video card: a video sResource in the declaration ROM whose frame buffer is
   the window's top 512 KiB (MinorBaseOS 0x80000, reachable in 24-bit mode too, which is why it is
   not out in the slot's 16 MiB space), and a video DRVR (mac/pivideo.s) that keeps this block
   current: the depth QuickDraw asked for, the colour table, whether the vertical-blank interrupt
   is wanted. The service core turns the Mac's indexed pixels into the VideoCore's frame buffer
   (the one the boot splash used) through the colour table, sixty times a second, and raises the
   slot interrupt at each of those ticks while the driver wants it: that is the card's VBL, which
   QuickDraw's cursor and the screen's VBL tasks run on. Nothing on the 68k side touches the
   VideoCore; the Pi draws what the Mac drew. */
#define PSMB_DISP_MAGIC      0x41363856u     /* 'A68V' */
#define PSMB_DISP_ENABLED    1u              /* flags: the Mac's driver is open and the block is live */
#define PSMB_DISP_GRAY       2u              /* luminance-mapped colours (cscSetGray) */
struct psmb_display {
    uint32_t magic, version;
    uint32_t width, height;                  /* 640 x 480: the mode list in the declaration ROM says the same */
    uint32_t depth, rowbytes;                /* Mac -> Pi: 1, 2, 4 or 8 and the row pitch QuickDraw uses */
    uint32_t fb_off, fb_size;                /* Pi -> Mac: where the frame buffer is in the window */
    uint32_t flags;                          /* PSMB_DISP_* (Mac) */
    uint32_t vbl_enable;                     /* Mac: 1 = raise the slot interrupt at each tick */
    uint32_t vbl_pending;                    /* Pi sets at a tick, the Mac's handler clears */
    uint32_t vbl_count;                      /* Pi: ticks since boot */
    uint32_t clut_seq;                       /* Mac: +1 after changing clut[] */
    uint32_t frames;                         /* Pi: frames converted */
    uint32_t pad[2];
    uint32_t clut[256];                      /* Mac: 0x00RRGGBB per index */
    /* version 2: direct modes and other resolutions (docs/10). Modes 128+i, i = 0..5: 1, 2, 4, 8, 16, 32
       bits. The Pi fills in where each mode's frame buffer is (a 68k address: the window's top 512 KiB
       for the indexed modes of a 640 x 480 screen, which 24-bit System 6 can reach, else the big region)
       and its row pitch; the driver answers QuickDraw from these tables. */
    uint32_t mode_base[8];                   /* Pi: 68k address of the frame buffer for mode 128+i (0 = no such mode) */
    uint32_t mode_rowbytes[8];               /* Pi: bytes per row for mode 128+i */
    uint32_t big_base, big_size;             /* Pi: the big frame buffer region (PSMB_FB_BIG_PHYS, 9 MiB) */
    uint32_t vc_width, vc_height, vc_depth;  /* Pi: the VideoCore frame buffer behind it */
};
/* the big frame buffer region: 9 MiB at the bottom of slot 9's super slot space ($90000000-$9FFFFFFF,
   the 256 MiB NuBus gives every slot in 32-bit space), enough for the largest mac.screen, 1920 x 1200,
   at 32 bits (8.8 MiB). Pi RAM carved next to the window, 32-bit addressing only. Until 18 November
   it was 8 MiB at $F9100000 inside the slot's 16 MiB space, which the window, its alias at $F9900000
   and the declaration ROM leave no room to grow (docs/10, "1920 by 1200"). The block's layout did not
   change with the move, so its version stays 2. */
#define PSMB_FB_BIG_PHYS     0x90000000u
#define PSMB_FB_BIG_SIZE     0x00900000u

/* ---- two screens on the SE board (docs/10, "Two screens on the SE", 20 November 2026) -------------
   Two cards. Slot 9 is the card it always was, its video sResource ($84) now the SE's own 512 x 342
   screen, one bit only, display block at PSMB_OFF_DISPLAY. Slot $A is a second card with one video
   sResource, the HDMI screen at WxH and every depth, its own driver (".Display_Video_PiHDMI",
   mac/pivideo.s assembled with SCREEN2) and display block at PSMB_OFF_DISPLAY2. Two cards rather than
   two video sResources on one: System 7.1's Monitors takes a screen's depths from the first video
   sResource of its slot, and showed only the SE's one bit for the HDMI screen. The slot $A card is a
   declaration ROM and nothing else: its driver reaches this window at slot 9's addresses, takes slot
   9's interrupt (each driver claims only its own block's VBL) and its modes' vpBaseOffsets point into
   slot 9's space, so the window's top 512 KiB is split between the screens and both keep what a
   24-bit system can reach: the SE's frame (21,888 bytes) in the first 32 KiB at $F9980000, the HDMI
   screen's indexed modes that fit in the other 480 KiB at $F9988000, its larger and direct modes in
   the big region at $90000000. The declaration ROM file for the SE (psmac-se-WxH.declrom) holds
   both ROMs, slot $A's first and slot 9's at the end; the kernel serves slot $A's from the top 16 KiB
   of the window, mapped at the top of slot $A's space, on the SE board with mac.hdmi=screen only.
   The kernel and tools/mac/mkdeclrom.py --se apply the same rules. */
#define PSMB_SE_FRAME_SIZE   0x8000u
#define PSMB_OFF_BOUNCE2     (PSMB_OFF_BOUNCE + PSMB_SE_FRAME_SIZE)
#define PSMB_BOUNCE2_SIZE    (PSMB_BOUNCE_SIZE - PSMB_SE_FRAME_SIZE)
#define PSMB_SLOT2           0xA
#define PSMB_OFF_DECLROM2    0x7C000         /* slot $A's declaration ROM in the window ... */
#define PSMB_DECLROM2_SIZE   0x4000u
#define PSMB_DECLROM2_PHYS   0xFAFFC000u     /* ... and where the Mac reads it: the top of slot $A's space */
#define PSMB_HW_SCREEN2      0x5048u         /* the HDMI screen's drHw, 'PH' (the SE's is 'PV', 0x5056) */

#define PSMB_MAX_SERVICES    8
#define PSMB_MAX_BLKDEV      4

/* service types */
#define PSMB_SVC_INFO        1
#define PSMB_SVC_BLOCK       2
#define PSMB_SVC_NET         3
#define PSMB_SVC_HID         4
#define PSMB_SVC_RTC         5
#define PSMB_SVC_ALICE       6      /* Alice68 boards: param[0] network board, [1] status page, [2] input ring (window offsets) */

struct psmb_header {
    uint32_t magic;           /* 'PSMB' */
    uint16_t version;
    uint16_t nservices;
    uint32_t features;        /* PSMB_FEAT_* */
    uint32_t pi_ram_mb;       /* Pi RAM available to services */
    uint32_t mac_ram_mb;      /* Mac RAM configured (mac.ram) */
    uint32_t uptime_ms;       /* updated by the service core every heartbeat */
    char     fw_version[32];  /* Emu68 psmac build string */
};
#define PSMB_FEAT_BLOCK      (1u << 0)
#define PSMB_FEAT_NET        (1u << 1)
#define PSMB_FEAT_RTC        (1u << 2)
#define PSMB_FEAT_HID        (1u << 3)

struct psmb_service {          /* 32 bytes */
    uint16_t type;             /* PSMB_SVC_* , 0 = empty */
    uint16_t id;               /* index into the command block table */
    uint32_t cmd_off;          /* offset of this service's command block in the window */
    uint32_t caps;             /* service-specific */
    uint32_t param[5];         /* service-specific (block: ndev; net: MAC hi/lo, rx ring off) */
};

/* command block: the Mac fills cmd, args, buf and len, then increments req_seq; the Pi sets status,
   result, xfer and copies req_seq to done_seq. 64 bytes. */
struct psmb_cmd {
    uint32_t req_seq;
    uint32_t done_seq;
    uint16_t cmd;
    uint16_t flags;
    uint32_t arg0, arg1, arg2, arg3;
    uint32_t buf;              /* 68k logical address */
    uint32_t len;              /* bytes */
    int32_t  status;           /* 0 OK, else PSMB_E_* (negative) */
    uint32_t result;           /* command specific */
    uint32_t xfer;             /* bytes transferred */
    uint32_t reserved[4];
};

#define PSMB_E_BADCMD        (-1)
#define PSMB_E_BADDEV        (-2)
#define PSMB_E_RANGE         (-3)
#define PSMB_E_IO            (-4)
#define PSMB_E_BADADDR       (-5)   /* buffer page not mapped */
#define PSMB_E_RO            (-6)
#define PSMB_E_NODATA        (-7)
#define PSMB_E_BUSY          (-8)

/* INFO service commands */
#define PSMB_INFO_PING       1      /* result = ~arg0 */
#define PSMB_INFO_LOG        2      /* buf/len: text appended to the Pi console log */
#define PSMB_INFO_TIME       3      /* result = seconds since 1904-01-01 (Mac epoch), 0 if unknown */
#define PSMB_INFO_SETOPT     4      /* buf/len: an edit list for cmdline.txt ("-NAME" / "+NAME=VALUE" lines),
                                       written by the card writer (psmac_card.c); status PSMB_E_RO without a card */
#define PSMB_INFO_OPTSTATE   5      /* result: 0 nothing asked, 1 being written, 2 saved, 3 failed */
#define PSMB_INFO_SAVELOG    6      /* the log ring into aurora68.log in the SD card's root (psmac_card.c), secrets
                                       hidden; status PSMB_E_RO without a writable card */
#define PSMB_INFO_LOGSTATE   7      /* result: 0 nothing asked, 1 being written, 2 saved, 3 failed */

/* BLOCK service: arg0 = device index, arg1 = first block, arg2 = block count, buf = data */
#define PSMB_BLK_READ        1
#define PSMB_BLK_WRITE       2
#define PSMB_BLK_FLUSH       3
#define PSMB_BLK_INFO        4      /* refresh struct psmb_blkdev table */
#define PSMB_BLK_EJECT       5
#define PSMB_BLK_BOOT        6      /* PiDisk's boot record (mac/pidisk_boot.s), as the Start Manager looks for a startup
                                       disk: result 0 = the card's image first, else the driver reference number of the
                                       SCSI disk to try first (psmac_mbox.c, mac.boot=); status PSMB_E_BUSY while the
                                       card has not yet been looked at for images */

struct psmb_blkdev {           /* 64 bytes, PSMB_OFF_BLKDEV + idx*64 */
    uint32_t blocks;           /* capacity in 512-byte blocks, 0 = no device */
    uint32_t block_size;       /* 512 */
    uint32_t flags;            /* PSMB_BD_* */
    uint32_t reserved;
    char     name[48];         /* C string, Mac Roman */
};
#define PSMB_BD_PRESENT      (1u << 0)
#define PSMB_BD_WRITABLE     (1u << 1)
#define PSMB_BD_REMOVABLE    (1u << 2)

/* NET service: Ethernet frames. TX by command; RX through a ring the Pi fills. */
#define PSMB_NET_SEND        1      /* buf/len = frame (dst, src, type, payload), no FCS */
#define PSMB_NET_GETADDR     2      /* result = MAC bytes 0..3, arg0 out: bytes 4..5 in xfer */
#define PSMB_NET_SETFILTER   3      /* arg0 = PSMB_NETF_* */
#define PSMB_NET_LINK        4      /* result = 1 if link up */
#define PSMB_NETF_PROMISC    (1u << 0)
#define PSMB_NETF_MULTICAST  (1u << 1)

#define PSMB_NETRX_SLOTS     32
#define PSMB_NETRX_FRAME     1536
struct psmb_netrx {            /* at PSMB_OFF_NETRX */
    uint32_t head;             /* written by the Pi: next slot to fill */
    uint32_t tail;             /* written by the Mac: next slot to consume; empty when tail == head */
    uint32_t dropped;
    uint32_t reserved;
    struct { uint32_t len; uint32_t reserved; uint8_t data[PSMB_NETRX_FRAME]; } slot[PSMB_NETRX_SLOTS];
};

#endif
