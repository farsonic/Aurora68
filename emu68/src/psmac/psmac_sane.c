/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_sane.c — exact SANE for the Mac: the SE ROM's software packages in place of the IIci ROM's
 * FPU ones (docs/08 item 101).
 *
 * Backstory (22 November 2026). Photoshop never finished starting on the owner's SE: the JIT ran at a
 * steady 72 MIPS translating nothing, the PC moving between Photoshop's code and the IIci ROM's SANE
 * (PACK 4, FP68K, at $40870ACC), the loop counters in D4, D5 and D7 standing still for minutes. The
 * kernel printed the loop (the heartbeat's "code at" line): a refinement step, then _FP68K FOSUB in
 * extended precision (new minus old), then a comparison with a tolerance. The IIci's SANE is the
 * version for a 68881/68882 and does its arithmetic with FPU instructions; Emu68's FPU keeps the 96-bit
 * extended format as a 64-bit double (Load96bit in M68k_LINEF.c), eleven bits short, so a loop that
 * waits for an extended-precision result to settle can wait for ever.
 *
 * The price: the software packages are 20 to 60 times slower at SANE arithmetic than the FPU ones in
 * the testbed (Speedometer's Whetstones 8.5 times a Mac Classic against 228), so mac.sane=rom stays.
 *
 * The SE's own ROM has the other SANE: PACK 4 (FP68K) and PACK 5 (Elems68K) for a 68000 with no FPU,
 * which do extended arithmetic exactly in integer instructions. They are Apple's, and they are in the
 * owner's machine already: on the SE board this file reads them from the SE's ROM chips over the wire
 * at the Mac's first request (about 9 KB, some 5,000 bus reads). In the testbeds and on a Pi alone,
 * mac.serom=PATH names an SE ROM image (through semihosting in QEMU, else a file in the card's root).
 * Nothing of them is in the repository. A68 Machine (mac/pimach.s) asks for them at startup with
 * PSMB_INFO_SANE, copies them into system-heap handles and puts those in AppPacks, where the ROM's
 * package dispatcher ($40826206 for _Pack4) looks. mac.sane=rom leaves the IIci ROM's SANE in place.
 *
 * The packages are found the way tools/romres.py reads an SE ROM's resource map: the long at ROM+$1A
 * points 4 bytes before the map header, whose type list holds 'PACK'; each reference gives the data's
 * ROM offset, behind an 8-byte block header whose low 24 bits are the size. A package's data starts
 * with a branch, a zero word, 'PACK' and its number, which is checked.
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"
#include "psmb.h"

extern int psmac_board_se, psmac_board_solo;
extern const char *psmac_option(const char *key);
extern uint32_t psmac_wire_read(uint32_t addr, int size);
extern int64_t psmac_sh_open(const char *name, uint64_t mode);
extern int     psmac_sh_read(int64_t fd, void *buf, uint64_t len);
extern int64_t psmac_sh_flen(int64_t fd);
extern int     psmac_sh_available(void);
extern int32_t psmac_card_read_file(const char *name, uint8_t *buf, uint32_t max);

#define SE_ROM_BASE   0x400000u
#define SE_ROM_SIZE   0x40000u          /* 256 KiB: both SE ROMs */
#define PACK_MAX      0x4000u           /* 16 KiB: the SE's are 4,544 and 4,198 bytes */

static uint8_t rom_file[SE_ROM_SIZE];   /* an SE ROM image (mac.serom=), when given */
static uint32_t rom_file_len;
static int from_wire;
static uint8_t pack[2][PACK_MAX];       /* PACK 4 and PACK 5 */
static uint32_t pack_len[2];
static int state;                       /* 0 not tried, 1 found, -1 not available */

static uint32_t rom8(uint32_t off)
{
    if (from_wire) return psmac_wire_read(SE_ROM_BASE + off, 1) & 0xff;
    return off < rom_file_len ? rom_file[off] : 0xff;
}
static uint32_t rom16(uint32_t off)
{
    if (from_wire && !(off & 1)) return psmac_wire_read(SE_ROM_BASE + off, 2) & 0xffff;
    return rom8(off) << 8 | rom8(off + 1);
}
static uint32_t rom32(uint32_t off) { return rom16(off) << 16 | rom16(off + 2); }

/* the package's bytes into pack[k]: 0, or why not (for the log) */
static const char *fetch(uint32_t doff, uint32_t size, int id, int k)
{
    if (size < 64 || size > PACK_MAX) return "an implausible size";
    if (doff < 8 || doff + size > SE_ROM_SIZE) return "data outside the ROM";
    for (uint32_t i = 0; i < size; i += 2) {
        uint32_t w = rom16(doff + i);
        pack[k][i] = (uint8_t)(w >> 8);
        if (i + 1 < size) pack[k][i + 1] = (uint8_t)w;
    }
    if (pack[k][4] != 'P' || pack[k][5] != 'A' || pack[k][6] != 'C' || pack[k][7] != 'K' ||
        (pack[k][8] << 8 | pack[k][9]) != id)
        return "no 'PACK' header in it";
    pack_len[k] = size;
    return 0;
}

static const char *find_packs(void)
{
    uint32_t p = rom32(0x1A);
    if (p < 0x100 || p + 0x40 > SE_ROM_SIZE) return "no resource map pointer at ROM+$1A";
    uint32_t m = p + 4, tl = m + rom16(m + 0x18);
    if (tl + 2 > SE_ROM_SIZE) return "the type list is outside the ROM";
    uint32_t ntypes = rom16(tl) + 1;
    if (ntypes > 64) return "an implausible type list";
    for (uint32_t t = 0; t < ntypes; t++) {
        uint32_t o = tl + 2 + t * 8;
        if (rom32(o) != 0x5041434Bu) continue;                       /* 'PACK' */
        uint32_t n = rom16(o + 4) + 1, rlo = rom16(o + 6);
        if (n > 32) return "an implausible PACK list";
        for (uint32_t r = 0; r < n; r++) {
            uint32_t e = tl + rlo + r * 12;
            int id = (int)rom16(e);
            if (id != 4 && id != 5) continue;
            uint32_t doff = rom8(e + 5) << 16 | rom16(e + 6);
            uint32_t size = rom8(doff - 7) << 16 | rom16(doff - 6);
            const char *why = fetch(doff, size, id, id - 4);
            if (why) return why;
        }
        return pack_len[0] && pack_len[1] ? 0 : "PACK 4 or 5 missing";
    }
    return "no PACK resources in the map";
}

static void load(void)
{
    const char *opt = psmac_option("mac.sane"), *path = psmac_option("mac.serom"), *src, *why;
    state = -1;
    if (opt && opt[0] == 'r') { kprintf("[PSMAC] SANE: the IIci ROM's (mac.sane=rom)\n"); return; }
    if (path) {
        int32_t got = -1;
        if (psmac_sh_available()) {
            int64_t fd = psmac_sh_open(path, 1);                     /* rb */
            int64_t len = fd >= 0 ? psmac_sh_flen(fd) : -1;
            if (len > 0 && len <= (int64_t)SE_ROM_SIZE && psmac_sh_read(fd, rom_file, (uint64_t)len)) got = (int32_t)len;
        }
        if (got < 0) got = psmac_card_read_file(path, rom_file, SE_ROM_SIZE);
        if (got < 0) { kprintf("[PSMAC] SANE: mac.serom=%s could not be read: the IIci ROM's SANE stays\n", path); return; }
        rom_file_len = (uint32_t)got; from_wire = 0; src = path;
    } else if (psmac_board_se && !psmac_board_solo) {
        from_wire = 1; src = "the SE's own ROM";
    } else {
        kprintf("[PSMAC] SANE: no SE ROM (mac.serom=PATH names an image): the IIci ROM's SANE stays\n");
        return;
    }
    uint32_t sum = rom32(0);
    if ((why = find_packs())) {
        kprintf("[PSMAC] SANE: %s (checksum %08x): %s; the IIci ROM's SANE stays\n", src, sum, why);
        return;
    }
    state = 1;
    kprintf("[PSMAC] SANE: from %s (checksum %08x): PACK 4 (FP68K) %u bytes, PACK 5 (Elems68K) %u bytes, for A68 Machine to install\n",
            src, sum, pack_len[0], pack_len[1]);
}

/* PSMB_INFO_SANE: package n (4 or 5): 0 and its bytes, or a PSMB_E_* status */
int psmac_sane_get(uint32_t n, const uint8_t **p, uint32_t *len)
{
    if (n != 4 && n != 5) return PSMB_E_RANGE;
    if (!state) load();
    if (state < 0) return PSMB_E_NODATA;
    *p = pack[n - 4]; *len = pack_len[n - 4];
    return 0;
}
