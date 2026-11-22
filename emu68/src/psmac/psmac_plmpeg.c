/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_plmpeg.c — where the video card's decoder, pl_mpeg, is compiled (docs/16-video-card.md).
 *
 * Backstory (22 November 2026, docs/08 item 98). The owner asked for YouTube on the Mac through a card
 * that exists only in the Pi. The helper on the owner's network turns any video into MPEG-1 with MP2
 * sound; the Mac's program fetches that over its own TCP/IP and copies the bytes into the card; the card
 * decodes them on core 1 with pl_mpeg (third_party/pl_mpeg: MIT, unmodified; README.md there says which
 * commit). This file is only where pl_mpeg's implementation is compiled, once, with what the kernel
 * needs; psmac_video.c calls it.
 *
 * The options, and why:
 *  - PLM_NO_STDIO: the kernel has no FILE; the card's bytes arrive through a load callback.
 *  - PLM_MALLOC, PLM_REALLOC, PLM_FREE: the kernel's TLSF heap, which has its own lock and is used from
 *    core 1 already (psmac_audio.c's stream buffer). pl_mpeg allocates when a stream starts (three
 *    pictures: 345 KB at 320 x 240, 2 MB at 1280 x 720) and grows its byte buffers by doubling when a
 *    packet needs more; nothing in the per-frame path allocates.
 *  - Floating point: pl_mpeg keeps times as doubles and its MP2 synthesis in floats, and it works out
 *    some tables with floats at start. Core 1 runs no JIT and owns its FP registers (psmac_mp3.c says
 *    the same for minimp3), and -ffixed-v19... are honoured here as everywhere.
 *  - Endianness: pl_mpeg reads its bit stream a byte at a time (plm_buffer_read) and keeps pictures as
 *    byte planes, so the big-endian kernel needs nothing special.
 *  - <math.h>: pl_mpeg needs none (checked: no sqrt, cos or pow in the file).
 */
#undef _FORTIFY_SOURCE                 /* the distribution compiler's default; the kernel has no __memcpy_chk */
#include <stdint.h>
#include <stddef.h>
#include "tlsf.h"                      /* not support.h: its __used__ collides with the C library's headers pl_mpeg includes */
extern void *tlsf;                     /* support.h: the kernel's heap */
#define PLM_NO_STDIO
#define PLM_MALLOC(sz)      tlsf_malloc(tlsf, (sz))
#define PLM_REALLOC(p, sz)  tlsf_realloc(tlsf, (p), (sz))
#define PLM_FREE(p)         tlsf_free(tlsf, (p))
#define PL_MPEG_IMPLEMENTATION
#include "third_party/pl_mpeg/pl_mpeg.h"
