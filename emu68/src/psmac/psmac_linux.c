// SPDX-License-Identifier: MPL-2.0
//
// psmac_linux.c - Linux/m68k started directly, without Mac OS and without a booter on the Mac side.
// Copyright (c) 2026 Aurora68 project.
//
// mac.linux=NAME names an m68k Linux kernel, an ELF file (vmlinux) in the card's root, or a host file in the
// testbed (a name with a '/', read through semihosting like the other host files). The kernel puts its segments
// into the Mac's RAM where they are linked, writes the bootinfo a Macintosh booter (Penguin) would write right
// after the kernel's _end, switches a screen on as a Mac OS video driver would, and starts the 68k at the
// kernel's entry with translation off, instead of the IIci ROM's reset vector. Linux then finds a Macintosh IIci
// with a 68030 and a 68882 and drives its chips itself, which the board layer serves as it does for A/UX.
// mac.linuxargs= is Linux's command line (quotes keep its spaces together), mac.linuxinitrd=NAME an initramfs,
// mac.linuxscreen=se|hdmi the screen Linux gets. mac.mmu=full is required: Linux needs the 68030's MMU.
// docs/27-linux.md has the story.

#include <stdint.h>
#include "support.h"
#include "devicetree.h"
#include "psmac.h"
#include "psmac_cmdline.h"

// The bootinfo (arch/m68k/include/uapi/asm/bootinfo.h and bootinfo-mac.h in Linux's tree)
#define BI_LAST 0x0000u
#define BI_MACHTYPE 0x0001u
#define BI_CPUTYPE 0x0002u
#define BI_FPUTYPE 0x0003u
#define BI_MMUTYPE 0x0004u
#define BI_MEMCHUNK 0x0005u
#define BI_RAMDISK 0x0006u
#define BI_COMMAND_LINE 0x0007u
#define BI_MAC_MODEL 0x8000u
#define BI_MAC_VADDR 0x8001u
#define BI_MAC_VDEPTH 0x8002u
#define BI_MAC_VROW 0x8003u
#define BI_MAC_VDIM 0x8004u
#define BI_MAC_VLOGICAL 0x8005u
#define BI_MAC_SCCBASE 0x8006u
#define BI_MAC_BTIME 0x8007u
#define BI_MAC_GMTBIAS 0x8008u
#define BI_MAC_MEMSIZE 0x8009u
#define BI_MAC_CPUID 0x800au
#define BI_MAC_ROMBASE 0x800bu
#define MACH_MAC 3u
#define CPU_68030 2u
#define FPU_68882 2u
#define MMU_68030 2u
#define MAC_MODEL_IICI 11u
#define BOOTINFOV_MAGIC 0x4249561au
#define MAC_BOOTI_MAJOR 2u

#define IICI_SCC 0x50f04000u                    // the IIci's SCC, which head.S and the serial driver use
#define IICI_VIA1 0x50f00000u
#define ELF_PT_LOAD 1u
#define ELF_SHT_SYMTAB 2u
#define KERNEL_PAGE 0x1000u
#define CMDLINE_MAX 512u
#define DEFAULT_ARGS "root=/dev/sda rw console=tty0"

extern int psmac_mmu_full;
extern int psmac_board_se;
extern int psmac_board_solo;
extern int psmac_board_plus;
const char *psmac_option(const char *key);
int64_t psmac_sh_open(const char *name, uint64_t mode);
int psmac_sh_read(int64_t fd, void *buf, uint64_t len);
int64_t psmac_sh_flen(int64_t fd);
int psmac_sh_available(void);
int32_t psmac_card_read_file(const char *name, uint8_t *buf, uint32_t max);
uint8_t *psmac_mac_ram_ptr(void);
int psmac_display_linux(int want_hdmi, uint32_t *w, uint32_t *h, uint32_t *depth, uint32_t *rowbytes, uint32_t *base);

struct LinuxBoot {
    int state;                                  // 0 not asked for, 1 the files are in RAM, 2 placed, -1 refused
    uint32_t ram_size;
    uint32_t elf_off, elf_len;                  // the ELF file, read into the Mac's RAM above where it goes
    uint32_t rd_off, rd_len;                    // the initramfs, at the top of the Mac's RAM
    uint32_t entry, end, bi_len;
    char args[CMDLINE_MAX];
};
static struct LinuxBoot lx;

static uint32_t be32_at(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16_at(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static int has_slash(const char *s)
{
    for (; *s; s++) {
        if (*s == '/') {
            return 1;
        }
    }
    return 0;
}

int psmac_linux_wanted(void)
{
    return lx.state > 0;
}

// A file into the Mac's RAM at off, at most max bytes: its length, or -1. A name with a '/' is a host file
// (the testbed's semihosting), any other a file in the card's root, read on core 0 before core 3 has the card.
static int32_t read_file(const char *name, uint32_t off, uint32_t max)
{
    uint8_t *ram = psmac_mac_ram_ptr();
    if (has_slash(name)) {
        if (!psmac_sh_available()) {
            return -1;
        }
        int64_t fd = psmac_sh_open(name, 1);
        int64_t len = fd >= 0 ? psmac_sh_flen(fd) : -1;
        if (len <= 0 || len > (int64_t)max || !psmac_sh_read(fd, ram + off, (uint64_t)len)) {
            return -1;
        }
        return (int32_t)len;
    }
    return psmac_card_read_file(name, ram + off, max);
}

// core 0, in psmac_setup_memory before the mailbox and core 3: the option, the checks, the files into RAM
void psmac_linux_read(void)
{
    const char *o = psmac_option("mac.linux");
    if (!o || !o[0]) {
        return;
    }
    char name[128];
    uint32_t n = 0;
    for (; o[n] && n + 1 < sizeof name; n++) {
        name[n] = o[n];
    }
    name[n] = 0;
    if (!psmac_mmu_full) {
        kprintf("[LINUX] mac.linux=%s needs mac.mmu=full (Linux needs the 68030's MMU, docs/23): the IIci ROM starts as usual\n",
                name);
        lx.state = -1;
        return;
    }
    if (!psmac_board_se || psmac_board_plus) {
        kprintf("[LINUX] mac.linux=%s: Linux starts on the SE board and the solo board only (mac.board=se or solo): "
                "the IIci ROM starts as usual\n", name);
        lx.state = -1;
        return;
    }
    lx.ram_size = psmac_ram_size();
    // the ELF in the upper half of the Mac's RAM, below the quarter kept for an initramfs: its segments go low
    uint32_t q = lx.ram_size / 4;
    lx.elf_off = q * 2;
    int32_t got = read_file(name, lx.elf_off, q);
    if (got < 64) {
        kprintf("[LINUX] mac.linux=%s: the file could not be read (%s, at most %u KiB): the IIci ROM starts as usual\n", name,
                has_slash(name) ? "a host file" : "the card's root", q >> 10);
        lx.state = -1;
        return;
    }
    lx.elf_len = (uint32_t)got;
    const uint8_t *e = psmac_mac_ram_ptr() + lx.elf_off;
    if (be32_at(e) != 0x7f454c46u || e[4] != 1 || e[5] != 2 || be16_at(e + 18) != 4) {
        kprintf("[LINUX] mac.linux=%s: not a 32-bit big-endian m68k ELF file (a vmlinux, not a zImage or vmlinuz): "
                "the IIci ROM starts as usual\n", name);
        lx.state = -1;
        return;
    }
    kprintf("[LINUX] %s read: %u KiB of ELF, at $%08x in the Mac's RAM for now\n", name, lx.elf_len >> 10, lx.elf_off);
    const char *r = psmac_option("mac.linuxinitrd");
    if (r && r[0]) {
        got = read_file(r, q * 3, q);
        if (got <= 0) {
            kprintf("[LINUX] mac.linuxinitrd=%s could not be read: Linux starts without an initramfs\n", r);
        } else {
            lx.rd_len = (uint32_t)got;
            lx.rd_off = (lx.ram_size - lx.rd_len) & ~(KERNEL_PAGE - 1);
            memmove(psmac_mac_ram_ptr() + lx.rd_off, psmac_mac_ram_ptr() + q * 3, lx.rd_len);
            kprintf("[LINUX] initramfs %s: %u KiB at $%08x\n", r, lx.rd_len >> 10, lx.rd_off);
        }
    }
    // Linux's command line: mac.linuxargs= from cmdline.txt, its quotes taken off
    lx.args[0] = 0;
    of_node_t *c = dt_find_node("/chosen");
    of_property_t *p = c ? dt_find_property(c, "bootargs") : 0;
    if (p && p->op_value) {
        const char *line = (const char *)p->op_value;
        if (pcl_get(line, (uint32_t)strlen(line), "mac.linuxargs", lx.args, sizeof lx.args) != 1) {
            lx.args[0] = 0;
        }
    }
    if (!lx.args[0]) {
        const char *d = DEFAULT_ARGS;
        uint32_t k = 0;
        for (; d[k]; k++) {
            lx.args[k] = d[k];
        }
        lx.args[k] = 0;
    }
    lx.state = 1;
}

// the symbol _end, where head.S looks for the bootinfo (get_bi_record: lea _end), or 0
static uint32_t elf_end_symbol(const uint8_t *e, uint32_t len)
{
    uint32_t shoff = be32_at(e + 32);
    uint32_t shentsize = be16_at(e + 46);
    uint32_t shnum = be16_at(e + 48);
    if (!shoff || shentsize < 40 || shoff + shnum * shentsize > len) {
        return 0;
    }
    for (uint32_t i = 0; i < shnum; i++) {
        const uint8_t *sh = e + shoff + i * shentsize;
        if (be32_at(sh + 4) != ELF_SHT_SYMTAB) {
            continue;
        }
        uint32_t off = be32_at(sh + 16), size = be32_at(sh + 20), link = be32_at(sh + 24);
        if (link >= shnum || off + size > len) {
            return 0;
        }
        const uint8_t *str_sh = e + shoff + link * shentsize;
        uint32_t str_off = be32_at(str_sh + 16), str_size = be32_at(str_sh + 20);
        if (str_off + str_size > len) {
            return 0;
        }
        for (uint32_t s = 0; s + 16 <= size; s += 16) {
            uint32_t nm = be32_at(e + off + s);
            if (nm + 5 > str_size) {
                continue;
            }
            const char *t = (const char *)e + str_off + nm;
            if (t[0] == '_' && t[1] == 'e' && t[2] == 'n' && t[3] == 'd' && t[4] == 0) {
                return be32_at(e + off + s + 4);
            }
        }
    }
    return 0;
}

struct BiWriter {
    uint8_t *at;
    uint32_t used, cap;
};
static void bi_put(struct BiWriter *b, uint32_t tag, const void *data, uint32_t len)
{
    uint32_t size = (4 + len + 3) & ~3u;
    if (b->used + size + 4 > b->cap) {
        return;
    }
    uint8_t *r = b->at + b->used;
    r[0] = (uint8_t)(tag >> 8);
    r[1] = (uint8_t)tag;
    r[2] = (uint8_t)(size >> 8);
    r[3] = (uint8_t)size;
    const uint8_t *d = data;
    for (uint32_t i = 0; i < size - 4; i++) {
        r[4 + i] = i < len ? d[i] : 0;
    }
    b->used += size;
}
static void bi_put32(struct BiWriter *b, uint32_t tag, uint32_t v)
{
    uint8_t d[4];
    put32(d, v);
    bi_put(b, tag, d, 4);
}

// The version the kernel says it speaks to a Mac booter: the table at _stext (struct bootversion: a branch, the
// magic, then machine and version pairs). 0 when it has none.
static uint32_t mac_bootinfo_version(const uint8_t *ram, uint32_t stext)
{
    const uint8_t *v = ram + stext;
    if (be32_at(v + 2) != BOOTINFOV_MAGIC) {
        return 0;
    }
    for (uint32_t i = 6; i < 6 + 8 * 16; i += 8) {
        uint32_t mach = be32_at(v + i);
        if (!mach) {
            break;
        }
        if (mach == MACH_MAC) {
            return be32_at(v + i + 4);
        }
    }
    return 0;
}

// core 0, at the end of psmac_setup_memory, once the screens are known: the segments where they are linked, the
// screen on, the bootinfo after _end, and the reset vectors at 0 the kernel's (SP and the entry), so that
// M68K_StartEmu starts the 68k in Linux
void psmac_linux_place(void)
{
    if (lx.state != 1) {
        return;
    }
    uint8_t *ram = psmac_mac_ram_ptr();
    const uint8_t *e = ram + lx.elf_off;
    uint32_t phoff = be32_at(e + 28), phentsize = be16_at(e + 42), phnum = be16_at(e + 44);
    if (phentsize < 32 || phoff + phnum * phentsize > lx.elf_len) {
        kprintf("[LINUX] the ELF file's program headers do not fit in it: the IIci ROM starts as usual\n");
        lx.state = -1;
        return;
    }
    uint32_t lo = 0xffffffffu, hi = 0, segs = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = e + phoff + i * phentsize;
        if (be32_at(ph) != ELF_PT_LOAD) {
            continue;
        }
        uint32_t off = be32_at(ph + 4), pa = be32_at(ph + 12), fsz = be32_at(ph + 16), msz = be32_at(ph + 20);
        if (msz < fsz || off + fsz > lx.elf_len || pa + msz > lx.elf_off) {
            kprintf("[LINUX] segment %u ($%08x, %u bytes) does not fit below the file at $%08x: the IIci ROM starts as usual\n",
                    i, pa, msz, lx.elf_off);
            lx.state = -1;
            return;
        }
        if (pa < lo) {
            lo = pa;
        }
        if (pa + msz > hi) {
            hi = pa + msz;
        }
        segs++;
    }
    if (!segs) {
        kprintf("[LINUX] the ELF file has no loadable segment: the IIci ROM starts as usual\n");
        lx.state = -1;
        return;
    }
    lx.entry = be32_at(e + 24);
    lx.end = elf_end_symbol(e, lx.elf_len);
    if (!lx.end) {
        lx.end = hi;                            // a stripped vmlinux: the linker script puts _end after the bss
    }
    if (lx.end < hi || lx.end + 2 * KERNEL_PAGE > lx.elf_off) {
        kprintf("[LINUX] _end $%08x is below the last segment's end $%08x or too high: the IIci ROM starts as usual\n",
                lx.end, hi);
        lx.state = -1;
        return;
    }
    // the segments, in ascending order of file offset; the file is above them all, so none overwrites another's bytes
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = e + phoff + i * phentsize;
        if (be32_at(ph) != ELF_PT_LOAD) {
            continue;
        }
        uint32_t off = be32_at(ph + 4), pa = be32_at(ph + 12), fsz = be32_at(ph + 16), msz = be32_at(ph + 20);
        memmove(ram + pa, e + off, fsz);
        memset(ram + pa + fsz, 0, msz - fsz);
        kprintf("[LINUX]   segment %u: $%08x-$%08x (%u bytes from the file, %u of bss)\n", i, pa, pa + msz, fsz, msz - fsz);
    }
    // the version table is at _stext, a page below the entry (_start, head.S); the first segment starts at 0 with
    // the ELF header in the page before _stext
    uint32_t stext = lx.entry >= KERNEL_PAGE ? lx.entry - KERNEL_PAGE : 0;
    uint32_t biv = mac_bootinfo_version(ram, stext);
    if (!biv) {
        kprintf("[LINUX] no bootinfo version table at $%08x: not a Linux/m68k kernel? started all the same\n", stext);
    } else if ((biv >> 16) != MAC_BOOTI_MAJOR) {
        kprintf("[LINUX] the kernel speaks Macintosh bootinfo %u.%u, this booter writes %u.0: started all the same\n",
                biv >> 16, biv & 0xffff, MAC_BOOTI_MAJOR);
    }
    // the screen
    const char *sc = psmac_option("mac.linuxscreen");
    int want_hdmi = sc && sc[0] == 'h';
    uint32_t w = 0, h = 0, depth = 0, rb = 0, base = 0;
    int screen = psmac_display_linux(want_hdmi, &w, &h, &depth, &rb, &base);
    if (!screen && want_hdmi) {
        kprintf("[LINUX] mac.linuxscreen=hdmi: there is no HDMI screen of the card's here (on the SE board it needs "
                "mac.hdmi=screen): the SE's screen instead\n");
        screen = psmac_display_linux(0, &w, &h, &depth, &rb, &base);
    }
    // the bootinfo, right after _end
    struct BiWriter b = { ram + lx.end, 0, lx.elf_off - lx.end - KERNEL_PAGE };
    if (b.cap > 64 * 1024) {
        b.cap = 64 * 1024;
    }
    uint8_t chunk[8];
    bi_put32(&b, BI_MACHTYPE, MACH_MAC);
    bi_put32(&b, BI_CPUTYPE, CPU_68030);
    bi_put32(&b, BI_FPUTYPE, FPU_68882);
    bi_put32(&b, BI_MMUTYPE, MMU_68030);
    put32(chunk, 0);
    put32(chunk + 4, lx.ram_size);
    bi_put(&b, BI_MEMCHUNK, chunk, 8);
    if (lx.rd_len) {
        put32(chunk, lx.rd_off);
        put32(chunk + 4, lx.rd_len);
        bi_put(&b, BI_RAMDISK, chunk, 8);
    }
    bi_put(&b, BI_COMMAND_LINE, lx.args, (uint32_t)strlen(lx.args) + 1);
    bi_put32(&b, BI_MAC_MODEL, MAC_MODEL_IICI);
    if (screen) {
        bi_put32(&b, BI_MAC_VADDR, base);
        bi_put32(&b, BI_MAC_VDEPTH, depth);
        bi_put32(&b, BI_MAC_VROW, rb);
        bi_put32(&b, BI_MAC_VDIM, h << 16 | w);
        bi_put32(&b, BI_MAC_VLOGICAL, base);    // the Mac's logical address of the screen: physical, translation is off
    }
    bi_put32(&b, BI_MAC_SCCBASE, IICI_SCC);
    bi_put32(&b, BI_MAC_BTIME, 0);              // Linux reads the clock chip itself (arch/m68k/mac/misc.c); Penguin's
    bi_put32(&b, BI_MAC_GMTBIAS, 0);            // two values are only printed
    bi_put32(&b, BI_MAC_MEMSIZE, lx.ram_size >> 20);
    bi_put32(&b, BI_MAC_CPUID, MAC_MODEL_IICI << 2 | 2);   // only read when the model is missing: (cpuid >> 2) & 63
    bi_put32(&b, BI_MAC_ROMBASE, PSMAC_ROM_BASE);
    uint8_t last[4] = { 0, 0, 0, 0 };
    if (b.used + 4 <= b.cap) {
        for (int i = 0; i < 4; i++) {
            b.at[b.used + i] = last[i];
        }
        b.used += 4;
    }
    lx.bi_len = b.used;
    // the reset vectors: an initial stack below the kernel (head.S sets its own at once) and the entry
    put32(ram, stext);
    put32(ram + 4, lx.entry);
    arm_flush_cache((uintptr_t)ram, lx.end + lx.bi_len);
    if (lx.rd_len) {
        arm_flush_cache((uintptr_t)(ram + lx.rd_off), lx.rd_len);
    }
    char shown[CMDLINE_MAX + 16];
    psmac_redact(lx.args, shown, sizeof shown);
    kprintf("[LINUX] kernel $%08x-$%08x, entry $%08x, bootinfo %u bytes at _end $%08x (Macintosh bootinfo %u.%u), "
            "memory $00000000-$%08x\n", lo, hi, lx.entry, lx.bi_len, lx.end, biv >> 16, biv & 0xffff, lx.ram_size);
    if (screen) {
        kprintf("[LINUX] screen: %ux%u at %u bit%s, %u bytes a row, at $%08x (%s)\n", w, h, depth, depth == 1 ? "" : "s", rb,
                base, psmac_board_solo ? "the card's screen" : (want_hdmi && base != 0xf9980000u) ? "the HDMI screen" :
                "the SE's screen");
    } else {
        kprintf("[LINUX] no screen for Linux: its console is the serial port only\n");
    }
    kprintf("[LINUX] command line: %s\n", shown);
    lx.state = 2;
}

// core 0, as the 68k is released (start.c, after the board is ready): what the IIci ROM would have left in the
// chips that Linux takes as it finds them. Linux sets up both VIAs' interrupts and timers, port B's clock chip
// and ADB lines, the RBV and the SCC itself (arch/m68k/mac/via.c, drivers/macintosh/via-macii.c); port A's data
// direction it never writes, and on the SE that port selects the screen and the sound buffer: the ROM's outputs
// (DDRA $2F, from what reached the SE's wire under the ROM, docs/08 item 93) with PA3 high, the main sound buffer.
// Returns 1 when the 68k starts in Linux.
int psmac_linux_release(void)
{
    if (lx.state != 2) {
        return 0;
    }
    if (!psmac_board_solo) {
        psmac_io_write(IICI_VIA1 + 3 * 0x200, 1, 0x2f, 0);
        psmac_io_write(IICI_VIA1 + 15 * 0x200, 1, 0x08, 0);
    }
    psmac_boot_mark("68k released: Linux's first instruction ($%08x)", lx.entry);
    return 1;
}

// ---- the serial ports in the log ------------------------------------------------------------------------------
// What Linux sends to the SCC (head.S's early console on both channels, the serial console on channel A), a line
// at a time, as "[SCC A] text": in the testbed there is nothing at the ports, and on the SE it shows in
// aurora68.log what the serial cable would. mac.scclog=on|off; on by default with mac.linux=.
static char scc_line[2][132];
static uint32_t scc_len[2];
static int scc_log = -1;
void psmac_linux_scc(int channel_a, uint8_t c)
{
    if (scc_log < 0) {
        const char *o = psmac_option("mac.scclog");
        scc_log = o ? (o[0] == 'o' && o[1] == 'n') : lx.state > 0;
    }
    if (!scc_log) {
        return;
    }
    int ch = channel_a ? 1 : 0;
    if (c == '\r') {
        return;
    }
    if (c == '\t') {
        c = ' ';
    }
    if (c != '\n' && scc_len[ch] < sizeof scc_line[ch] - 1) {
        scc_line[ch][scc_len[ch]++] = (char)(c >= 32 && c < 127 ? c : '.');
        if (scc_len[ch] < sizeof scc_line[ch] - 1) {
            return;
        }
    }
    scc_line[ch][scc_len[ch]] = 0;
    kprintf("[SCC %c] %s\n", ch ? 'A' : 'B', scc_line[ch]);
    scc_len[ch] = 0;
}
