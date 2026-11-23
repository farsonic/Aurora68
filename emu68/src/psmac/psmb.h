/*
 * psmb.h — Aurora68 mailbox ABI between the Macintosh (68k) and the Pi service core. MPL-2.0.
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
/* 0x52000 and 0x5A000: the Aurora68 Agent's transfer buffer and request block (below, PSMB_OFF_AGENTBUF, PSMB_OFF_AGENT) */

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
    uint32_t big_base, big_size;             /* Pi: the big frame buffer region (PSMB_FB_BIG_PHYS, 9 or 15 MiB) */
    uint32_t vc_width, vc_height, vc_depth;  /* Pi: the VideoCore frame buffer behind it */
};
/* the big frame buffer region: 9 MiB at the bottom of slot 9's super slot space ($90000000-$9FFFFFFF,
   the 256 MiB NuBus gives every slot in 32-bit space), enough for 1920 x 1200 at 32 bits (8.8 MiB).
   Pi RAM carved next to the window, 32-bit addressing only. Until 18 November it was 8 MiB at
   $F9100000 inside the slot's 16 MiB space, which the window, its alias at $F9900000 and the
   declaration ROM leave no room to grow (docs/10, "1920 by 1200"). The block's layout did not
   change with the move, so its version stays 2.
   2560 x 1440 (20 November 2026, docs/10 "2560 by 1440"): 2560 x 4 x 1440 is 14,745,600 bytes, more than
   the 9 MiB. The super slot space has 256 MiB, so the address range is no limit; the Pi RAM behind it
   is carved once at start-up and taken from what the Mac and the disk images would get, so it grows
   only when the screen needs it: 9 MiB (PSMB_FB_BIG_SIZE) for every size up to 1920 x 1200, as before,
   and the screen's 32-bit frame rounded up to a MiB, at most PSMB_FB_BIG_MAX, for a larger one
   (psmac_setup_memory; the size in use is psmac_fb_big_size(), and the display block's big_size):
   15 MiB at 2560 x 1440. The same day the HDMI screen became any size the firmware sets (the
   declaration ROM is rewritten for it at start-up, docs/10 "Any size"), so the ceiling is 32 MiB,
   which holds 3840 x 2160 at 32 bits; a depth whose frame would not fit is left out of the ROM. */
#define PSMB_FB_BIG_PHYS     0x90000000u
#define PSMB_FB_BIG_SIZE     0x00900000u     /* the region for screens up to 1920 x 1200 */
#define PSMB_FB_BIG_MAX      0x02000000u     /* the most it grows to: 32 MiB, 3840 x 2160 at 32 bits */
/* the screen sizes the kernel takes (mac.screen=, or the firmware's HDMI mode): the SE's own at the
   bottom, at the top as wide as QuickDraw's coordinates and the ROM's 16-bit bounds comfortably allow */
#define PSMB_SCREEN_MIN_W    512u
#define PSMB_SCREEN_MIN_H    342u
#define PSMB_SCREEN_MAX_W    4096u
#define PSMB_SCREEN_MAX_H    2560u
#define PSMB_ROWBYTES_MAX    0x3FFEu         /* QuickDraw's limit on a pixel map's rowBytes: a depth with wider rows is left out */
/* A mode's row pitch: its pixels rounded up to whole bytes, then to a long. QuickDraw needs an even
   rowBytes, and Apple's cards keep rows long-aligned, which 32-bit QuickDraw's blits like; every width
   that is a multiple of 32 (all the sizes before 20 November 2026) comes out as it always did, and
   1366 at one bit gets 172 bytes rather than 171. tools/mac/mkdeclrom.py applies the same rule. */
static inline uint32_t psmb_rowbytes(uint32_t w, uint32_t depth) { return ((w * depth + 7) / 8 + 3) & ~3u; }

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

/* ---- the sound card (20 November 2026, docs/09 "Sound through HDMI") ------------------------------
   One more function of slot 9's card: 16-bit stereo at 44.1 kHz, played by the Pi through HDMI or the
   3.5 mm jack (psmac_audio.c, core 1). The Mac's side is the PiSound extension (mac/pisound.s), a Sound
   Manager 3 output device component ('sdev') that takes the Sound Manager's mixed samples and writes
   them into the ring; the Pi takes them out at the output's own pace and says how far it got (tail).
   The block lies in the window's first megabyte, so a 24-bit system reaches it too ($911000). Frames
   are left then right, each a big-endian signed 16-bit word, exactly what the Sound Manager's 'twos'
   format holds. head and tail count frames for ever; head - tail is what is queued. */
/* ---- the video card, slot $C (22 November 2026, docs/16-video-card.md, psmac_videocard.c) -----------
   A card of its own, not part of slot 9's window: its block and ring (psmac_video.h) are 1 MiB of Pi RAM
   carved beside the big frame buffer, at the start of slot $C's space and again where a 24-bit Mac
   reaches the slot ($C00000, which NuBus geometry puts at $FCC00000, as $900000 is $F9900000), and its
   declaration ROM (psmac_video_declrom.h) is the slot's top 16 KiB. Slot $C because the IIci's built-in
   video uses slot $B's space ($FBB08000); $C, $D and $E are the IIci's real NuBus slots, empty here. */
#define PSMB_VID_SLOT        0xCu
#define PSMB_VID_WIN_PHYS    0xFC000000u
#define PSMB_VID_WIN24_PHYS  0xFCC00000u
#define PSMB_VID_ROM_PHYS    0xFCFFC000u
#define PSMB_VID_ROM_SIZE    0x4000u

#define PSMB_OFF_SOUND       0x11000         /* struct psmb_sound */
#define PSMB_OFF_SNDRING     0x12000         /* the ring: PSMB_SND_FRAMES frames of 4 bytes */
#define PSMB_SND_FRAMES      8192u           /* 32 KiB, 186 ms at 44.1 kHz */
#define PSMB_SND_MAGIC       0x41363853u     /* 'A68S' */
#define PSMB_SND_OUT_NONE    0u              /* out_kind: nothing plays the card (mac.audio=off, or no output found) */
#define PSMB_SND_OUT_HDMI    1u
#define PSMB_SND_OUT_JACK    2u
#define PSMB_SND_OUT_FILE    3u              /* the testbed: a file through semihosting (mac.audiodump=) */
struct psmb_sound {
    uint32_t magic, version;                 /* Pi: 'A68S', 1 */
    uint32_t rate;                           /* Pi: the card's rate, 16.16 fixed (44100 << 16), as the Sound Manager writes rates */
    uint32_t frames;                         /* Pi: the ring's size in frames */
    uint32_t out_rate;                       /* Pi: the output's rate in Hz */
    uint32_t out_kind;                       /* Pi: PSMB_SND_OUT_*; the Mac registers its component only when it is not NONE */
    uint32_t tail;                           /* Pi: frames taken from the ring */
    uint32_t underruns;                      /* Pi: frames the ring did not have while the Mac said it was playing */
    uint32_t test;                           /* Pi: mac.audiotest=card: the extension plays its stereo test at start */
    uint32_t pad0[3];
    uint32_t head;                           /* Mac: frames written into the ring */
    uint32_t state;                          /* Mac: 1 while the Sound Manager plays (an empty ring is then an underrun) */
    uint32_t volume;                         /* Mac: left << 16 | right, $0100 full (the Sound Manager's 'hvol' scale) */
    uint32_t sm_version;                     /* Mac: the Sound Manager's version (NumVersion), for the log */
    uint32_t opens, starts, feeds;           /* Mac: counters for the log */
    uint32_t pad1[9];
};

/* ---- the HDMI monitor, for PiDisplay (21 November 2026, docs/10 "PiDisplay") ----------------------------
   What PSMB_INFO_DISPINFO copies into the Mac's buffer: the monitor's EDID as the kernel read it over the
   VideoCore mailbox (tag 0x00030020, block by block) and parsed (psmac_edid.c), every mode it lists, each
   judged against what a Pi 3's HDMI can send, and the state of the HDMI settings on the card. Big-endian,
   every long at a multiple of 4, so m68k GCC's two-byte alignment and the kernel's lay it out alike. */
#define PSMB_DISPINFO_MAGIC  0x41363844u     /* 'A68D' */
#define PSMB_DISP_MAXMODES   48
struct psmb_dispmode {                       /* 32 bytes */
    uint16_t width, height;
    uint16_t refresh;                        /* centihertz: 6000 is 60.00 Hz */
    uint16_t flags;                          /* PSMB_DM_* */
    uint32_t pixclk;                         /* kHz, the mode's own timing */
    uint16_t alt_refresh;                    /* PSMB_DM_ALT: the reduced-blanking alternative's refresh (centihertz) ... */
    uint16_t source;                         /* PSMB_DS_* << 8 | the CEA VIC or the descriptor's number */
    uint32_t alt_pixclk;                     /* ... and its pixel clock, kHz */
    uint16_t aspect_w, aspect_h;             /* the shape, reduced: 16, 9 */
    uint16_t cfg_group, cfg_mode;            /* what config.txt gets for the mode itself: hdmi_group, hdmi_mode (87 = custom) */
    uint32_t reserved;
};
#define PSMB_DM_PREFERRED    0x0001u         /* the monitor's preferred mode (its first detailed timing) */
#define PSMB_DM_USABLE       0x0002u         /* a Pi 3 sends it at its own refresh */
#define PSMB_DM_TOOFAST      0x0004u         /* its pixel clock is beyond the Pi 3's HDMI (or the monitor's own limit) */
#define PSMB_DM_ALT          0x0008u         /* too fast, but CVT reduced blanking at alt_refresh fits */
#define PSMB_DM_SHAPE        0x0010u         /* not the panel's shape: the monitor stretches it */
#define PSMB_DM_INTERLACED   0x0020u
#define PSMB_DM_NATIVE       0x0040u         /* the panel's own size */
#define PSMB_DM_CURRENT      0x0080u         /* the mode in force now */
#define PSMB_DM_SUGGESTED    0x0100u         /* the one PiDisplay suggests (dispinfo.reason says why) */
#define PSMB_DM_MACSIZE      0x0200u         /* outside the sizes the kernel gives the Mac (PSMB_SCREEN_*) */
#define PSMB_DS_DETAILED     1u              /* source: a detailed timing descriptor */
#define PSMB_DS_ESTABLISHED  2u
#define PSMB_DS_STANDARD     3u
#define PSMB_DS_CEA          4u              /* a CEA-861 short video descriptor (low byte: the VIC) */
#define PSMB_DS_LIST         5u              /* no EDID: Setup and Recovery's list of standard modes (low byte: the DMT number) */
struct psmb_dispinfo {
    uint32_t magic, version, size, flags;    /* 'A68D', 1, the bytes of this struct, PSMB_DI_* */
    uint32_t edid_blocks, edid_status;       /* blocks read; PSMB_ES_* */
    char     name[16], serial_text[16];      /* the monitor's name and serial-number descriptors */
    char     maker[4], maker_name[20];       /* the PNP ID ("DEL") and, if the kernel knows it, the company */
    uint32_t product, serial;                /* product code, the binary serial number */
    uint16_t week, year;                     /* of manufacture (week 0: not given; 255: the model year) */
    uint16_t edid_version, input;            /* 0x0104; bit 0 digital input, bits 4-6 the EDID 1.4 bits per colour code */
    uint16_t size_w_mm, size_h_mm;           /* the picture's size */
    uint16_t min_vhz, max_vhz, min_hkhz, max_hkhz;   /* the range limits descriptor (0: none) */
    uint32_t max_pixclk;                     /* the monitor's own maximum, kHz (range limits; 0: not said) */
    uint32_t pi_pixclk;                      /* the Pi 3's limit used for the judgement, kHz */
    uint32_t audio_formats;                  /* bit n: CEA audio format code n (1 LPCM, 2 AC-3, ...) */
    uint32_t lpcm_channels, lpcm_rates, lpcm_bits;   /* rates bit 0 32 kHz ... 6 192 kHz; bits bit 0 16, 1 20, 2 24 */
    uint32_t speakers;                       /* the CEA speaker allocation byte (bit 0 front left and right ...) */
    uint32_t conn;                           /* PSMB_CN_*: the connection */
    uint32_t tmds_max;                       /* the HDMI vendor block's maximum TMDS clock, kHz (0: not said) */
    uint16_t cur_w, cur_h, cur_refresh, cur_source;  /* the HDMI mode in force (the firmware's frame buffer), its refresh
                                                        (centihertz, 0 unknown), where it came from: PSMB_SRC_* */
    uint16_t mac_w, mac_h, rotation, rotation_next;  /* the Mac's HDMI screen (W and H swapped at 90 and 270) */
    uint16_t vc_depth, board;                /* the VideoCore buffer's depth; 0 the IIci testbed, 1 solo, 2 SE */
    uint32_t cfg;                            /* PSMB_CF_* */
    uint32_t apply_state;                    /* PSMB_INFO_DISPSET: 0 idle, 1 being written, 2 written, 3 failed */
    uint32_t confirm_state;                  /* PSMB_CS_NONE ... */
    uint32_t confirm_secs;                   /* while asking: seconds left before the kernel reverts by itself */
    uint32_t restart_armed;                  /* PSMB_INFO_DISPRESTART asked */
    uint16_t confirm_w, confirm_h, confirm_refresh, nmodes;
    uint16_t suggested, frame_us;            /* index of the suggested mode (0xFFFF none); the longest frame on core 3 lately, us */
    char     reason[96];                     /* one line: why the suggested mode */
    char     cur_text[64];                   /* the HDMI lines in force, as config.txt has them */
    char     message[64];                    /* the last thing the card writer said */
    struct psmb_dispmode mode[PSMB_DISP_MAXMODES];
};
#define PSMB_DI_EDID         1u              /* an EDID was read */
#define PSMB_DI_SAMPLE       2u              /* ... the built-in sample or a file (mac.edidtest=), not a monitor */
#define PSMB_ES_OK           0u
#define PSMB_ES_NONE         1u              /* the firmware gave no EDID (no monitor, QEMU) */
#define PSMB_ES_HEADER       2u
#define PSMB_ES_CHECKSUM     3u              /* read, but a block's checksum is wrong (used anyway) */
#define PSMB_CN_HDMI         1u              /* conn: an HDMI vendor block (else DVI: no sound) */
#define PSMB_CN_BASICAUDIO   2u
#define PSMB_CN_YCC444       4u
#define PSMB_CN_YCC422       8u
#define PSMB_CN_DC30         16u             /* deep colour, 30 bits a pixel */
#define PSMB_CN_DC36         32u
#define PSMB_CN_DC48         64u
#define PSMB_CN_DIGITAL      128u
#define PSMB_CN_CEA          256u            /* a CEA-861 extension block */
#define PSMB_SRC_UNKNOWN     0u              /* cur_source: the firmware's own choice (no hdmi_mode anywhere) */
#define PSMB_SRC_CONFIG      1u              /* config.txt */
#define PSMB_SRC_FILE        2u              /* aurora68-display.txt, through config.txt's include */
#define PSMB_CF_INCLUDE      1u              /* config.txt has "include aurora68-display.txt" */
#define PSMB_CF_OVERRIDE     2u              /* ... but sets an hdmi_ mode after it, which wins */
#define PSMB_CF_WRITABLE     4u              /* the card can be written */
#define PSMB_CF_FILE         8u              /* aurora68-display.txt is on the card */
#define PSMB_CF_BAK          16u             /* aurora68-display.bak is on the card */
#define PSMB_CF_CONFIG       32u             /* config.txt was read */
#define PSMB_CS_NONE         0u              /* confirm_state: nothing to confirm */
#define PSMB_CS_PENDING      1u              /* a new mode, not yet confirmed: the Mac's PiScrn asks at startup */
#define PSMB_CS_ASKING       2u              /* the question is on the screen (confirm_secs counts down) */
#define PSMB_CS_KEPT         3u
#define PSMB_CS_REVERTING    4u              /* the old file back, the Pi restarts */

/* ---- the sound card's stream channel: PiPlay (21 November 2026, docs/09 "PiPlay") ----------------
   The card's second function, for whole sound files: the Mac writes a file's bytes as they are into a
   byte ring (MPEG audio, which is MP3, or the PCM samples of an AIFF or WAV file), and core 1 does the
   rest (psmac_audio.c, "source 4"): it takes the bytes into a buffer of its own of some megabytes, so
   that the Mac may fall behind for many seconds (a menu held down, another program in front, a slow
   SCSI disk) without a gap; it decodes MPEG audio with minimp3 (third_party/minimp3), converts PCM,
   resamples to the output's rate and mixes the result with the ASC and the card's ring above, so a beep
   still sounds over the music. A 68k could not decode an MP3 in time; some real sound cards of the
   period carried a DSP for such work, and this one has a Cortex-A53. The ring lies in the window's first
   megabyte (24-bit systems reach it at $971000), between the log's ring and slot $A's declaration ROM.
   head and tail count bytes for ever; the byte numbered n is at data_off + n % size.
   A new file: STOP (the ring is emptied: the Pi sets tail = head), wait for done_seq, fill in format and
   the PCM fields, write the first bytes, then START. More bytes may follow at any time; eof = 1 after the
   last. PAUSE and RESUME keep everything; STOP forgets everything. A command: cmd, then cmd_seq + 1; the
   Pi copies cmd_seq to done_seq when it has carried it out. The stream may be open-ended (data_bytes 0, eof
   never set: a radio stream); the decoder finds the next frame after a gap or damage, and a gap is counted as
   underruns, not an error. The block is 384 bytes, $11100-$1127F. */
#define PSMB_OFF_STREAM      0x11100         /* struct psmb_stream, in the page of struct psmb_sound */
#define PSMB_OFF_STRRING     0x71000         /* the byte ring: after the log's ring (0x61000-0x71000) */
#define PSMB_STR_SIZE        0xB000u         /* 44 KiB: up to slot $A's declaration ROM at 0x7C000 */
#define PSMB_STR_MAGIC       0x41363850u     /* 'A68P' */
#define PSMB_STR_MPEG        1u              /* format: MPEG audio, layers I-III; rate and channels from the frames */
#define PSMB_STR_PCM         2u              /* format: PCM, as pcm_rate, pcm_channels, pcm_bits, pcm_flags say */
#define PSMB_STR_FLAC        3u              /* format: FLAC (version 2: psmac_codec.h says what the Mac sends) */
#define PSMB_STR_OGG         4u              /* format: Ogg Vorbis (version 2) */
#define PSMB_STR_ALAC        5u              /* format: Apple Lossless, packets from an .m4a file (version 2) */
#define PSMB_STR_PCM_LE      1u              /* pcm_flags: little-endian words (WAV, AIFF-C 'sowt'); else big-endian (AIFF) */
#define PSMB_STR_PCM_UNSIGNED 2u             /* pcm_flags: 8-bit samples are unsigned (WAV); else signed (AIFF) */
#define PSMB_STR_C_START     1u              /* cmd: a new stream from what the ring holds */
#define PSMB_STR_C_PAUSE     2u
#define PSMB_STR_C_RESUME    3u
#define PSMB_STR_C_STOP      4u              /* silence at once; the ring and the Pi's buffer emptied */
#define PSMB_STR_S_IDLE      0u              /* state */
#define PSMB_STR_S_PLAYING   1u
#define PSMB_STR_S_PAUSED    2u
#define PSMB_STR_S_ENDED     3u              /* the last sample has been played */
#define PSMB_STR_S_ERROR     4u              /* error says why */
#define PSMB_STR_E_FORMAT    1u              /* a format or PCM layout the Pi does not play */
#define PSMB_STR_E_NOAUDIO   2u              /* no MPEG audio frame in the first 256 KB */
#define PSMB_STR_E_NOMEM     3u              /* no memory for the Pi's buffer */
#define PSMB_STR_E_DECODE    5u              /* FLAC, Ogg or ALAC data the decoder could not read (version 2) */
/* version 2 (22 November 2026, docs/08 item 105): FLAC, Ogg Vorbis and ALAC (formats 3-5), start_ms (where the
   bytes start in the file's time: a seek), and pos_ms counts from start_ms. A version-1 card answers formats
   3-5 with PSMB_STR_E_FORMAT, so the Mac checks the version first and says the card's software is too old. */
#define PSMB_STR_VERSION     2u
struct psmb_stream {
    uint32_t magic, version;                 /* Pi: 'A68P', PSMB_STR_VERSION */
    uint32_t size, data_off;                 /* Pi: PSMB_STR_SIZE, PSMB_OFF_STRRING */
    uint32_t out_kind;                       /* Pi: PSMB_SND_OUT_*; NONE: nothing plays (mac.audio=off, no output found) */
    uint32_t out_rate;                       /* Pi: the output's rate in Hz */
    uint32_t pibuf_size;                     /* Pi: the size of its own buffer, bytes */
    uint32_t done_seq;                       /* Pi: the last cmd_seq carried out */
    uint32_t tail;                           /* Pi: bytes taken from the ring */
    uint32_t state, error;                   /* Pi: PSMB_STR_S_*, PSMB_STR_E_* */
    uint32_t rate, channels;                 /* Pi: the stream's, once known (MPEG: from its first frame) */
    uint32_t bitrate;                        /* Pi: kbit/s, the average of the MPEG frames so far */
    uint32_t layer;                          /* Pi: MPEG layer 1-3, 0 for PCM */
    uint32_t pos_ms, length_ms;              /* Pi: what has been played; the length, 0 while not known */
    uint32_t buffered;                       /* Pi: bytes in its own buffer, not yet decoded */
    uint32_t mpeg_frames;                    /* Pi: MPEG frames decoded */
    uint32_t skipped;                        /* Pi: bytes that were no audio (tags, damage) */
    uint32_t underruns;                      /* Pi: output frames the stream did not have while it played */
    uint32_t load;                           /* Pi: core 1's time in the decoder, per mille, over the last second */
    uint32_t pad0[10];
    uint32_t cmd_seq, cmd;                   /* Mac (from +0x80): cmd, then cmd_seq + 1 */
    uint32_t head;                           /* Mac: bytes written into the ring */
    uint32_t eof;                            /* Mac: 1 once the last byte is in the ring */
    uint32_t format;                         /* Mac: PSMB_STR_*, read at START */
    uint32_t pcm_rate, pcm_channels, pcm_bits, pcm_flags;   /* Mac: the PCM's layout, read at START */
    uint32_t data_bytes;                     /* Mac: the stream's length in bytes (0: not known), for length_ms */
    uint32_t volume;                         /* Mac: left << 16 | right, $0100 full */
    uint32_t title_seq;                      /* Mac: +1 after writing title */
    char     title[128];                     /* Mac: what is playing, Mac Roman, NUL-terminated: the ID3 title, the
                                                file's name, or later an internet radio station's "now playing" */
    uint32_t start_ms;                       /* Mac (version 2): the file's time at the first byte sent, read at START */
    uint32_t pad1[19];
};

/* ---- the Aurora68 Agent: files and programs for the remote control (21 November 2026, docs/13 "Files and applications") ----
   The remote control (psmac_remote.c) reads the Mac's screen and memory by itself, but a file or a program needs the
   Mac's own File Manager and Process Manager, which only code on the 68k may call, and only at application time:
   a synchronous File Manager call from an interrupt can deadlock the File Manager's queue. The Aurora68 Agent
   (mac/piagent) is an INIT that hooks the GetNextEvent filter (jGNEFilter) and carries out each request there,
   between two events of whichever program calls GetNextEvent or WaitNextEvent: application time under System 6
   and 7 alike, and the moment the WiFi menu already uses (mac/wifimenu). Its VBL task counts vbl_beat (so the
   kernel can tell an agent that is installed from one that is not) and, under System 7, wakes the program in
   front with WakeUpProcess when a request waits, so that a program asleep in WaitNextEvent comes round at once
   (PSMB_AGX_WAKE).
   One request at a time, the kernel the client and the agent the server: the kernel fills in the request (op,
   path, offsets, the bytes to write in the buffer), then req_seq + 1; the agent answers in the reply fields and
   the buffer, then done_seq = req_seq. The agent opens and closes a file within each request: it runs in other
   programs' process contexts, and System 7 closes a quitting program's files, so it keeps nothing open between
   requests and needs no state; the kernel keeps it all (offsets, the MacBinary header, the HTTP connection).
   Paths are Mac Roman full paths, "Volume:Folder:File"; a leading ':' is the startup volume; "" (LIST) the
   volumes. The block is 2 KiB at $5A000, after the HDMI screen's display block; the buffer 24 KiB at $52000,
   after the input ring and up to the display block. Both are in the first megabyte, which a 24-bit System 6
   reaches ($95A000, $952000); the Agent reads and writes files straight into the buffer. */
#define PSMB_OFF_AGENT       0x5A000         /* struct psmb_agent */
#define PSMB_OFF_AGENTBUF    0x52000         /* the transfer buffer */
#define PSMB_AGENT_BUF       0x6000u         /* 24 KiB: what the window has free in one piece in its first megabyte */
#define PSMB_AGENT_KMAGIC    0x41363846u     /* 'A68F': the kernel serves the block */
#define PSMB_AGENT_MAGIC     0x41363841u     /* 'A68A': the agent is installed */
#define PSMB_AG_PING         1u              /* nothing: the agent answers */
#define PSMB_AG_LIST         2u              /* path, index (1 = the first entry): struct psmb_agent_entry records into the
                                                buffer; result = how many, more = entries after them; path "" lists the volumes */
#define PSMB_AG_STAT         3u              /* path: one struct psmb_agent_entry into the buffer */
#define PSMB_AG_READ         4u              /* path, fork, offset, length (at most the buffer): result = bytes read (fewer at the end) */
#define PSMB_AG_CREATE       5u              /* path, ftype, fcreator, fdflags; PSMB_AGF_FORCE: an existing file is emptied
                                                (both forks); PSMB_AGF_MKDIR: missing folders on the way are made */
#define PSMB_AG_WRITE        6u              /* path, fork, offset, length: the bytes from the buffer; result = bytes written */
#define PSMB_AG_SETINFO      7u              /* path, ftype, fcreator, fdflags, crdate, mddate (0: keep): the Finder information
                                                and dates once the forks are written; the volume is flushed */
#define PSMB_AG_MKDIR        8u              /* path; PSMB_AGF_MKDIR: the missing folders above it too; result = its directory ID */
#define PSMB_AG_DELETE       9u              /* path: a file, or a folder that is empty; never more */
#define PSMB_AG_RENAME       10u             /* path to path2, on one volume: a new name, another folder, or both (an existing
                                                folder as path2: into it, keeping the name) */
#define PSMB_AG_OPEN         11u             /* path: an application is launched (or brought to the front), a document opened
                                                with its creator's application (System 7: an 'odoc' Apple event, sent to it when
                                                it runs, its launch parameters when it does not). result 1 launched, 2 it ran
                                                already; note says which in words */
#define PSMB_AG_APPS         12u             /* index (0 = the first): struct psmb_agent_proc records into the buffer; result,
                                                more as for LIST. System 6: the current application only */
#define PSMB_AG_QUIT         13u             /* path: an application's name, or fcreator its signature: a 'quit' Apple event
                                                (System 7 only) */
#define PSMB_AG_FRONT        14u             /* path: an application's name, or fcreator its signature: SetFrontProcess (System 7);
                                                note = the name of the application that was in front, for putting it back
                                                (the remote's /screenshot?front=1) */
#define PSMB_AGF_FORCE       1u              /* flags */
#define PSMB_AGF_MKDIR       2u
#define PSMB_AGF_FRONT       4u              /* OPEN: bring an application that runs already to the front */
#define PSMB_AGX_PROCMGR     1u              /* features: the Process Manager (System 7) */
#define PSMB_AGX_AEVENTS     2u              /* the Apple Event Manager */
#define PSMB_AGX_MULTIFINDER 4u              /* System 6 with MultiFinder */
#define PSMB_AGX_WAKE        8u              /* the VBL task wakes the program in front (WakeUpProcess) */
/* status: 0, a Mac OSErr (fnfErr -43, dupFNErr -48, fBsyErr -47, dirNFErr -120, nsvErr -35, dskFulErr -34 ...), or one of
   the agent's own */
#define PSMB_AGE_NOTEMPTY    (-30001)        /* DELETE of a folder that holds something */
#define PSMB_AGE_ISDIR       (-30002)        /* a file was asked for and path is a folder */
#define PSMB_AGE_NOTDIR      (-30003)        /* a folder was asked for and path is a file */
#define PSMB_AGE_UNSUPPORTED (-30004)        /* not on this system (QUIT under System 6, a document under System 6) */
#define PSMB_AGE_XVOLUME     (-30005)        /* RENAME to another volume */
#define PSMB_AGE_BADPATH     (-30006)        /* empty, too long, a name longer than 31, or a part that is empty */
#define PSMB_AGE_NOAPP       (-30007)        /* OPEN: no application for the document's creator */
#define PSMB_AGE_NOPROC      (-30008)        /* QUIT: no such application running */
#define PSMB_AGE_BADOP       (-30009)
#define PSMB_AGE_BUSY        (-30010)        /* not yet: the Process Manager has not started (System 7's boot) */
struct psmb_agent {
    /* the kernel's, +$000 */
    uint32_t kmagic, kversion;               /* 'A68F', 1 */
    uint32_t buf_off, buf_size;              /* PSMB_OFF_AGENTBUF, PSMB_AGENT_BUF */
    uint32_t req_seq;                        /* +1 for a request, once the fields below are written */
    uint32_t op, flags;                      /* PSMB_AG_*, PSMB_AGF_* */
    uint32_t fork;                           /* 0 the data fork, 1 the resource fork */
    uint32_t offset, length;                 /* READ, WRITE: bytes of the fork */
    uint32_t index;                          /* LIST, APPS: where to start */
    uint32_t ftype, fcreator, fdflags;       /* CREATE, SETINFO; QUIT: fcreator */
    uint32_t crdate, mddate;                 /* SETINFO: seconds since 1904, local time; 0 keeps */
    uint32_t kpad[16];
    char     path[256];                      /* +$080: Mac Roman, NUL-terminated */
    char     path2[256];                     /* +$180: RENAME's new path */
    uint32_t kpad2[64];
    /* the agent's, +$380 */
    uint32_t magic, version;                 /* 'A68A', 2 (1 before 23 November 2026): written once the agent is installed */
    uint32_t beat;                           /* +1 at each look at the block (every GetNextEvent while one is waiting, else once a tick) */
    uint32_t vbl_beat;                       /* +1 at each VBL (60 Hz): the agent is installed and interrupts run */
    uint32_t sysversion;                     /* SysVersion ($15A): 0x0710 for 7.1 */
    uint32_t features;                       /* PSMB_AGX_* */
    uint32_t done_seq;                       /* = req_seq once the answer is complete */
    int32_t  status;                         /* 0 or an error (above) */
    uint32_t result, more;                   /* op-specific (above) */
    uint32_t served;                         /* requests answered since the agent started */
    uint32_t wakes;                          /* WakeUpProcess calls of the VBL task */
    uint32_t repeats;                        /* a request looked at again while it was being carried out (a launch's) */
    uint32_t taken_seq;                      /* = req_seq once the agent has begun the request (23 November 2026: the kernel
                                                tells the time a request waited for the Mac from the time it took; 0 from an
                                                older agent) */
    uint32_t chained;                        /* requests served straight after the previous one in the same GetNextEvent */
    uint32_t apad[1];
    char     note[64];                       /* a few words more about an error, Mac Roman ("no application for 'ttxt'") */
    char     startup[32];                    /* the startup volume's name, Mac Roman */
    uint32_t apad2[64];
};
struct psmb_agent_entry {                    /* 80 bytes, LIST and STAT */
    uint32_t flags;                          /* PSMB_AGN_*, and the low byte ioFlAttrib */
    uint32_t type, creator;                  /* a file's; 0 for a folder */
    uint32_t dlen, rlen;                     /* the forks' lengths; a folder: dlen its entries; a volume: dlen KiB free, rlen KiB in all */
    uint32_t crdate, mddate, bkdate;         /* seconds since 1904, local time */
    uint32_t fdflags;                        /* the Finder flags */
    uint32_t location;                       /* v << 16 | h in its folder's window */
    uint32_t id, parid;                      /* the file's number or the folder's ID; its folder's ID; a volume: its vRefNum */
    char     name[32];                       /* Mac Roman, NUL-terminated (HFS names have 31 characters at most) */
};
#define PSMB_AGN_DIR         0x100u          /* a folder */
#define PSMB_AGN_VOLUME      0x200u          /* a volume (LIST of "") */
#define PSMB_AGN_LOCKED      0x400u          /* the file is locked, or the volume */
#define PSMB_AGN_OPEN        0x800u          /* the file is open (by someone) */
#define PSMB_AGN_STARTUP     0x1000u         /* the startup volume */
struct psmb_agent_proc {                     /* 72 bytes, APPS */
    uint32_t psn_hi, psn_lo;                 /* the ProcessSerialNumber */
    uint32_t type, creator;                  /* the file's type ('APPL', 'appe', 'FNDR') and signature */
    uint32_t mode;                           /* processMode (the SIZE resource's flags: $0100 onlyBackground ...) */
    uint32_t size, free;                     /* its partition and what is free in it, bytes */
    uint32_t launched, active;               /* Ticks at its launch; ticks it has had the processor */
    uint32_t flags;                          /* PSMB_AGP_* */
    char     name[32];                       /* Mac Roman */
};
#define PSMB_AGP_FRONT       1u              /* the application in front */
#define PSMB_AGP_AGENT       2u              /* the one the agent answered in (its GetNextEvent) */
#define PSMB_AGP_CURRENT     4u              /* System 6: CurApName, the only one known */

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
    uint32_t board;           /* offset 56: the logic board the Pi sits in, PSMB_BOARD_* (22 November 2026; 0 before) */
};
#define PSMB_BOARD_IICI      0  /* the testbed's IIci motherboard (MAME maciici_ps) */
#define PSMB_BOARD_SOLO      1  /* no Macintosh: mac.board=solo */
#define PSMB_BOARD_SE        2  /* the Macintosh SE: mac.board=se */
#define PSMB_BOARD_PLUS      3  /* the Macintosh Plus: mac.board=plus (Aurora68 Machine answers Gestalt 'mach' with gestaltMacPlus) */
#define PSMB_BOARD_128K      4  /* the Macintosh 128K: mac.board=128k (gestaltClassic) */
#define PSMB_BOARD_512K      5  /* the Macintosh 512K: mac.board=512k (gestaltClassic) */
#define PSMB_BOARD_512KE     6  /* the Macintosh 512Ke: mac.board=512ke (gestaltMac512KE) */
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
    uint32_t progress;         /* Pi: moves on while a command takes long (a USB disk's: psmac_usbdisk.c), so the
                                  driver waits for as long as the Pi works on it (since 20 November 2026) */
    uint32_t reserved[3];
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
/* PiDisplay (mac/pidisplay.c, docs/10 "PiDisplay", 21 November 2026): the HDMI monitor's EDID, the modes a Pi 3
   can send it, and the HDMI settings the kernel keeps in aurora68-display.txt on the card (psmac_hdmi.c) */
#define PSMB_INFO_DISPINFO   8      /* buf/len: struct psmb_dispinfo (below) copied into the Mac's buffer; result = bytes */
#define PSMB_INFO_DISPSET    9      /* arg0 = index into dispinfo's modes, arg1 = 1 for its reduced-blanking alternative,
                                       arg2 = rotation (0, 90, 180, 270): aurora68-display.txt rewritten (the old one kept as
                                       aurora68-display.bak), with a mark that the new mode waits to be confirmed; progress in
                                       dispinfo.apply_state. status PSMB_E_RO without a writable card, PSMB_E_RANGE for a bad index */
#define PSMB_INFO_DISPROTATE 10     /* arg0 = rotation: kept in aurora68-display.txt, in force from the Mac's next restart */
#define PSMB_INFO_DISPCONFIRM 11    /* arg0: PSMB_DC_* (the keep-or-revert question after a new mode) */
#define PSMB_INFO_DISPRESTART 12    /* the whole Pi restarts (its watchdog) at the 68k's next RESET, once the card is written:
                                       PiDisplay then restarts the Mac through the Shutdown Manager */
#define PSMB_INFO_SANE       13     /* arg0 = 4 or 5: the SE ROM's software SANE package, PACK 4 (FP68K) or PACK 5 (Elems68K),
                                       read from the SE's ROM or an image (psmac_sane.c, docs/08 item 101): result = its size;
                                       with buf/len, its bytes copied there (xfer); PSMB_E_NODATA when there is none or
                                       mac.sane=rom keeps the IIci ROM's FPU SANE */
#define PSMB_DC_REVERT       0      /* aurora68-display.bak back in place, then the Pi restarts */
#define PSMB_DC_KEEP         1      /* the confirm mark taken out of aurora68-display.txt */
#define PSMB_DC_ASKING       2      /* the question is on the screen now: the kernel reverts by itself if no answer follows */

/* BLOCK service: arg0 = device index, arg1 = first block, arg2 = block count, buf = data */
#define PSMB_BLK_READ        1
#define PSMB_BLK_WRITE       2
#define PSMB_BLK_FLUSH       3
#define PSMB_BLK_INFO        4      /* refresh struct psmb_blkdev table */
#define PSMB_BLK_EJECT       5      /* a removable unit (a USB disk): flushed and released; the unit is then not
                                       present until a disk is put in again */
#define PSMB_BLK_BOOT        6      /* PiDisk's boot record (mac/pidisk_boot.s), as the Start Manager looks for a startup
                                       disk: result 0 = the card's image first, else the driver reference number of the
                                       SCSI disk to try first (psmac_mbox.c, mac.boot=); status PSMB_E_BUSY while the
                                       card has not yet been looked at for images */

struct psmb_blkdev {           /* 64 bytes, PSMB_OFF_BLKDEV + idx*64 */
    uint32_t blocks;           /* capacity in 512-byte blocks, 0 = no device */
    uint32_t block_size;       /* 512 */
    uint32_t flags;            /* PSMB_BD_* */
    uint32_t media;            /* Pi: +1 each time a new disk is in the unit (a USB stick plugged in); the driver
                                  then tells the Mac a disk was inserted. Written after blocks and name. */
    char     name[48];         /* C string, Mac Roman */
};
#define PSMB_BD_PRESENT      (1u << 0)  /* a disk is in the unit (a removable one that is ejected or pulled out: clear) */
#define PSMB_BD_WRITABLE     (1u << 1)
#define PSMB_BD_REMOVABLE    (1u << 2)  /* the Mac sees an ejectable disk (a USB stick, psmac_usbdisk.c) */

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
