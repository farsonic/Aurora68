/* SPDX-License-Identifier: MPL-2.0
 *
 * usbnet_frame_test.c - native checks of emu68/src/psmac/usbnet_frame.c, the byte-level half of
 * the Pi 3 family's USB Ethernet drivers (LAN9514 on the 3B, LAN7515 on the 3B+, CDC-ECM for
 * QEMU's usb-net). The chips themselves are only on the real Pis; what can be proved here is that
 * frames are wrapped and unwrapped the way the datasheets lay them out and that the hash filter
 * picks the bits Linux's drivers pick (the expected CRCs were computed independently with
 * Python's zlib: bit-reversed zlib.crc32(addr) ^ 0xffffffff).
 *
 *   cc -std=c99 -Wall -I emu68/src/psmac -o /tmp/usbnet_frame_test tools/usbnet_frame_test.c \
 *      emu68/src/psmac/usbnet_frame.c && /tmp/usbnet_frame_test       (make -C mac check runs it)
 */
#include <stdio.h>
#include <string.h>
#include "usbnet_frame.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void test_crc_and_hash(void)
{
    static const struct { uint8_t mac[6]; uint32_t crc; unsigned smsc, lan; } v[] = {
        { { 0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb }, 0x3f7b3b21u, 15, 126 },     /* mDNS, 224.0.0.251 */
        { { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 }, 0xf99baabau, 62, 499 },     /* IPv6 all-nodes */
        { { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 }, 0x7fa32d9bu, 31, 255 },     /* 224.0.0.1 */
        { { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x00 }, 0x67d8c317u, 25, 207 },     /* spanning tree */
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        CHECK(usbnet_ether_crc(v[i].mac, 6) == v[i].crc);
        CHECK(usbnet_hash_bit(USBNET_SMSC95XX, v[i].mac) == v[i].smsc);
        CHECK(usbnet_hash_bit(USBNET_LAN78XX, v[i].mac) == v[i].lan);
    }
}

static void test_tx(void)
{
    uint8_t h[8];
    CHECK(usbnet_tx_header(USBNET_SMSC95XX, h, 60) == 8);
    CHECK(h[0] == 60 && h[1] == 0x30 && h[2] == 0 && h[3] == 0);          /* 60 | FIRST_SEG | LAST_SEG */
    CHECK(h[4] == 60 && h[5] == 0 && h[6] == 0 && h[7] == 0);
    CHECK(usbnet_tx_header(USBNET_SMSC95XX, h, 1514) == 8);
    CHECK(h[0] == 0xea && h[1] == 0x35 && h[4] == 0xea && h[5] == 0x05);
    CHECK(usbnet_tx_header(USBNET_LAN78XX, h, 1514) == 8);
    CHECK(h[0] == 0xea && h[1] == 0x05 && h[2] == 0x40 && h[3] == 0);     /* 1514 | FCS */
    CHECK(h[4] == 0 && h[5] == 0 && h[6] == 0 && h[7] == 0);
    CHECK(usbnet_tx_header(USBNET_CDC_ECM, h, 100) == 0);
}

/* a transfer of two frames and a bad one, as the LAN9514 packs them with MEF on */
static void test_rx_smsc(void)
{
    uint8_t buf[512];
    const uint8_t *f;
    unsigned pos = 0, n = 0, len;
    memset(buf, 0, sizeof buf);
    put32(buf + n, (64u + 4u) << 16);                   /* 64 bytes and the FCS */
    for (unsigned i = 0; i < 64; i++)
        buf[n + 4 + i] = (uint8_t)i;
    n = (n + 4 + 68 + 3) & ~3u;
    put32(buf + n, (63u + 4u) << 16 | 0x8000u);         /* error summary set */
    n = (n + 4 + 67 + 3) & ~3u;
    put32(buf + n, (61u + 4u) << 16 | 0x2000u);         /* broadcast flag, fine */
    buf[n + 4] = 0xab;
    n = n + 4 + 65;                                     /* the last one needs no padding */
    CHECK(usbnet_rx_next(USBNET_SMSC95XX, buf, n, &pos, &f, &len) == 1);
    CHECK(len == 64 && f == buf + 4 && f[63] == 63);
    CHECK(pos == 72);
    CHECK(usbnet_rx_next(USBNET_SMSC95XX, buf, n, &pos, &f, &len) == 0);
    CHECK(pos == 72 + 4 + 68);
    CHECK(usbnet_rx_next(USBNET_SMSC95XX, buf, n, &pos, &f, &len) == 1);
    CHECK(len == 61 && f[0] == 0xab);
    CHECK(usbnet_rx_next(USBNET_SMSC95XX, buf, n, &pos, &f, &len) == -1);
    /* a frame cut off by the end of the transfer */
    pos = 0;
    CHECK(usbnet_rx_next(USBNET_SMSC95XX, buf, 40, &pos, &f, &len) == -1);
    /* a runt: 10 bytes is no Ethernet frame */
    put32(buf, (10u + 4u) << 16);
    pos = 0;
    CHECK(usbnet_rx_next(USBNET_SMSC95XX, buf, 20, &pos, &f, &len) == 0);
}

static void test_rx_lan78xx(void)
{
    uint8_t buf[512];
    const uint8_t *f;
    unsigned pos = 0, n = 0, len;
    memset(buf, 0, sizeof buf);
    put32(buf, 70u + 4u);                               /* RX_CMD_A: 70 bytes and the FCS */
    buf[10] = 0x11;
    n = (10 + 74 + 3) & ~3u;                            /* 84: already aligned */
    CHECK(n == 84);
    put32(buf + n, (100u + 4u) | 0x00400000u);          /* receive error */
    n = (n + 10 + 104 + 3) & ~3u;
    put32(buf + n, 1514u + 4u);
    buf[n + 10] = 0x22;
    CHECK(usbnet_rx_next(USBNET_LAN78XX, buf, sizeof buf, &pos, &f, &len) == 1);
    CHECK(len == 70 && f == buf + 10 && f[0] == 0x11 && pos == 84);
    CHECK(usbnet_rx_next(USBNET_LAN78XX, buf, sizeof buf, &pos, &f, &len) == 0);
    CHECK(pos == 84 + 116);
    CHECK(usbnet_rx_next(USBNET_LAN78XX, buf, sizeof buf, &pos, &f, &len) == -1);   /* 1518 bytes do not fit */
}

static void test_rx_ecm(void)
{
    uint8_t buf[64] = { 0 };
    const uint8_t *f;
    unsigned pos = 0, len;
    CHECK(usbnet_rx_next(USBNET_CDC_ECM, buf, 60, &pos, &f, &len) == 1 && len == 60 && f == buf);
    CHECK(usbnet_rx_next(USBNET_CDC_ECM, buf, 60, &pos, &f, &len) == -1);
    pos = 0;
    CHECK(usbnet_rx_next(USBNET_CDC_ECM, buf, 0, &pos, &f, &len) == -1);
}

static void test_link(void)
{
    struct usbnet_link l;
    usbnet_link_decode(0x7809, 0x01e1, 0x45e1, 0, 0, &l);      /* no link */
    CHECK(!l.up && !l.speed);
    usbnet_link_decode(0x782d, 0x05e1, 0xc5e1, 0, 0, &l);      /* LAN9514 to a 100 Mbit/s switch */
    CHECK(l.up && l.speed == 100 && l.full && l.autoneg_done);
    usbnet_link_decode(0x782d, 0x0061, 0x00a1, 0, 0, &l);      /* 10 full against 10/100 half */
    CHECK(l.up && l.speed == 10 && !l.full);
    usbnet_link_decode(0x796d, 0x0de1, 0xcde1, 0x0300, 0x3c00, &l);  /* LAN7800 at a gigabit */
    CHECK(l.up && l.speed == 1000 && l.full);
}

static void test_filter(void)
{
    const uint8_t own[6] = { 0xb8, 0x27, 0xeb, 0x12, 0x34, 0x56 }, other[6] = { 0xb8, 0x27, 0xeb, 0x12, 0x34, 0x57 };
    const uint8_t bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, mdns[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb };
    uint32_t hash[16];
    memset(hash, 0, sizeof hash);
    CHECK(usbnet_filter_pass(USBNET_SMSC95XX, own, own, hash, 0, 0));
    CHECK(!usbnet_filter_pass(USBNET_SMSC95XX, other, own, hash, 0, 0));
    CHECK(usbnet_filter_pass(USBNET_SMSC95XX, other, own, hash, 1, 0));
    CHECK(usbnet_filter_pass(USBNET_SMSC95XX, bc, own, hash, 0, 0));
    CHECK(!usbnet_filter_pass(USBNET_SMSC95XX, mdns, own, hash, 0, 0));
    CHECK(usbnet_filter_pass(USBNET_SMSC95XX, mdns, own, hash, 0, 1));
    hash[15 >> 5] |= 1u << (15 & 31);                    /* HASHL bit 15 */
    CHECK(usbnet_filter_pass(USBNET_SMSC95XX, mdns, own, hash, 0, 0));
    memset(hash, 0, sizeof hash);
    hash[126 >> 5] |= 1u << (126 & 31);                  /* the LAN7800's table: word 3, bit 30 */
    CHECK(usbnet_filter_pass(USBNET_LAN78XX, mdns, own, hash, 0, 0));
    CHECK(!usbnet_filter_pass(USBNET_SMSC95XX, mdns, own, hash, 0, 0));
}

int main(void)
{
    test_crc_and_hash();
    test_tx();
    test_rx_smsc();
    test_rx_lan78xx();
    test_rx_ecm();
    test_link();
    test_filter();
    printf("usbnet_frame: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
