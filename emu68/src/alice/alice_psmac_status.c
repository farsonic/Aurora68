/* SPDX-License-Identifier: GPL-2.0-only
 *
 * alice_psmac_status.c - the status page's Macintosh half (Aurora68 Status 2.0, 23 November 2026).
 *
 * Backstory. The owner's Pi 3B, in the SE's 68000 socket, played music videos for long stretches and
 * got hot: the firmware capped the ARM's clock, and the 68k fell from 169 MIPS to 31 (the heartbeat's
 * "JIT: ... MIPS" line said so, but only in the kernel's log). He asked for the numbers on the Mac's
 * own screen, as gauges. Alice68's status page (alice_status.h, at slot-$9 window + 0x50000 on the
 * Mac) already had a place for most of them, filled for the Amiga's cores; this file fills it for
 * the Macintosh build, from core 3's loop once a second (alice_status_poll calls it), and adds what
 * the gauges need at the page's end (0x1a0-0x1ff, alice_status.h).
 *
 * The cores, as Aurora68 uses them:
 *   0  the 68k: Emu68's JIT and the code it made. It never idles (the Mac's ROM waits in loops, not
 *      in STOP), so its load is not measured; its speed is: insn_rate, the JIT's instruction counter
 *      (M68KState.INSN_COUNT, written back by core 0 at its exits from translated code) over the
 *      last second.
 *   1  USB, the sound output, the video decoder card: it waits in wfe when there is nothing to do
 *      (psmac_usb.c counts those ticks); its load is the rest.
 *   2  the bus: every 68k access to the Mac's own board. It polls without pause, so its load is the
 *      time of the turns that served the 68k a cycle (psmac_bus.c counts them).
 *   3  the services: mailbox, network, Bluetooth, screens, the remote. Alice68's own figure, the
 *      loop's time beyond its shortest turn (alice_status.c's loop_busy, which runs on this core).
 *
 * The firmware's sensors, through the VideoCore mailbox (Raspberry Pi firmware wiki, "Mailbox
 * property interface"): GET_TEMPERATURE and GET_MAX_TEMPERATURE (id 0), GET_CLOCK_RATE,
 * GET_CLOCK_RATE_MEASURED and GET_MAX_CLOCK_RATE (clock id 3, the ARM). The BCM2711's AVS register
 * that alice_status.c reads does not exist on a Pi 3, so there the mailbox is the only thermometer.
 * The mailbox is shared (support_rpi.c's vc_mbox_lock): alice_mbox_once makes one call under that
 * lock, refuses while the WiFi coroutine holds it (-9: then the reading waits a second), and says -5
 * when the firmware answered the buffer but not the tag (QEMU's raspi3b knows the clock rates but no
 * temperature and no measured clock). A tag that is not answered is not asked again; one that is
 * costs a few tens of microseconds, three a second. The throttled bits come from psmac_power.c,
 * which reads GET_THROTTLED every two seconds already.
 *
 * mac.sensors=off: no mailbox calls from here. mac.sensorsim=on (testbed): made-up sensors, a Pi 3B
 * that heats from 48 to 84 C over five minutes and cools over three, its soft limit at 60 C and its
 * clock capped from 1200 to 600 MHz at 80 C, so the gauges can be seen moving under QEMU; the page's
 * flags say ALICE68_STF_SIMULATED and the window says "simulated".
 */
#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "M68k.h"
#include "alice_psmac.h"
#include "alice_status.h"
#include "psmb.h"
#include "psmac_video.h"

extern void *tlsf;
extern struct M68KState *__m68k_state;
extern int alice_mbox_once(uint32_t tag, uint32_t a, uint32_t b, uint32_t out[2]);
extern const char *psmac_option(const char *key);
extern volatile uint64_t psmac_core1_idle_ticks, psmac_bus_busy_ticks;
extern const volatile struct a68v_block *psmac_videocard_block(void);

#define UNK ALICE68_ST_UNKNOWN

/* one firmware value: its tag, its argument (the clock or sensor id), what it last said */
struct fw_value {
    uint32_t tag, id;
    const char *name;
    int once;                   /* a fixed value: asked until it answers, then kept */
    int dead;                   /* not answered: not asked again */
    int fails;
    uint32_t val;               /* UNK until answered */
};
static struct fw_value fw[] = {
    { 0x00030006u, 0, "temperature",       0, 0, 0, UNK },   /* GET_TEMPERATURE, thousandths of a degree */
    { 0x0003000au, 0, "temperature limit", 1, 0, 0, UNK },   /* GET_MAX_TEMPERATURE */
    { 0x00030002u, 3, "ARM clock",         0, 0, 0, UNK },   /* GET_CLOCK_RATE: what the firmware set */
    { 0x00030047u, 3, "measured ARM clock",0, 0, 0, UNK },   /* GET_CLOCK_RATE_MEASURED: what runs */
    { 0x00030004u, 3, "nominal ARM clock", 1, 0, 0, UNK },   /* GET_MAX_CLOCK_RATE */
};
enum { FW_TEMP, FW_TEMP_MAX, FW_ARM, FW_ARM_MEAS, FW_ARM_MAX, FW_N };

static int started, sensors_off, sim, said;
static uint64_t t_prev, t_first, idle_prev, bus_prev, insn_prev;
static uint32_t load1 = UNK, load2 = UNK, rate = UNK;
static uint32_t sim_temp, sim_arm, sim_bits;
static const volatile struct alice68_status *page;     /* for the remote's /state (psmac_remote.c) */

static int word_on(const char *o) { return o && ((o[0] == 'o' && o[1] == 'n') || o[0] == '1' || o[0] == 'y'); }

static void read_firmware(void)
{
    for (int i = 0; i < FW_N; i++) {
        struct fw_value *f = &fw[i];
        if (f->dead || (f->once && f->val != UNK)) continue;
        uint32_t o[2] = { 0, 0 };
        int e = alice_mbox_once(f->tag, f->id, 0, o);
        if (e == -9) return;                        /* the WiFi has the mailbox: the rest next second */
        if (e) {
            if (e == -5 || ++f->fails >= 3) {
                f->dead = 1;
                kprintf("[PSMAC] status: the firmware does not give the %s (tag %x: %s)\n", f->name, f->tag,
                        e == -5 ? "not answered" : "no answer from the mailbox");
            }
            continue;
        }
        f->fails = 0;
        f->val = o[1];
    }
}

/* the made-up Pi 3B of mac.sensorsim=on: an eight-minute cycle, five heating, three cooling */
static void simulate(uint64_t now, uint64_t hz)
{
    uint32_t t = (uint32_t)((now - t_first) / hz) % 480u;
    uint32_t mc = t < 300u ? 48000u + 36000u * t / 300u : 84000u - 36000u * (t - 300u) / 180u;
    mc += (uint32_t)((now / (hz / 8u)) % 5u) * 100u;   /* a little noise, as a real sensor has */
    uint32_t bits = 0;
    if (mc >= 60000u) bits |= 1u << 3;                  /* soft temperature limit */
    if (mc >= 80000u) bits |= 1u << 1 | 1u << 2;        /* frequency capped, throttled */
    sim_bits = (sim_bits & 0xf0000u) | (bits << 16) | bits;
    sim_temp = mc;
    sim_arm = mc >= 80000u ? 600000000u : 1200000000u;
}

/* core 3, before the page is opened for writing */
void psmac_status_measure(uint64_t now, uint64_t hz)
{
    if (!started) {
        started = 1;
        const char *o = psmac_option("mac.sensors");
        sensors_off = o && o[0] == 'o' && o[1] == 'f';
        sim = word_on(psmac_option("mac.sensorsim"));
        t_first = t_prev = now;
        idle_prev = psmac_core1_idle_ticks; bus_prev = psmac_bus_busy_ticks;
        insn_prev = __m68k_state ? __m68k_state->INSN_COUNT : 0;
        if (sim) kprintf("[PSMAC] status: mac.sensorsim=on: the temperature, clock and throttling on the status page are made up\n");
        else if (sensors_off) kprintf("[PSMAC] status: mac.sensors=off: the firmware's temperature and clock are not read\n");
        return;
    }
    uint64_t dt = now - t_prev;
    if (!dt) return;
    uint64_t idle = psmac_core1_idle_ticks, bus = psmac_bus_busy_ticks;
    uint64_t insn = __m68k_state ? __m68k_state->INSN_COUNT : 0;
    /* core 1: the time not in its wfe; before its loop has waited once it has no figure */
    if (!idle) load1 = UNK;
    else { uint64_t i = idle - idle_prev; load1 = i >= dt ? 0 : (uint32_t)(10000u - i * 10000u / dt); }
    { uint64_t b = bus - bus_prev; load2 = b >= dt ? 10000u : (uint32_t)(b * 10000u / dt); }
    rate = __m68k_state ? (uint32_t)((insn - insn_prev) * hz / dt) : UNK;
    t_prev = now; idle_prev = idle; bus_prev = bus; insn_prev = insn;

    if (sim) simulate(now, hz);
    else if (!sensors_off) {
        read_firmware();
        if (!said && fw[FW_TEMP].val != UNK && fw[FW_ARM].val != UNK) {
            said = 1;
            kprintf("[PSMAC] status: the firmware's sensors: %u.%u C (limit %u C), ARM %u MHz of %u MHz\n",
                    fw[FW_TEMP].val / 1000u, fw[FW_TEMP].val / 100u % 10u, fw[FW_TEMP_MAX].val == UNK ? 0 : fw[FW_TEMP_MAX].val / 1000u,
                    fw[FW_ARM].val / 1000000u, fw[FW_ARM_MAX].val == UNK ? 0 : fw[FW_ARM_MAX].val / 1000000u);
        }
    }
}

/* core 3, with the page's seq odd: everything above, and the cards, into the page */
void psmac_status_fill(volatile struct alice68_status *s)
{
    static const char *const names[4] = { "68k JIT", "USB, sound, video card", "Mac bus", "services" };
    static const uint32_t roles[4] = { ALICE68_ROLE_JIT, ALICE68_ROLE_MEDIA, ALICE68_ROLE_BUS, ALICE68_ROLE_SERVICE };
    page = s;
    if (s->core[2].role != ALICE68_ROLE_BUS)
        for (int i = 0; i < 4; i++) {
            s->core[i].role = roles[i];
            volatile char *d = s->core[i].name;
            unsigned k = 0;
            for (const char *n = names[i]; *n && k + 1 < sizeof s->core[i].name; n++) d[k++] = *n;
            while (k < sizeof s->core[i].name) d[k++] = 0;
        }
    s->core[3].busy = s->core[1].busy;              /* alice_status.c's own figure is this core's: core 3 runs it */
    s->core[0].busy = UNK;
    s->core[1].busy = load1;
    s->core[2].busy = load2;
    s->insn_rate = rate;
    s->heap_free = (uint32_t)tlsf_get_free_size(tlsf);
    s->heap_total = (uint32_t)tlsf_get_total_size(tlsf);

    if (sim) {
        s->flags |= ALICE68_STF_SIMULATED;
        s->temp_mc = sim_temp; s->temp_max_mc = 85000u;
        s->arm_hz = s->arm_measured_hz = sim_arm; s->arm_max_hz = 1200000000u;
        s->throttled = sim_bits;
        s->sensors_ms = s->uptime_ms;
    } else if (!sensors_off) {
        if (s->temp_mc == UNK) s->temp_mc = fw[FW_TEMP].val;    /* a Pi 3: no AVS register, the mailbox's */
        s->temp_max_mc = fw[FW_TEMP_MAX].val;
        if (fw[FW_ARM].val != UNK) s->arm_hz = fw[FW_ARM].val;
        s->arm_measured_hz = fw[FW_ARM_MEAS].val;
        s->arm_max_hz = fw[FW_ARM_MAX].val;
        if (fw[FW_TEMP].val != UNK || fw[FW_ARM].val != UNK) s->sensors_ms = s->uptime_ms;
    }

    /* the video decoder card (psmac_video.h's block, in its own slot) */
    const volatile struct a68v_block *v = psmac_videocard_block();
    if (v) {
        s->vcard_state = v->state; s->vcard_load = v->load;
        s->vcard_decode_us = v->decode_us; s->vcard_draw_us = v->draw_us;
        s->vcard_buffer_ms = v->version >= 2 ? v->buffer_ms : UNK;
        s->vcard_underruns = v->underruns;
        s->vcard_shown = v->frames_shown; s->vcard_dropped = v->frames_dropped;
    } else s->vcard_state = UNK;

    /* the sound card and its stream channel, in this same window (psmb.h) */
    const volatile uint8_t *win = (const volatile uint8_t *)s - ALICE_STATUS_WINDOW_OFF;
    const volatile struct psmb_sound *snd = (const volatile struct psmb_sound *)(win + PSMB_OFF_SOUND);
    const volatile struct psmb_stream *str = (const volatile struct psmb_stream *)(win + PSMB_OFF_STREAM);
    if (snd->magic == PSMB_SND_MAGIC) {
        s->snd_out = snd->out_kind; s->snd_playing = snd->state ? 1u : 0u; s->snd_underruns = snd->underruns;
    } else s->snd_out = UNK;
    if (str->magic == PSMB_STR_MAGIC) {
        s->str_state = str->state; s->str_load = str->load; s->str_underruns = str->underruns;
    } else s->str_state = UNK;
}

/* the remote control's /state (psmac_remote.c): the page as last filled, 0 before the first fill */
const volatile struct alice68_status *psmac_status_page(void) { return page; }
