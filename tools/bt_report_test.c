/* SPDX-License-Identifier: GPL-2.0-only
 *
 * bt_report_test.c - which Bluetooth reports reach the kernel as keys and which as mouse buttons, on the PC: the host
 * (emu68/src/alice/bt/bthost.c and btle.c, included here so that their own report-map parsers and le_notify run as
 * they are) given a keyboard with a pointer in one device, as the owner's is (24 November 2026, docs/08 item 139: an
 * LE keyboard paired as keyboard and mouse, kind 7, whose Report Map begins 05 01 09 06 a1 01 85 01, "map 247 mouse
 * kbd report" in the heartbeat). The map below is a combination keyboard's of that shape: the keyboard as report 1, a
 * five-button pointer with 12-bit X and Y, a wheel and AC Pan as report 2, consumer keys as report 3. Checked:
 *   - a keyboard report (id 1) is a key report and never a mouse one, Shift and all;
 *   - a consumer report (id 3, volume up) is neither;
 *   - the pointer's report (id 2): its buttons, movement and wheel; its fourth and fifth buttons are in the bits the
 *     kernel masks off (bt_pi.c: b & 7);
 *   - a link going down with a button held lets go of it, an LE one and (new on this day) a Classic boot mouse;
 *     a keyboard's held keys too; each with the link's index in cur_conn, which the kernel uses to keep devices apart.
 * GPL-2.0-only like the code it tests. Run by tools/bt_test.sh.
 */
#include <stdio.h>
#include <string.h>
#include "bthost.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t t_ms;
static int nkb, nms, last_conn_kb, last_conn_ms;
static uint8_t kb[8]; static int ms_dx, ms_dy, ms_w; static unsigned ms_b;
static struct bt_host H;
static void io_send(void *c, const uint8_t *p, unsigned n) { (void)c; (void)p; (void)n; }
static uint32_t io_ms(void *c) { (void)c; return t_ms; }
static void io_log(void *c, const char *l) { (void)c; (void)l; }
static void io_kb(void *c, const uint8_t r[8]) { (void)c; memcpy(kb, r, 8); nkb++; last_conn_kb = H.cur_conn; }
static void io_mouse(void *c, int dx, int dy, int w, unsigned b) { (void)c; ms_dx = dx; ms_dy = dy; ms_w = w; ms_b = b; nms++; last_conn_ms = H.cur_conn; }
static struct bt_io io = { 0, io_send, 0, io_ms, io_log, io_kb, io_mouse, 0, 0, 0 };

static const uint8_t combo_map[] = {
    /* the keyboard: report 1, modifiers, a reserved byte, five LEDs out, six keys */
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2a, 0xff, 0x00, 0x81, 0x00,
    0xc0,
    /* the pointer: report 2, five buttons and three bits of padding, X and Y in 12 bits, a wheel, AC Pan */
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0xf8, 0x26, 0xff, 0x07, 0x75, 0x0c, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0x05, 0x0c, 0x0a, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06,
    0xc0, 0xc0,
    /* consumer keys: report 3, one 16-bit usage */
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x03, 0x15, 0x00, 0x26, 0xff, 0x03, 0x19, 0x00, 0x2a, 0xff, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xc0,
};

/* an LE link up in report mode with the three Report characteristics, as GATT discovery would leave it */
static struct bt_conn *le_link(int slot, uint16_t handle)
{
    struct bt_conn *c = &H.conn[slot];
    memset(c, 0, sizeof *c);
    c->used = 1; c->le = 1; c->handle = handle; c->addr[0] = (uint8_t)(0x10 + slot);
    struct bt_le *l = &c->l;
    l->g = G_UP;
    uint16_t vh[3] = { 0x20, 0x24, 0x28 };
    for (int i = 0; i < 3; i++) { l->chr_vh[i] = vh[i]; l->chr_uuid[i] = U_REPORT; l->rep_id[i] = (uint8_t)(i + 1); l->rep_type[i] = 1; }
    l->nchr = 3;
    l->mouse_ok = (uint8_t)bt_hid_mouse_parse(combo_map, (int)sizeof combo_map, (struct hid_mouse *)(void *)l->hmouse);
    kb_parse(combo_map, sizeof combo_map, &l->kbm);
    return c;
}
static void disconnect(uint16_t handle)
{
    uint8_t b[8] = { 0x04, 0x05, 4, 0, (uint8_t)handle, (uint8_t)(handle >> 8), 0x13 };
    bt_rx(&H, b, 7);
}

int main(void)
{
    memset(&H, 0, sizeof H);
    H.io = &io;
    struct bt_conn *c = le_link(1, 0x41);
    CHECK(c->l.kbm.ok && c->l.mouse_ok, "the map gives a keyboard and a pointer (kbd %d, mouse %d)", c->l.kbm.ok, c->l.mouse_ok);
    CHECK(c->l.kbm.n == 1 && c->l.kbm.r[0].id == 1 && c->l.kbm.r[0].has_mod && c->l.kbm.r[0].nkeys == 6, "the keyboard is report 1: modifiers and six keys");
    {
        const struct hid_mouse *m = (const struct hid_mouse *)(const void *)c->l.hmouse;
        CHECK(m->ids && m->btn.id == 2 && m->x.id == 2 && m->nbtn == 5 && m->x.size == 12, "the pointer is report 2: five buttons, 12-bit X (id %d, %d buttons, x %d bits)", m->btn.id, m->nbtn, m->x.size);
    }
    /* report 1: Shift and A, then nothing */
    { uint8_t r[8] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 }; nkb = nms = 0; le_notify(&H, c, 0x20, r, 8); }
    CHECK(nkb == 1 && nms == 0 && kb[0] == 0x02 && kb[2] == 0x04, "a keyboard report is keys only (kb %d mouse %d)", nkb, nms);
    CHECK(last_conn_kb == 1, "with its link's index");
    { uint8_t r[8] = { 0 }; nkb = nms = 0; le_notify(&H, c, 0x20, r, 8); }
    CHECK(nkb == 1 && nms == 0 && kb[0] == 0 && kb[2] == 0, "and let go");
    /* report 3: volume up, then up again */
    { uint8_t r[2] = { 0xe9, 0x00 }; nkb = nms = 0; le_notify(&H, c, 0x28, r, 2); r[0] = 0; le_notify(&H, c, 0x28, r, 2); }
    CHECK(nkb == 0 && nms == 0, "a consumer report is neither keys nor buttons (kb %d mouse %d)", nkb, nms);
    /* report 2: left button, X +3, Y -2 (12 bits each: 0x003, 0xffe), wheel -1 */
    { uint8_t r[6] = { 0x01, 0x03, 0xe0, 0xff, 0xff, 0x00 }; nkb = nms = 0; le_notify(&H, c, 0x24, r, 6); }
    CHECK(nms == 1 && nkb == 0 && ms_b == 1 && ms_dx == 3 && ms_dy == -2 && ms_w == -1, "the pointer: left button, +3, -2, wheel -1 (b %u, %d, %d, %d)", ms_b, ms_dx, ms_dy, ms_w);
    CHECK(c->l.lastbtn == 1 && last_conn_ms == 1, "the held button noted for the link");
    /* all five buttons: the fourth and fifth are bits 3 and 4, which the kernel's b & 7 drops */
    { uint8_t r[6] = { 0x1f, 0, 0, 0, 0, 0 }; nms = 0; le_notify(&H, c, 0x24, r, 6); }
    CHECK(nms == 1 && ms_b == 0x1f && (ms_b & 7u) == 7, "five buttons: bits 0-4 (b %x)", ms_b);
    /* a keyboard report meanwhile does not touch the buttons */
    { uint8_t r[8] = { 0x08, 0, 0x13, 0, 0, 0, 0, 0 }; nms = 0; le_notify(&H, c, 0x20, r, 8); }
    CHECK(nms == 0, "Command-P from the same device: no mouse report");
    /* the link goes down with the buttons and Command-P held: both let go, as link 1's */
    nkb = nms = 0; last_conn_kb = last_conn_ms = -1;
    disconnect(0x41);
    CHECK(nms == 1 && ms_b == 0 && last_conn_ms == 1, "disconnected with buttons down: let go, link 1's (%d reports, b %u, link %d)", nms, ms_b, last_conn_ms);
    CHECK(nkb == 1 && kb[0] == 0 && kb[2] == 0 && last_conn_kb == 1, "and its keys let go (%d reports)", nkb);
    CHECK(!H.conn[1].used, "the link is free");

    /* a Classic boot mouse (HID interrupt channel, DATA | Input, report 2) disconnecting with its button down */
    {
        struct bt_conn *k = &H.conn[2];
        memset(k, 0, sizeof *k);
        k->used = 1; k->handle = 0x0c; k->addr[0] = 0x22;
        uint8_t r[6] = { 0xa1, 0x02, 0x01, 0x05, 0x00, 0x00 };
        nms = 0; hid_in(&H, k, r, 6);
        CHECK(nms == 1 && ms_b == 1 && last_conn_ms == 2, "a Classic mouse's button down, link 2's");
        nms = 0; disconnect(0x0c);
        CHECK(nms == 1 && ms_b == 0 && last_conn_ms == 2, "its link gone: the button let go (it stayed down before 24 November 2026)");
    }
    printf("bt_report_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
