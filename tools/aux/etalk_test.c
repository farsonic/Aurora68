/* SPDX-License-Identifier: MPL-2.0
 *
 * etalk_test.c - the A/UX network card's chip model (emu68/src/psmac/psmac_etalk.c) on the host. Run by
 * tools/aux/etalk_test.sh.
 *
 * The test plays both ends. On one side it does what A/UX's ae6 driver was seen to do (docs/24-aux-network.md
 * section 3): sizes the buffer, stops the chip and waits for the reset flag, programs it, starts it, sends a frame
 * from page 0, and takes received frames out of the ring with the driver's own arithmetic (the boundary page one
 * behind, the wrap from the ring's last page to page 6). On the other side it is the network board's kernel half
 * (alice_net.c): it answers the unit's commands, empties the transmit ring and fills the receive ring. Checked: the
 * ROM and the address cell, what is mapped and what is not, the slot interrupt's bit and its acknowledgement, 4,000
 * frames of every length through a ring that wraps many times, frames waiting in the board's ring while the driver
 * stays away (nothing overwritten, no overflow reported), the address filter, the 68k's RESET.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../emu68/src/alice/alice_net.h"

int psmac_board_se = 1, psmac_board_solo = 0, psmac_hdmi_mode = 2;
uint32_t psmac_videocard_size(void) { return 0x104000; }
const char *psmac_option(const char *k) { return !strcmp(k, "mac.auxnet") ? "on" : 0; }
static uint8_t board[AN_BOARD_BYTES] __attribute__((aligned(8)));
volatile uint8_t *alice_net_board(void) { return board; }

uintptr_t psmac_etalk_phys(uint32_t p);
int psmac_etalk_ro(uint32_t p);
int psmac_etalk_access(uint32_t addr, int size, int write, uint32_t data, uint32_t *val);
unsigned psmac_etalk_slot_irq(void);
void psmac_etalk_poll(void), psmac_etalk_reset(void), psmac_etalk_init(void);

#define BASE 0xFD000000u                /* slot $D, where mac.auxnet=on puts the card */
static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)
static void wr(int reg, uint8_t v)
{
    uint32_t x;
    OK(psmac_etalk_access(BASE + 0xE0000 + 0x3C - 4 * (uint32_t)reg, 1, 1, v, &x), "a register write is the card's");
}
static uint8_t rd(int reg)
{
    uint32_t x = 0;
    OK(psmac_etalk_access(BASE + 0xE0000 + 0x3C - 4 * (uint32_t)reg, 1, 0, 0, &x), "a register read is the card's");
    return (uint8_t)x;
}
static uint8_t *ram;
static struct an_unit *unit(int u) { return (struct an_unit *)(board + AN_OFF_UNIT(u)); }

/* ---- the network board's kernel half ---- */
static int rxon, sent;
static uint8_t last_tx[1600];
static uint32_t last_tx_len, cmds[16], ncmds;
static void board_turn(void)
{
    struct an_unit *u = unit(1);
    if (u->cmd_seq != u->cmd_done) {
        if (ncmds < 16) cmds[ncmds++] = u->cmd;
        if (u->cmd == AN_CMD_RXON) { rxon = 1; u->rx_on = 1; }
        if (u->cmd == AN_CMD_RXOFF) { rxon = 0; u->rx_on = 0; }
        u->cmd_result = 0;
        u->cmd_done = u->cmd_seq;
    }
    while (u->tx_tail != u->tx_head) {
        struct an_slot *s = (struct an_slot *)(board + AN_OFF_TX(1) + (u->tx_tail % AN_TX_SLOTS) * AN_SLOT_BYTES);
        last_tx_len = s->len;
        memcpy(last_tx, s->data, s->len);
        sent++;
        u->tx_tail++;
    }
}
static int board_rx(const uint8_t *f, uint32_t len)
{
    struct an_unit *u = unit(1);
    if (!rxon || u->rx_head - u->rx_tail >= AN_RX_SLOTS) return 0;
    struct an_slot *s = (struct an_slot *)(board + AN_OFF_RX(1) + (u->rx_head % AN_RX_SLOTS) * AN_SLOT_BYTES);
    memcpy(s->data, f, len);
    s->len = len;
    s->flags = 0;
    u->rx_head++;
    return 1;
}

/* ---- ae6's half, as its code does it ---- */
static const uint8_t mymac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static uint32_t last_page;                                  /* the driver's record +$6E: $3F or $FE */
static uint8_t boundary_page(void) { uint8_t b = (uint8_t)(rd(3) + 1); return b > last_page ? 6 : b; }
static uint8_t current_page(void) { wr(0, 0x40); uint8_t c = rd(7); wr(0, 0x00); return c; }
static int halt(void)
{
    wr(0, 0x21); wr(10, 0); wr(11, 0);
    for (int i = 0; i < 1000 && !(rd(7) & 0x80); i++) { }
    return !(rd(7) & 0x80);
}
static void ae6_init(void)
{
    OK(!halt(), "halt: the reset flag comes");
    wr(14, 0x49); wr(12, 0x0c); wr(13, 0x04); wr(1, 6); wr(2, (uint8_t)(last_page + 1)); wr(3, (uint8_t)last_page); wr(7, 0xff);
    wr(0, 0x61); wr(7, 6);
    for (int i = 0; i < 6; i++) wr(1 + i, mymac[i]);
    for (int i = 0; i < 8; i++) wr(8 + i, 0);               /* ae6_mar_set: the filter, then page 0 again */
    wr(0, 0x21);
    wr(15, 0x1f); wr(0, 0x22); wr(13, 0);
}
static uint32_t got_frames, bad_frames;
static uint8_t expect_fill;
static void ae6_rintr(void)                                 /* while the boundary page is not the current page */
{
    for (;;) {
        uint8_t b = boundary_page(), c = current_page();
        if (b == c) break;
        uint8_t *p = ram + ((uint32_t)b << 8), *end = ram + ((last_page + 1) << 8);
        uint32_t count = p[2] | (uint32_t)p[3] << 8, next = p[1];
        OK(p[0] & 1, "the header's status says received");
        OK(count >= 68 && count <= 1522, "the header's length");
        uint32_t pages = (count + 255) >> 8, want_next = b + pages;
        if (want_next > last_page) want_next -= last_page + 1 - 6;
        OK(next == want_next, "the header's next page");
        /* byte 14 of each test frame is its number in the sequence: the frames arrive in order, none twice */
        uint8_t *q = p + 4 + 14;
        if (q >= end) q = ram + 0x600 + (q - end);
        if (*q != expect_fill) bad_frames++;
        expect_fill = (uint8_t)(expect_fill % 250 + 1);
        got_frames++;
        uint8_t nb = (uint8_t)(next - 1);                   /* update_boundary_page */
        if (nb < 6) nb = (uint8_t)last_page;
        wr(3, nb);
    }
}
static void ae6_int(void)
{
    uint8_t isr = rd(7);
    OK(isr != 0, "the handler finds a flag");
    if (isr & 0x0a) wr(7, isr & 0x0a);
    isr = rd(7);
    if (isr & 0x15) { wr(7, isr & 0x15); ae6_rintr(); OK(!(isr & 0x10), "no overflow warning"); }
}
static void ae6_send(const uint8_t *f, uint32_t len)
{
    memcpy(ram, f, len);
    if (len < 60) len = 60;
    wr(6, (len >> 8) & 7); wr(5, len & 0xff); wr(4, 0); wr(0, 0x26);
}
static void turn(void) { psmac_etalk_poll(); if (psmac_etalk_slot_irq()) ae6_int(); }

int main(void)
{
    struct an_hdr *h = (struct an_hdr *)board;
    h->magic = AN_MAGIC; h->nunits = 2;
    unit(1)->kind = AN_KIND_ETHER; unit(1)->link = 1; memcpy(unit(1)->mac, mymac, 6);
    unit(0)->kind = AN_KIND_WIFI;
    psmac_etalk_init();
    ram = (uint8_t *)psmac_etalk_phys(BASE + 0xD0000);
    uint8_t *rom = (uint8_t *)psmac_etalk_phys(BASE + 0xFFF000);
    OK(ram && rom, "the buffer and the ROM's page are mapped");
    OK(!psmac_etalk_phys(BASE + 0xE0000) && !psmac_etalk_phys(BASE) && !psmac_etalk_phys(0xFE0D0000u), "the registers, the rest of the slot and other slots are not");
    OK(psmac_etalk_ro(BASE + 0xFFF000) && !psmac_etalk_ro(BASE + 0xD0000), "the ROM is read-only, the buffer is not");
    OK(rom[0xFFA] == 0x5A && rom[0xFFB] == 0x93 && rom[0xFFC] == 0x2B && rom[0xFFD] == 0xC7 && rom[0xFFF] == 0x0F, "the format block's test pattern and byte lanes at the slot's top");
    psmac_etalk_poll();
    OK(!memcmp(rom + 0x1000 - 204 - 8, mymac, 6), "the address in the cell below the image");
    uint32_t v;
    OK(psmac_etalk_access(BASE + 0xE003C, 1, 0, 0, &v) && (v & 0xff) == 0x21, "the probe's read: the command register after reset");
    OK(psmac_etalk_access(BASE + 0xE003C, 4, 0, 0, &v) && v == 0x21ffffffu, "a long read: the chip on the high byte lane");
    OK(!psmac_etalk_access(BASE + 0x1000, 1, 0, 0, &v) && !psmac_etalk_access(0xFE0E003Cu, 1, 0, 0, &v), "the rest of the slot and other slots are not answered");
    OK(!psmac_etalk_slot_irq(), "no interrupt after reset");
    ram[0] = 0x12; ram[1] = 0x34;                           /* size_ram */
    last_page = (ram[0x4000] == 0x12 && ram[0x4001] == 0x34) || (ram[0x8000] == 0x12 && ram[0x8001] == 0x34) ? 0x3f : 0xfe;
    OK(last_page == 0xfe, "64 KiB with no alias");
    ae6_init();
    OK(!psmac_etalk_slot_irq(), "no interrupt after the start");
    for (int i = 0; i < 6; i++) { psmac_etalk_poll(); board_turn(); }
    OK(ncmds == 3 && cmds[0] == AN_CMD_START && cmds[1] == AN_CMD_RXON && cmds[2] == AN_CMD_FILTER && rxon, "START, RXON, FILTER to the board");

    uint8_t f[1600];                                        /* a frame out */
    memset(f, 0xff, 6); memcpy(f + 6, mymac, 6); f[12] = 8; f[13] = 6; memset(f + 14, 0x5a, 28);
    ae6_send(f, 42);
    OK(psmac_etalk_slot_irq() == 0x10, "the transmit interrupt, slot $D's bit");
    OK(rd(4) & 1, "TSR says sent");
    ae6_int();
    OK(!psmac_etalk_slot_irq(), "acknowledged: the line is down");
    board_turn();
    OK(sent == 1 && last_tx_len == 60 && !memcmp(last_tx, f, 42), "the frame is in the board's transmit ring");

    /* frames in: every length, to the driver's address or to all, the ring wrapping many times */
    expect_fill = 1;
    uint32_t pushed = 0;
    uint8_t fillv = 1;
    for (uint32_t n = 0; n < 4000; n++) {
        uint32_t len = 14 + 1 + (n * 37) % 1500;
        if (n & 1) memset(f, 0xff, 6); else memcpy(f, mymac, 6);
        memset(f + 6, 0x77, 6); f[12] = 8; f[13] = 0; memset(f + 14, fillv, len - 14);
        while (!board_rx(f, len)) turn();
        pushed++; fillv = (uint8_t)(fillv % 250 + 1);
        if (n % 3 == 0) turn();
        if (n == 2000) {       /* the driver stays away: frames wait in the board's ring, nothing is overwritten */
            for (int k = 0; k < 400; k++) {
                memcpy(f, mymac, 6); memset(f + 14, fillv, 1500);
                if (!board_rx(f, 1514)) { psmac_etalk_poll(); if (!board_rx(f, 1514)) break; }
                pushed++; fillv = (uint8_t)(fillv % 250 + 1);
            }
            OK(psmac_etalk_slot_irq(), "frames waiting: the line is up");
        }
    }
    for (int k = 0; k < 100; k++) turn();
    OK(got_frames == pushed, "every frame arrived");
    OK(!bad_frames, "every frame in its turn");
    printf("frames pushed %u, taken %u, out of turn %u\n", pushed, got_frames, bad_frames);

    uint32_t before = got_frames;                           /* not for the driver */
    memset(f, 0x02, 6); f[5] = 0x99; board_rx(f, 100);
    f[0] = 0x01; board_rx(f, 100);                          /* a group address, the filter empty */
    for (int k = 0; k < 4; k++) turn();
    OK(got_frames == before && unit(1)->rx_tail == unit(1)->rx_head, "frames for others are taken from the board and dropped");

    memcpy(f, mymac, 6); memset(f + 14, expect_fill, 50); board_rx(f, 64); psmac_etalk_poll();   /* the 68k's RESET */
    OK(psmac_etalk_slot_irq(), "a frame pending");
    psmac_etalk_reset();
    OK(!psmac_etalk_slot_irq() && (rd(0) & 1) && (rd(7) & 0x80), "reset: no interrupt, stopped, the reset flag");
    for (int i = 0; i < 3; i++) { psmac_etalk_poll(); board_turn(); }
    OK(!rxon, "RXOFF to the board after the reset");
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
