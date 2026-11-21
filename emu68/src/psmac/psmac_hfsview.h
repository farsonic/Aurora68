/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_hfsview.h - a read-only HFS volume made from a FAT or exFAT file system (psmac_fatfs.c): a
 * folder of the SD card, or a whole USB stick, as a disk the Mac's own HFS reads. Written 21 November
 * 2026 for issue #20; psmac_hfsview.c tells the story.
 *
 * Use: hv_init, hv_set_root (a folder) or not (the whole volume), then hv_step until it says done (each
 * step reads a bounded number of sectors), then hv_read for the Mac's blocks. All memory comes from the
 * caller's alloc function and goes back through hv_release.
 */
#ifndef PSMAC_HFSVIEW_H
#define PSMAC_HFSVIEW_H
#include <stdint.h>
#include "psmac_fatfs.h"

#define HV_MAX_NODES 32000u                 /* files and folders in one view (memory: about 130 bytes each) */

struct hv_node;
struct hv_fork { uint32_t start, blks, node; uint32_t which; };     /* allocation blocks of one fork */
struct hv_run { uint32_t clus, n; };

struct hv {
    struct fx_vol *fx;
    void *(*alloc)(uint32_t bytes);         /* 64-byte aligned memory, or 0 */
    void (*release)(void *p);
    void (*log)(const char *fmt, ...);
    const char *tag;                        /* the log lines' start: "[CARD] Mac Exchange" */
    /* what was found */
    struct hv_node *n; uint32_t nn, ncap;
    struct hv_run *runs; uint32_t nruns, rcap;
    uint16_t *names; uint32_t nnames, namecap;
    /* the build */
    int phase, failed;
    uint32_t scan_i, work_i;
    struct fx_dir *dir; struct fx_dirent *ent; int dir_open;
    uint32_t root_clus, root_size; int root_contig, root_volume;
    uint16_t root_name[64]; int root_nlen;  /* a folder as the volume: its name is the volume's */
    uint8_t adbuf[1024];                    /* an AppleDouble file's header */
    int (*skip)(void *arg, const struct fx_dirent *e); void *skip_arg;   /* the root's files that are not to show */
    uint32_t n_files, n_dirs, n_ad, n_hidden, n_full, n_big, n_broken, n_dropped, n_renamed, n_short, n_unmapped;
    uint64_t bytes_dropped;
    /* the volume */
    char vname[28];                         /* Mac Roman, a Pascal string */
    char vname_c[28];                       /* the same for the log: ASCII, C string */
    uint32_t ablk, spa, nablks, used_ablks, alblst, bmsecs, total;
    uint32_t xt_start, xt_blks, ct_start, ct_blks;
    struct hv_fork *forks; uint32_t nforks;
    uint32_t *recs; uint32_t nrecs;         /* the catalog's leaf records in key order: node << 1 | thread */
    uint32_t *leaf_first; uint32_t nleaves; /* each leaf node's first record */
    uint8_t *fixed; uint32_t nfixed;        /* the header, map and index nodes, rendered once */
    uint32_t cat_nodes, cat_used, map_nodes, leaf0, depth, root_node, cdate, mdate;
    uint32_t files_root, dirs_root, files_all, dirs_all;
    uint8_t *scratch;                       /* HV_SCRATCH bytes for reads that do not start on a sector */
    uint8_t mdb[512];
    uint32_t mem;                           /* bytes held after the build */
};
#define HV_SCRATCH (16u * 512u)

void hv_init(struct hv *h, struct fx_vol *fx, void *(*alloc)(uint32_t), void (*release)(void *), void (*log)(const char *, ...), const char *tag);
void hv_set_root(struct hv *h, const struct fx_dirent *folder);
/* one step of the build: reads at most a few dozen sectors. 0 still going, 1 done (hv_read works),
   < 0 failed (the log says why) */
int hv_step(struct hv *h);
/* n blocks of 512 bytes of the volume from block lba: 0, or -1 (the medium did not give them) */
int hv_read(struct hv *h, uint32_t lba, uint32_t n, uint8_t *out);
void hv_release(struct hv *h);
void hv_report(struct hv *h, uint32_t ms);  /* the log lines for a view that is ready */
#endif
