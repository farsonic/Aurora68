/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_fatfs.h - one reader for FAT12/16/32 and exFAT, for the SD card's exchange folder and for USB
 * sticks (psmac_card.c on core 3, psmac_usbdisk.c on core 1, tools/hfsview_test.c on the host).
 * Written 21 November 2026 for issue #20; psmac_fatfs.c tells the story.
 *
 * Nothing here knows where the sectors come from: the caller gives a read function for 512-byte
 * sectors of the whole device, and a context for it. Nothing is allocated: struct fx_vol and
 * struct fx_dir hold their own buffers (64-byte aligned, so a USB read can go straight into them).
 */
#ifndef PSMAC_FATFS_H
#define PSMAC_FATFS_H
#include <stdint.h>

/* 0 when the n sectors from lba are in buf; buf is 64-byte aligned and holds n * 512 bytes */
typedef int (*fx_read_fn)(void *ctx, uint32_t lba, uint32_t n, uint8_t *buf);

enum { FX_NONE, FX_FAT12, FX_FAT16, FX_FAT32, FX_EXFAT };
#define FX_ATTR_RO     0x01u
#define FX_ATTR_HIDDEN 0x02u
#define FX_ATTR_SYSTEM 0x04u
#define FX_ATTR_DIR    0x10u
#define FX_EOC         0xffffffffu          /* fx_next: the end of a chain */
#define FX_NAME_MAX    255                  /* UTF-16 units, FAT's and exFAT's limit */
#define FX_FATWIN      8u                   /* FAT sectors kept: a chain mostly stays in one window */

struct fx_vol {
    uint8_t fat[FX_FATWIN * 512u] __attribute__((aligned(64)));   /* a window of the first FAT */
    uint8_t sec[512] __attribute__((aligned(64)));                /* the boot sector and odd reads */
    fx_read_fn read; void *ctx;
    int type;
    uint32_t part, total;                   /* the volume's first device sector, its length in sectors */
    uint32_t spc;                           /* 512-byte sectors per cluster */
    uint32_t fat_lba, fat_secs;             /* the first FAT and its length, device sectors */
    uint32_t data_lba;                      /* cluster 2 */
    uint32_t root_lba, root_secs;           /* FAT12/16: the fixed root directory */
    uint32_t root_clus;                     /* FAT32, exFAT */
    uint32_t clusters;                      /* clusters 2 .. clusters + 1 exist */
    uint32_t fat_at;                        /* the first sector in fat[], or ~0 */
    uint32_t reads, read_secs, errors;      /* counters (the HFS view's build budget, the log) */
    uint16_t label[16]; int label_len;      /* the volume's name, UTF-16, 0 if it has none */
};

/* one entry of a directory: a file or a folder */
struct fx_dirent {
    uint16_t name[FX_NAME_MAX + 1]; int nlen;
    uint8_t attr;
    uint8_t contig;                         /* exFAT NoFatChain: the clusters follow each other, no FAT chain */
    uint32_t clus;
    uint64_t size, valid;                   /* exFAT: bytes from valid on read as zeros */
    uint32_t ctime, mtime;                  /* DOS form, date << 16 | time, the medium's local time */
};

struct fx_dir {
    uint8_t buf[FX_FATWIN * 512u] __attribute__((aligned(64)));
    struct fx_vol *v;
    uint32_t clus;                          /* the cluster being read (0: FAT12/16's fixed root) */
    uint32_t sec;                           /* the next sector within it (or within the fixed root) */
    uint32_t left;                          /* a contiguous exFAT directory: sectors still to read */
    uint32_t nbuf, pos;                     /* entries in buf, the next one */
    int contig, end, err, root;
    /* a name being put together across entries (FAT long-name pieces, an exFAT entry set) */
    uint16_t lfn[FX_NAME_MAX + 1 + 13]; int lfn_n, lfn_sum, lfn_ok;
    uint8_t set[19 * 32]; int set_n, set_want;
};

/* where on a device the first FAT or exFAT volume starts: a volume at block 0, an MBR's partition,
   a GPT's Microsoft basic data partition (EFI system partitions are passed over: a stick formatted on a
   modern Mac carries one before its data). 1 found (*lba, *type), 0 none. what: a few words for the log. */
int fx_probe(fx_read_fn read, void *ctx, uint32_t dev_blocks, struct fx_vol *scratch, uint32_t *lba, int *type, const char **what);
int fx_mount(struct fx_vol *v, fx_read_fn read, void *ctx, uint32_t lba);    /* 0, or < 0 */
const char *fx_type_name(int type);
uint32_t fx_next(struct fx_vol *v, uint32_t c);                              /* FX_EOC, 0 for a bad entry */
uint32_t fx_clus_lba(const struct fx_vol *v, uint32_t c);
/* a directory: the root (d == 0) or a folder's entry */
void fx_dir_open(struct fx_dir *it, struct fx_vol *v, const struct fx_dirent *d);
int fx_dir_next(struct fx_dir *it, struct fx_dirent *e);                     /* 1 an entry, 0 the end, < 0 an error */
/* the clusters of a file (its first cluster, size, exFAT NoFatChain), as runs: calls f(arg, first cluster, count) for each, in order; the runs'
   total in *nclus. 0, or < 0 (a broken chain: cut there) */
int fx_runs(struct fx_vol *v, uint32_t clus, uint64_t size, int contig, int (*f)(void *arg, uint32_t c, uint32_t n), void *arg, uint32_t *nclus);
#endif
