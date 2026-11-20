/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_power.c — the Pi's power state, watched from the service core.
 *
 * In a Macintosh SE the Pi lives in the 68000's socket and takes its 5 V from there: from an
 * analog board thirty-odd years old, through the socket's pins and the PiStorm, while a Pi 3B
 * draws half an amp to eight tenths. Under-voltage is the first thing to suspect when such a
 * machine misbehaves, and the Pi's firmware knows it: the mailbox tag GET_THROTTLED (0x00030046)
 * answers with
 *   bit 0  under-voltage now          bit 16  under-voltage has occurred since boot
 *   bit 1  ARM frequency capped now   bit 17  ...has occurred
 *   bit 2  throttled now              bit 18  ...has occurred
 *   bit 3  soft temperature limit now bit 19  ...has occurred
 * (the bits Linux's vcgencmd get_throttled shows). alice_status.c reads the same tag for the
 * Amiga's status page, but only with status.sensors=mbox; this file reads it on every board,
 * every two seconds, and says something only when the bits change (20 November 2026, before
 * the first SE bring-up):
 *   [PSMAC] power: under-voltage now; ARM frequency capped (0x30003)
 *   [PSMAC] power: normal again; under-voltage, frequency capping occurred since boot (0x30000)
 * The first under-voltage also gets a "[BOOT]" line, so the remote control's record of the boot
 * (docs/13) keeps it after the log ring has moved on, and the remote's /state carries the bits.
 *
 * The mailbox is shared (support_rpi.c's vc_mbox_lock: the display core's frame buffer calls, the
 * WiFi and Bluetooth on this core). alice_mbox_once (alice_net.c) makes one call under that lock
 * and refuses while the WiFi coroutine, which runs on this same core and yields inside its own
 * mailbox calls, holds it: then the reading is simply taken a little later. One call costs tens
 * of microseconds on a Pi, so every two seconds it is nothing to the loop. QEMU's model of the
 * mailbox answers the buffer but not this tag (8.2 has no GET_THROTTLED); a firmware that does
 * not know the tag leaves its response bit clear. Either way the watch stops after saying so
 * once. mac.power=off switches it off.
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"

extern const char *psmac_option(const char *key);
extern int alice_mbox_once(uint32_t tag, uint32_t a, uint32_t b, uint32_t out[2]);

#define TAG_GET_THROTTLED  0x00030046u
#define POWER_EVERY_MS     2000u
#define POWER_RETRY_MS     100u     /* the WiFi had the mailbox: soon again */
#define POWER_GIVE_UP      3        /* failed calls in a row before the watch stops */

static inline uint64_t gt_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t gt_frq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v & 0xffffffffu; }

static int state;                   /* 0 not started, 1 watching, 2 off or not answered */
static int have;                    /* a reading exists */
static uint32_t bits;               /* the last reading */
static uint64_t next_at;
static int fails, uv_marked;

static const char *const now_names[4]  = { "under-voltage now", "ARM frequency capped", "throttled", "soft temperature limit active" };
static const char *const past_names[4] = { "under-voltage", "frequency capping", "throttling", "soft temperature limit" };

struct sbuf { char *s; unsigned n, cap; };
static void sb_puts(struct sbuf *b, const char *t) { while (*t && b->n + 1 < b->cap) b->s[b->n++] = *t++; b->s[b->n] = 0; }

/* "under-voltage now; ARM frequency capped" from bits 0-3, or the "occurred" list from 16-19 */
static void names(struct sbuf *b, uint32_t v, const char *const *list, const char *sep)
{
    int first = 1;
    for (int i = 0; i < 4; i++)
        if (v & (1u << i)) { if (!first) sb_puts(b, sep); sb_puts(b, list[i]); first = 0; }
}

static void report(uint32_t was, uint32_t v, int first)
{
    char text[160]; struct sbuf b = { text, 0, sizeof text };
    uint32_t now = v & 0xfu, past = (v >> 16) & 0xfu;
    text[0] = 0;
    if (now) {
        names(&b, now, now_names, "; ");
    } else if (first) {
        sb_puts(&b, "normal");
        if (past) { sb_puts(&b, " now; "); names(&b, past, past_names, ", "); sb_puts(&b, " occurred since boot, before the kernel"); }
    } else if (was & 0xfu) {
        sb_puts(&b, "normal again");
        if (past) { sb_puts(&b, "; "); names(&b, past, past_names, ", "); sb_puts(&b, " occurred since boot"); }
    } else {
        /* only the "occurred" bits moved: a dip shorter than the two seconds between readings */
        uint32_t fresh = past & ~(was >> 16);
        if (!fresh) sb_puts(&b, "normal");
        else { sb_puts(&b, "a short "); names(&b, fresh, past_names, ", "); sb_puts(&b, " between two readings, normal again"); }
    }
    kprintf("[PSMAC] power: %s (0x%x)%s\n", text, v, first ? ", watched every 2 s (mac.power=off to stop)" : "");
}

/* the service core's loop (psmac_alice_core), every turn: almost always a compare and a return */
void psmac_power_poll(void)
{
    if (state == 2) return;
    uint64_t now = gt_now();
    if (state == 0) {
        const char *o = psmac_option("mac.power");
        if (o && o[0] == 'o' && o[1] == 'f') { state = 2; kprintf("[PSMAC] power: not watched (mac.power=off)\n"); return; }
        state = 1; next_at = now;
    }
    if ((int64_t)(now - next_at) < 0) return;
    uint32_t o[2] = { 0, 0 };
    int e = alice_mbox_once(TAG_GET_THROTTLED, 0, 0, o);
    if (e == -9) { next_at = now + gt_frq() * POWER_RETRY_MS / 1000u; return; }
    next_at = now + gt_frq() * POWER_EVERY_MS / 1000u;
    if (e) {
        /* -5: the firmware answered without the tag (QEMU, an old start.elf); -2/-3: no answer */
        if (e == -5 || ++fails >= POWER_GIVE_UP) {
            state = 2;
            kprintf("[PSMAC] power: the firmware does not report it (GET_THROTTLED: %s); not watched\n",
                    e == -5 ? "tag not answered" : "no answer from the mailbox");
        }
        return;
    }
    fails = 0;
    uint32_t v = o[0] & 0x000f000fu;
    if (!have || v != bits) report(bits, v, !have);
    if (!uv_marked && (v & 0x00010001u)) { uv_marked = 1; psmac_boot_mark("power: under-voltage seen (GET_THROTTLED 0x%x)", v); }
    bits = v; have = 1;
}

/* for the remote's /state and the status page: the last reading, -1 while there is none */
int64_t psmac_power_bits(void) { return have ? (int64_t)bits : -1; }
