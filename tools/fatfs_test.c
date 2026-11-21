/* SPDX-License-Identifier: MPL-2.0
 *
 * fatfs_test.c - the kernel's FAT12/16/32 and exFAT reader (emu68/src/psmac/psmac_fatfs.c) on the host:
 * every file of an image, found by walking its folders, read through its clusters (FAT chains and exFAT's
 * NoFatChain runs) and printed as "path<TAB>size<TAB>FNV-1a 64 of its bytes", names in UTF-8. The caller
 * (tools/fatfs_test.sh) compares that with the tree the image was made from. Written 21 November 2026
 * (issue #20).
 */
#define _POSIX_C_SOURCE 200112L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psmac_fatfs.h"

static FILE *img;
static uint32_t img_blocks;
static int rd(void *ctx, uint32_t lba, uint32_t n, uint8_t *buf)
{
    (void)ctx;
    if ((uint64_t)lba + n > img_blocks) return -1;
    if (fseek(img, (long)lba * 512L, SEEK_SET) || fread(buf, 512, n, img) != n) return -1;
    return 0;
}
/* 64-byte aligned, and full of 0xA5 rather than zeros: the kernel's heap (tlsf) is not cleared either, and a
   counter left uninitialised made the first stick's view "give read errors" in the testbed (21 November 2026) */
static void *al(uint32_t n) { void *p = 0; if (posix_memalign(&p, 64, n ? n : 1)) return 0; memset(p, 0xa5, n); return p; }
static int utf8(const struct fx_dirent *e, char *o)
{
    int k = 0;
    for (int i = 0; i < e->nlen; i++) {
        uint32_t u = e->name[i];
        if (u >= 0xd800 && u < 0xdc00 && i + 1 < e->nlen) u = 0x10000 + ((u - 0xd800) << 10) + (e->name[++i] - 0xdc00);
        if (u < 0x80) o[k++] = (char)u;
        else if (u < 0x800) { o[k++] = (char)(0xc0 | u >> 6); o[k++] = (char)(0x80 | (u & 63)); }
        else if (u < 0x10000) { o[k++] = (char)(0xe0 | u >> 12); o[k++] = (char)(0x80 | ((u >> 6) & 63)); o[k++] = (char)(0x80 | (u & 63)); }
        else { o[k++] = (char)(0xf0 | u >> 18); o[k++] = (char)(0x80 | ((u >> 12) & 63)); o[k++] = (char)(0x80 | ((u >> 6) & 63)); o[k++] = (char)(0x80 | (u & 63)); }
    }
    o[k] = 0;
    return k;
}
static struct fx_vol *v;
static uint64_t h, left, valid_left;
static uint8_t *cbuf;
static int fails;
static int one_run(void *arg, uint32_t c, uint32_t n)
{
    (void)arg;
    for (uint32_t i = 0; i < n && left; i++) {
        if (rd(0, fx_clus_lba(v, c + i), v->spc, cbuf)) { fails++; return -1; }
        uint32_t k = left < v->spc * 512u ? (uint32_t)left : v->spc * 512u;
        for (uint32_t j = 0; j < k; j++) {
            uint8_t b = valid_left ? cbuf[j] : 0;
            if (valid_left) valid_left--;
            h = (h ^ b) * 1099511628211ull;
        }
        left -= k;
    }
    return 0;
}
static void walk(const struct fx_dirent *dir, const char *path, int depth)
{
    struct fx_dir *d = al(sizeof *d);
    struct fx_dirent *e = al(sizeof *e);
    fx_dir_open(d, v, dir);
    int r;
    while ((r = fx_dir_next(d, e)) == 1) {
        char name[1024], p[4096];
        utf8(e, name);
        snprintf(p, sizeof p, "%s%s%s", path, *path ? "/" : "", name);
        if (e->attr & FX_ATTR_DIR) {
            if (depth < 64) walk(e, p, depth + 1);
            continue;
        }
        uint32_t nc;
        h = 1469598103934665603ull; left = e->size; valid_left = e->valid;
        if (e->size && fx_runs(v, e->clus, e->size, e->contig, one_run, 0, &nc)) { printf("BROKEN\t%s\n", p); fails++; continue; }
        printf("%s\t%llu\t%016llx\n", p, (unsigned long long)e->size, (unsigned long long)h);
    }
    if (r < 0) { printf("DIRERROR\t%s\n", path); fails++; }
    free(d); free(e);
}
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: fatfs_test IMAGE\n"); return 2; }
    img = fopen(argv[1], "rb");
    if (!img) { perror(argv[1]); return 2; }
    fseek(img, 0, SEEK_END); img_blocks = (uint32_t)(ftell(img) / 512);
    v = al(sizeof *v);
    uint32_t lba; int type; const char *what;
    if (!fx_probe(rd, 0, img_blocks, v, &lba, &type, &what)) { fprintf(stderr, "no FAT or exFAT volume\n"); return 1; }
    if (fx_mount(v, rd, 0, lba)) { fprintf(stderr, "mount failed\n"); return 1; }
    cbuf = al(v->spc * 512u);
    walk(0, "", 0);
    char lab[64]; int k = 0;
    for (int i = 0; i < v->label_len; i++) lab[k++] = v->label[i] < 128 ? (char)v->label[i] : '?';
    lab[k] = 0;
    fprintf(stderr, "fatfs_test: %s %s at block %u, label \"%s\", %u clusters of %u bytes, %u reads\n", fx_type_name(v->type), what, lba, lab, v->clusters, v->spc * 512u, v->reads);
    return fails ? 1 : 0;
}
