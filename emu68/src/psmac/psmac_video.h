/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_video.h — the video card's block and ring, and the board-neutral core's interface
 * (docs/16-video-card.md, GitHub issue #23).
 *
 * Backstory (22 November 2026, docs/08 item 98). The owner wants YouTube on the Mac: a helper on their
 * network converts any video into an MPEG program stream (MPEG-1 video, MP2 sound); the Mac's program,
 * Aurora68 Video, fetches it over the Mac's own TCP/IP and copies the bytes into this card; the card
 * decodes them on a spare Pi core and draws each picture into the Mac's own frame buffer, inside the
 * rectangle the program gives, at whatever depth that screen is in, and sends the sound to the sound
 * card's mixer. The card does no networking and has nothing to do with the remote control.
 *
 * The first half of this file is the card's window as the Mac (or, for Alice68, the Amiga) sees it: a
 * block of 4 KiB at the start, the stream's byte ring at A68V_OFF_RING. Every word is big-endian, which
 * is both the 68k's order and this kernel's. Nothing in it is NuBus: the Macintosh's card (slot $C, its
 * declaration ROM and its place in the address space) is psmac_videocard.c; an Amiga card or library
 * would put the same window somewhere of its own.
 *
 * How the Mac uses it, in the way the sound card's stream channel works (psmb.h struct psmb_stream):
 *  1. Find the card (Slot Manager: the sResource of type {$7050 'pP', 3, 1, 'AV'}), check magic.
 *  2. Write the first bytes of the stream into the ring, add them to head; set screen, the rectangle,
 *     visible, then geom_seq + 1.
 *  3. cmd = A68V_C_START, then cmd_seq + 1; done_seq follows when the card has taken it.
 *  4. Keep the ring full: there is room while head - tail < ring_size. A full ring is how the card
 *     holds the Mac back, and through TCP the helper.
 *  5. At the stream's end, eof = 1. The state goes to ENDED once the last picture and sound are out.
 *  6. Pause and resume are commands. A seek is STOP (the ring is emptied: tail becomes head), then the
 *     new stream's bytes, then START.
 *  7. The window moves, resizes, is covered or uncovered: the new rectangle and visible, geom_seq + 1.
 * The card reads head only once it has stood for 50 us (the 68k's stores come from the JIT on core 0
 * with no barrier between the bytes and the head; psmac_audio.c, STR_HEAD_LAG).
 */
#ifndef PSMAC_VIDEO_H
#define PSMAC_VIDEO_H
#include <stdint.h>

#define A68V_MAGIC        0x41363856u        /* 'A68V' */
#define A68V_VERSION      1u
#define A68V_WIN_SIZE     0x00100000u        /* 1 MiB: the block, and the ring in the top half */
#define A68V_OFF_BLOCK    0x00000u
#define A68V_OFF_RING     0x80000u
#define A68V_RING_SIZE    0x80000u           /* 512 KiB, a power of two: 7 s of 320 x 240, 3.5 s of 512 x 342 */

#define A68V_C_START      1u                 /* a new stream from what the ring holds */
#define A68V_C_PAUSE      2u
#define A68V_C_RESUME     3u
#define A68V_C_STOP       4u                 /* at once: the ring emptied (tail = head), the decoder gone */

#define A68V_S_IDLE       0u
#define A68V_S_BUFFERING  1u                 /* waiting for enough bytes to start, or after running dry */
#define A68V_S_PLAYING    2u
#define A68V_S_PAUSED     3u
#define A68V_S_ENDED      4u                 /* the last picture shown and the last sound played */
#define A68V_S_ERROR      5u                 /* error and message say why */

#define A68V_E_NOVIDEO    1u                 /* no MPEG-1 video in the first A68V_PROBE bytes */
#define A68V_E_NOMEM      2u                 /* no memory for the decoder */
#define A68V_E_SIZE       3u                 /* a picture larger than A68V_MAX_W x A68V_MAX_H */
#define A68V_PROBE        (512u << 10)
#define A68V_MAX_W        1920u
#define A68V_MAX_H        1088u

#define A68V_SCREEN_FIRST 0u                 /* the Mac's first screen: the SE's own on the SE board */
#define A68V_SCREEN_HDMI  1u                 /* the HDMI screen (slot $A) on the SE board */

struct a68v_block {
    /* the card's half: 0x00-0x7F */
    uint32_t magic, version;                 /* A68V_MAGIC, A68V_VERSION, written last */
    uint32_t win_size, ring_off, ring_size;  /* A68V_WIN_SIZE, A68V_OFF_RING, A68V_RING_SIZE */
    uint32_t done_seq;                       /* the last cmd_seq carried out */
    uint32_t tail;                           /* bytes taken from the ring */
    uint32_t state, error;                   /* A68V_S_*, A68V_E_* */
    uint32_t video_w, video_h;               /* the stream's picture, once known */
    uint32_t fps_x100;                       /* its frame rate x 100 (2500) */
    uint32_t audio_rate, audio_channels;     /* its sound, 0 if it has none */
    uint32_t pos_ms;                         /* the stream's time of the picture on the screen */
    uint32_t frames_decoded, frames_shown, frames_dropped;   /* dropped: decoded too late to be worth drawing */
    uint32_t decode_us, decode_us_max;       /* a picture's decoding, average and longest over the last second */
    uint32_t draw_us;                        /* a picture's drawing, average over the last second */
    uint32_t load;                           /* the core's time in the card, per mille over the last second */
    uint32_t buffered;                       /* bytes in the card's own buffers, not yet decoded */
    uint32_t screens;                        /* how many screens it can draw on */
    uint32_t underruns;                      /* times it ran dry while playing */
    uint32_t sound;                          /* 1: its sound plays (an output exists), 0: pictures only, by the Pi's clock */
    uint32_t pad0[6];
    /* the Mac's half: 0x80-0xFF */
    uint32_t cmd_seq, cmd;                   /* cmd, then cmd_seq + 1 */
    uint32_t head;                           /* bytes written into the ring */
    uint32_t eof;                            /* 1 once the last byte is in */
    uint32_t screen;                         /* A68V_SCREEN_*, read with the geometry */
    int32_t  top, left, bottom, right;       /* the picture's rectangle in that screen's pixels, QuickDraw's order */
    uint32_t visible;                        /* 0: draw nothing (the window is covered, being dragged, hidden) */
    uint32_t geom_seq;                       /* + 1 after changing screen, the rectangle or visible */
    uint32_t volume;                         /* left << 16 | right, $0100 full */
    uint32_t pad1[20];
    /* the card again: 0x100 */
    char     message[128];                   /* the last error, or what it is doing, in words (ASCII) */
};
_Static_assert(sizeof(struct a68v_block) == 0x180, "a68v_block");
_Static_assert(__builtin_offsetof(struct a68v_block, cmd_seq) == 0x80, "a68v_block Mac half");

/* ---- the frame decoder (psmac_vframes.c, 22 November 2026, GitHub issue #13, docs/08 item 110) ----------
   The second half of the card, beside the stream: one compressed picture at a time, as QuickTime's
   decompressor components hand them over, decoded on the Pi and drawn into a pixel map of the Mac's (a
   screen, or a GWorld in its memory) at that map's depth. The Mac's side is the card's driver ".A68Video"
   (its Control calls, docs/video-decoder-api.md), which copies a request into a free slot, adds one to its
   seq and waits for done to follow; the QuickTime components and any other program call the driver.
   A session holds what a format keeps from picture to picture (Cinepak's codebooks, the last picture for
   the blocks a picture leaves out), so a movie opens one, decodes its pictures in order, and closes it. */
#define A68V_OFF_FRAMES   0x01000u           /* struct a68v_fhdr */
#define A68V_FMAGIC       0x41363846u        /* 'A68F' */
#define A68V_FVERSION     1u
#define A68V_OFF_FSLOTS   0x01100u           /* A68V_FSLOTS x struct a68v_freq */
#define A68V_FSLOTS       8u
#define A68V_MAX_SESSIONS 16u                /* movies open at once */
#define A68V_MAX_DATA     (1u << 20)         /* the largest compressed picture taken (a 640 x 480 Animation key frame is ~900 KB) */

#define A68V_OP_OPEN      1u                 /* codec, width, height, depth, desc: session is set */
#define A68V_OP_DECODE    2u                 /* session, src, src_len, and where to draw it (unless A68V_FF_NODRAW) */
#define A68V_OP_CLOSE     3u                 /* session */
#define A68V_OP_DRAW      4u                 /* session: its last picture drawn again where the request says */
#define A68V_OP_SOUND     5u                 /* an 'MP2 ' session: src, src_len of MP2 in; PCM out at dst_base, room dst_rowbytes
                                                (answers: width the PCM bytes written, height 1 if more waits, depth the rate,
                                                dst_w the channels; 23 November 2026, the QuickTime import's sound track) */

#define A68V_FF_NODRAW    1u                 /* decode only: the picture is kept, nothing is drawn (a frame QuickTime skips) */
#define A68V_FF_NODITHER  2u                 /* indexed destinations: the nearest colour, no ordered dither */

#define A68V_FE_OK        0u
#define A68V_FE_CODEC     1u                 /* not a format the card decodes */
#define A68V_FE_DEPTH     2u                 /* not at that depth (the image's or the destination's) */
#define A68V_FE_SIZE      3u                 /* larger than the card decodes (VC_MAX_W x VC_MAX_H) */
#define A68V_FE_MEM       4u                 /* no memory for the session */
#define A68V_FE_SESSION   5u                 /* no such session, or none free */
#define A68V_FE_DATA      6u                 /* the picture's data is damaged (what could be decoded was) */
#define A68V_FE_ADDR      7u                 /* an address the card cannot reach (memory not there, or paged out) */
#define A68V_FE_OFF       8u                 /* frame decoding is switched off (mac.vdecode=off, or the driver's switch) */
#define A68V_FE_OP        9u                 /* no such operation */
#define A68V_CODEC_MP2    0x4D503220u        /* 'MP2 ': OPEN makes a sound session (op 5), not a picture one */

struct a68v_fhdr {                           /* the card's, but for enabled */
    uint32_t magic, version;                 /* A68V_FMAGIC, A68V_FVERSION, written last */
    uint32_t slots, slot_off, slot_size;     /* A68V_FSLOTS, A68V_OFF_FSLOTS, sizeof(struct a68v_freq) */
    uint32_t enabled;                        /* 1: in use; 0: requests answered A68V_FE_OFF (the driver's switch) */
    uint32_t sessions;                       /* open now */
    uint32_t frames, drawn, errors;          /* pictures decoded, drawn, refused, since the start */
    uint32_t decode_us, draw_us;             /* the last second's averages */
    uint32_t max_w, max_h;                   /* the largest picture */
    uint32_t pad0[2];
    uint32_t codecs[16];                     /* the formats decoded, as FourCCs, 0-terminated */
    uint32_t pad1[32];
};
struct a68v_freq {                           /* 128 bytes: the Mac fills, adds one to seq; the card answers, sets done = seq */
    uint32_t seq, done;
    uint32_t op, session;
    uint32_t codec, width, height, depth;    /* OPEN: the ImageDescription's cType, size and depth */
    uint32_t src, src_len;                   /* DECODE: the compressed picture (68k logical address, bytes) */
    uint32_t desc, desc_len;                 /* OPEN: the ImageDescription itself, for its colour table */
    uint32_t dst_base, dst_rowbytes, dst_depth;   /* the pixel map: its baseAddr, rowBytes (flags masked off), pixelSize */
    int32_t  dst_x, dst_y, dst_w, dst_h;     /* where the picture goes, in the map's pixels from baseAddr, and its size there */
    int32_t  clip_left, clip_top, clip_right, clip_bottom;   /* what may be written, the same coordinates */
    uint32_t ctab, ctab_seed;                /* depths 1-8: the map's ColorTable (dereferenced; 0: the standard one) and its seed */
    uint32_t flags;                          /* A68V_FF_* */
    uint32_t result;                         /* A68V_FE_* */
    uint32_t decode_us, draw_us;             /* this request's */
    uint32_t frame;                          /* the Mac's frame number, for the log */
    uint32_t pad[2];
};
_Static_assert(sizeof(struct a68v_fhdr) == 256, "a68v_fhdr");
_Static_assert(sizeof(struct a68v_freq) == 128, "a68v_freq");
_Static_assert(__builtin_offsetof(struct a68v_freq, ctab) == 92, "a68v_freq ctab");
void psmac_vframes_init(uint8_t *win);
int  psmac_vframes_poll(void);
void psmac_vframes_reset(void);
void psmac_vframes_report(void);

/* ---- the core (psmac_video.c), on whichever core the host gives it ---------------------------- */
void psmac_video_init(uint8_t *win, uint32_t screens);  /* the window, in the kernel's view; how many screens */
int  psmac_video_poll(void);                            /* one slice of work; 1 if it did something */
void psmac_video_reset(void);                           /* the computer was reset: stop (from any core) */
void psmac_video_report(void);                          /* the heartbeat's line */

/* ---- what the host provides ------------------------------------------------------------------- */
/* A screen to draw on, as the computer's own frame buffer: base in the kernel's view, the size and row
   pitch in bytes, the depth (1, 2, 4, 8 indexed; 16 as 0RRRRRGGGGGBBBBB; 32 as 00RRGGBB, all
   big-endian), and for the indexed depths its colour table (0x00RRGGBB) and a number that changes when
   the table does. 0 if there is no such screen or it is not on. */
struct pv_surface {
    uint8_t *base;
    uint32_t width, height, rowbytes, depth;
    const volatile uint32_t *clut;
    uint32_t clut_seq;
    int gray;                                /* the screen is set to greys: the table's colours shown as their luminance */
};
int psmac_video_surface(uint32_t screen, struct pv_surface *s);
/* the sound: 44.1 kHz or another rate, 16-bit stereo, mixed with the computer's own (psmac_audio.c) */
int      psmac_audio_video_open(uint32_t rate);         /* 1 if an output plays it; 0: no sound here */
uint32_t psmac_audio_video_room(void);                  /* frames that fit now */
uint32_t psmac_audio_video_queued(void);                /* frames given and not yet played */
void     psmac_audio_video_push(const int16_t *lr, uint32_t frames);
uint64_t psmac_audio_video_played(void);                /* frames played since open */
void     psmac_audio_video_pause(int on);
void     psmac_audio_video_volume(uint32_t lr);         /* left << 16 | right, $0100 full */
void     psmac_audio_video_close(void);
/* a test producer (psmac_videocard.c, mac.videotest=): fills the ring as the Mac would; may be empty */
void psmac_video_test_poll(volatile struct a68v_block *b, uint8_t *ring);

#endif
