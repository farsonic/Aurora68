/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_diag.h - Setup and Recovery's options and diagnostics in portable C (psmac_diag.c): every option a
 * user or a tester sets in cmdline.txt, with its group, the boards that use it, its valid values and a line of
 * plain English; the validator the Options tab and the one-shot debug start go through; the merge of a one-shot
 * debug start's words into the boot arguments; and the scrubbing of secrets out of a report bundle.
 * Plain C99 with no kernel headers, so tools/diag_test.c tests it on the host (make -C mac check).
 */
#ifndef PSMAC_DIAG_H
#define PSMAC_DIAG_H
#include <stdint.h>

/* the boards, as bits: the IIci is the testbed's default (no mac.board=) */
enum { PDG_SE = 1, PDG_PLUS = 2, PDG_512KE = 4, PDG_512K = 8, PDG_128K = 16, PDG_SOLO = 32, PDG_IICI = 64 };
#define PDG_PLUSFAM (PDG_PLUS | PDG_512KE | PDG_512K | PDG_128K)       /* the Plus's board layer: its keyboard, mouse, ADB */
#define PDG_COMPACT (PDG_SE | PDG_PLUSFAM)                             /* a real logic board behind the classic PiStorm */
#define PDG_ALL     (PDG_COMPACT | PDG_SOLO | PDG_IICI)

#ifdef PSMAC_PC          /* the PC card's group exists in a kernel built with it (psmac_pc.c, docs/19-pc-card.md) */
enum { PDG_G_MACHINE, PDG_G_DISPLAY, PDG_G_SOUND, PDG_G_STORAGE, PDG_G_NETWORK, PDG_G_VIDEO, PDG_G_PC, PDG_G_DEBUG, PDG_GROUPS };
#else
enum { PDG_G_MACHINE, PDG_G_DISPLAY, PDG_G_SOUND, PDG_G_STORAGE, PDG_G_NETWORK, PDG_G_VIDEO, PDG_G_DEBUG, PDG_GROUPS };
#endif

#define PDG_F_SECRET  1     /* never shown: the page has a write-only field, the bundle "(hidden)" */
#define PDG_F_WIFITAB 2     /* set on the WiFi tab (with its scan and test join), not listed on the Options tab */
#define PDG_F_ONCE    4     /* may be in a one-shot debug start (debug-once.txt) */

struct pdg_opt {
    const char *key;
    uint8_t group, flags;
    uint8_t boards;
    const char *values;     /* alternatives joined by |: a word, or a pattern of # (digits), % (hex digits), * (any text) and literals */
    const char *example;    /* the placeholder for a field that takes a pattern */
    const char *text;       /* a line of plain English */
};

extern const struct pdg_opt pdg_opts[];
extern const unsigned pdg_nopts;
extern const char *const pdg_group_name[PDG_GROUPS];

const struct pdg_opt *pdg_find(const char *key);                       /* case-insensitive; NULL for a key the page does not know */
int pdg_board(const char *value);                                      /* mac.board's value (NULL or "" for none) as a PDG_ bit */
const char *pdg_board_word(int bit);                                   /* "se" ... "iici" */
int pdg_uses(const struct pdg_opt *o, int board);                      /* 1 when that board uses the option */
int pdg_choices(const struct pdg_opt *o);                              /* 1 when every value is a plain word (a menu) */
/* v is one of o's values: 1, or 0 with a reason in why ("mac.fpu: fast is not one of exact|on|double|none") */
int pdg_value_ok(const struct pdg_opt *o, const char *v, char *why, uint32_t whymax);

/* A one-shot debug start (debug-once.txt, docs/08 item 122): one line of key=value words, each an option marked
   PDG_F_ONCE with one of its values. 0, or -1 and why */
int pdg_once_check(const char *text, uint32_t n, char *why, uint32_t whymax);
/* args (an bytes, the boot arguments) with once's valid words set in it (each replaces the word of the same key,
   or goes at the end), into out. Returns the new length, -1 when out is too small; *merged and *skipped count the
   words; skipped ones are named in skip_why (the first) */
int pdg_once_merge(const char *args, uint32_t an, const char *once, uint32_t on, char *out, uint32_t cap,
                   int *merged, int *skipped, char *skip_why, uint32_t whymax);

/* The report bundle's secrets: in copied to out with the values of secret options (a key whose last dotted part
   is psk, pass, password, passphrase, key, secret, token or pin, recoverypin, the password part of mac.wifisim
   and the key part of bt.dev) as "(hidden)", "***" left by the kernel's own redaction as "(hidden)" too, and every
   literal occurrence of the given secrets (6 characters or more) as "(hidden)". Returns the bytes in out (NUL
   ended, cut at cap - 1) */
uint32_t pdg_scrub(const char *in, uint32_t n, char *out, uint32_t cap, const char *const *secrets, int nsecrets);

#endif
