/* SPDX-License-Identifier: MPL-2.0
 *
 * hfsview_test.c - the kernel's FAT/exFAT reader and HFS view (emu68/src/psmac/psmac_fatfs.c,
 * psmac_hfsview.c) on the host, built from the same sources: a FAT or exFAT image in, the HFS volume the
 * Mac would see out, for hfsutils to check (tools/hfsview_test.sh does, from `make -C mac check`).
 *
 * usage: hfsview_test IMAGE OUT.hfs [--folder NAME] [--list] [--skip-images]
 *   --folder NAME   a folder of the volume's root as the volume (the SD card's "Mac Exchange")
 *   --list          the root's entries as the reader sees them (UTF-16 shown as \uXXXX beyond ASCII)
 *   --skip-images   disk images in the root are not shown (a stick whose images are mounted on their own)
 * The steps are counted and each one's reads too: no step may read more than 64 sectors at a time (the
 * kernel runs them between other work). Written 21 November 2026 (issue #20).
 */
#define _POSIX_C_SOURCE 200112L
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "psmac_fatfs.h"
#include "psmac_hfsview.h"

static FILE *img;
static uint32_t img_blocks, max_n;
static int rd(void *ctx, uint32_t lba, uint32_t n, uint8_t *buf)
{
    (void)ctx;
    if (n > max_n) max_n = n;
    if ((uint64_t)lba + n > img_blocks) return -1;
    if (fseek(img, (long)lba * 512L, SEEK_SET) || fread(buf, 512, n, img) != n) return -1;
    return 0;
}
/* 64-byte aligned, and full of 0xA5 rather than zeros: the kernel's heap (tlsf) is not cleared either, and a
   counter left uninitialised made the first stick's view "give read errors" in the testbed (21 November 2026) */
static void *al(uint32_t n) { void *p = 0; if (posix_memalign(&p, 64, n ? n : 1)) return 0; memset(p, 0xa5, n); return p; }
static void logf_(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
static int eq_ascii(const struct fx_dirent *e, const char *s)
{
    int i = 0;
    for (; i < e->nlen && s[i]; i++) if (e->name[i] != (uint8_t)s[i]) return 0;
    return i == e->nlen && !s[i];
}
static int ends(const struct fx_dirent *e, const char *s)
{
    int n = (int)strlen(s);
    if (e->nlen < n) return 0;
    for (int i = 0; i < n; i++) if ((e->name[e->nlen - n + i] | 0x20) != s[i]) return 0;
    return 1;
}
static int skip_images(void *arg, const struct fx_dirent *e)
{
    (void)arg;
    return !(e->attr & FX_ATTR_DIR) && (ends(e, ".image") || ends(e, ".dsk") || ends(e, ".hda"));
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: hfsview_test IMAGE OUT.hfs [--folder NAME] [--list] [--skip-images]\n"); return 2; }
    const char *folder = 0; int list = 0, skipimg = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--folder") && i + 1 < argc) folder = argv[++i];
        else if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--skip-images")) skipimg = 1;
    }
    img = fopen(argv[1], "rb");
    if (!img) { perror(argv[1]); return 2; }
    fseek(img, 0, SEEK_END); img_blocks = (uint32_t)(ftell(img) / 512);
    struct fx_vol *v = al(sizeof *v);
    uint32_t lba; int type; const char *what;
    if (!fx_probe(rd, 0, img_blocks, v, &lba, &type, &what)) { printf("no FAT or exFAT volume\n"); return 1; }
    int e = fx_mount(v, rd, 0, lba);
    if (e) { printf("mount failed %d\n", e); return 1; }
    printf("volume: %s %s at block %u, %u clusters of %u bytes\n", fx_type_name(v->type), what, lba, v->clusters, v->spc * 512);
    struct fx_dir *d = al(sizeof *d);
    struct fx_dirent *ent = al(sizeof *ent), *fold = al(sizeof *fold);
    int found = 0;
    fx_dir_open(d, v, 0);
    while (fx_dir_next(d, ent) == 1) {
        if (list) {
            printf("  %s ", ent->attr & FX_ATTR_DIR ? "dir " : "file");
            for (int i = 0; i < ent->nlen; i++) { if (ent->name[i] < 128) putchar(ent->name[i]); else printf("\\u%04x", ent->name[i]); }
            printf(" %llu bytes, cluster %u%s\n", (unsigned long long)ent->size, ent->clus, ent->contig ? ", contiguous" : "");
        }
        if (folder && (ent->attr & FX_ATTR_DIR) && eq_ascii(ent, folder)) { *fold = *ent; found = 1; }
    }
    if (folder && !found) { printf("no folder %s in the root\n", folder); return 1; }
    struct hv *h = al(sizeof *h);
    hv_init(h, v, al, free, logf_, "[TEST]");
    if (folder) hv_set_root(h, fold);
    if (skipimg) { h->skip = skip_images; h->skip_arg = 0; }
    uint32_t steps = 0, worst = 0;
    for (;;) {
        uint32_t r0 = v->read_secs;
        int r = hv_step(h);
        steps++;
        if (v->read_secs - r0 > worst) worst = v->read_secs - r0;
        if (r < 0) { printf("build failed\n"); return 1; }
        if (r) break;
    }
    printf("built in %u steps, at most %u sectors read in one step (%u in one read); %u reads in all\n", steps, worst, max_n, v->reads);
    printf("view: \"%.*s\", %u files, %u folders (%u AppleDouble, %u not shown, %u renamed, %u shortened, %u characters not in Mac Roman, %u dropped, %u too big, %u broken, %u over the limit)\n",
           h->vname[0], h->vname + 1, h->files_all, h->dirs_all, h->n_ad, h->n_hidden, h->n_renamed, h->n_short, h->n_unmapped, h->n_dropped, h->n_big, h->n_broken, h->n_full);
    printf("HFS: %u allocation blocks of %u bytes, %u blocks of 512 (%u MB), catalog %u nodes (%u used, %u leaves, depth %u, %u map nodes), memory %u bytes\n",
           h->nablks, h->ablk, h->total, h->total / 2048, h->cat_nodes, h->cat_used, h->nleaves, h->depth, h->map_nodes, h->mem);
    if (steps > 1 && worst > 64) { printf("FAIL: a step read %u sectors\n", worst); return 1; }
    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror(argv[2]); return 2; }
    uint8_t *buf = al(128 * 512);
    for (uint32_t b = 0; b < h->total; ) {
        uint32_t n = h->total - b < 128 ? h->total - b : 128;
        /* odd sizes and offsets as the Mac's reads come: 1, 7, 128 blocks */
        if (b % 3 == 1 && n > 7) n = 7;
        if (hv_read(h, b, n, buf)) { printf("FAIL: read of blocks %u-%u failed\n", b, b + n - 1); return 1; }
        int zero = 1;
        for (uint32_t i = 0; i < n * 512 && zero; i++) zero = !buf[i];
        if (zero) fseek(o, (long)n * 512, SEEK_CUR);          /* a sparse file: a 2 GB volume is mostly empty space */
        else fwrite(buf, 512, n, o);
        b += n;
    }
    if (ftruncate(fileno(o), (off_t)h->total * 512)) { perror("ftruncate"); return 2; }
    fclose(o);
    hv_release(h);
    return 0;
}
