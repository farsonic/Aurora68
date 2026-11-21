/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_mp3.c — the MPEG audio decoder of the sound card's stream channel (PiPlay, docs/09).
 *
 * Backstory (21 November 2026, docs/08 item 82). The user's SE plays the Mac's sound on an HDMI
 * monitor, and they wanted to play MP3 files. System 7.1 on a 68k has no MP3 player, and a 68000-class
 * Macintosh could not decode one in time anyway; some real sound cards carried a DSP for such work.
 * This card has a Cortex-A53: the Mac hands it the file's bytes (psmb.h, struct psmb_stream) and core 1
 * decodes them with minimp3 (third_party/minimp3: CC0, public domain, unmodified; README.md there says
 * which commit). This file is only where minimp3's implementation is compiled, once, with the options
 * the kernel needs; psmac_audio.c calls it.
 *
 * The options, and what was checked:
 *  - MINIMP3_NO_SIMD: minimp3 uses NEON intrinsics on every AArch64, written for little-endian, and
 *    the kernel is big-endian, where a vector's lanes sit in its register the other way round. Both
 *    builds were checked under qemu-aarch64_be against the same decoder on x86-64 and gave the same
 *    samples to within one LSB, so either would do; the scalar code, which reads the bit stream a byte
 *    at a time, is kept as the plainer of the two. A frame takes core 1 a millisecond or so, of the
 *    26 ms it lasts (docs/08 item 82); the heartbeat's stream line says how much on hardware.
 *  - Floating point: minimp3 works in single precision. The kernel keeps no floating point on core 0,
 *    whose FP and SIMD registers belong to the JIT's 68k (the -ffixed-v19... of CMakeLists.txt are
 *    reserved everywhere, this file included); core 1 runs no JIT, and nothing else on it touches
 *    those registers.
 *  - No C library: minimp3 includes <stdlib.h> and <string.h> for memcpy and memset, which the
 *    kernel has (support.c); it allocates nothing. Its scratch, about 16 KB, is on core 1's stack
 *    (64 KiB, start.c).
 */
#undef _FORTIFY_SOURCE                 /* the distribution compiler's default; the kernel has no __memcpy_chk */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "third_party/minimp3/minimp3.h"
