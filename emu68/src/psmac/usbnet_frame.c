/* SPDX-License-Identifier: MPL-2.0
 *
 * usbnet_frame.c - framing, hashing and link decoding for the Pi 3 family's USB Ethernet chips
 * (usbnet_frame.h says which chips and where the layouts come from). Written from the Microchip
 * datasheets' register and descriptor tables; portable C, no hardware, so tools/usbnet_frame_test.c
 * checks it natively.
 */
#include "usbnet_frame.h"

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* The CRC the MAC computes over the address as it arrives, bit 0 of each byte first, polynomial
   0x04C11DB7, register preset to all ones, no final inversion; the hash filters index by its
   most significant bits. */
uint32_t usbnet_ether_crc(const uint8_t *data, unsigned len)
{
    uint32_t crc = 0xffffffffu;
    while (len--) {
        uint8_t b = *data++;
        for (int i = 0; i < 8; i++, b >>= 1) {
            uint32_t top = (crc >> 31) ^ (b & 1u);
            crc <<= 1;
            if (top)
                crc ^= 0x04c11db7u;
        }
    }
    return crc;
}

unsigned usbnet_hash_bit(enum usbnet_chip chip, const uint8_t mac[6])
{
    uint32_t crc = usbnet_ether_crc(mac, 6);
    return chip == USBNET_LAN78XX ? (crc >> 23) & 0x1ffu : (crc >> 26) & 0x3fu;
}

unsigned usbnet_tx_header(enum usbnet_chip chip, uint8_t *hdr, unsigned len)
{
    switch (chip) {
    case USBNET_SMSC95XX:
        put_le32(hdr, len | 0x2000u | 0x1000u);         /* TX_CMD_A: buffer size, first and last segment */
        put_le32(hdr + 4, len);                         /* TX_CMD_B: frame length */
        return 8;
    case USBNET_LAN78XX:
        put_le32(hdr, (len & 0xfffffu) | 0x00400000u);  /* TX_CMD_A: length, the chip appends the FCS */
        put_le32(hdr + 4, 0);                           /* TX_CMD_B: no segmentation, no VLAN tag */
        return 8;
    default:
        return 0;
    }
}

int usbnet_rx_next(enum usbnet_chip chip, const uint8_t *buf, unsigned n, unsigned *pos,
                   const uint8_t **frame, unsigned *flen)
{
    unsigned p = *pos, hdr, len, bad, next;
    if (chip == USBNET_CDC_ECM) {
        /* one transfer, one frame, no FCS */
        if (p >= n)
            return -1;
        *frame = buf + p;
        *flen = n - p;
        *pos = n;
        return (n - p >= 14 && n - p <= USBNET_FRAME_MAX) ? 1 : 0;
    }
    if (chip == USBNET_SMSC95XX) {
        if (p + 4 > n)
            return -1;
        uint32_t sts = le32(buf + p);
        hdr = 4;
        len = (sts >> 16) & 0x3fffu;                    /* RX_STS_FL: the frame with its FCS */
        bad = (sts & 0x8000u) != 0;                     /* RX_STS_ES: CRC, runt, too long, MII error ... */
    } else {
        if (p + 10 > n)
            return -1;
        uint32_t a = le32(buf + p);
        hdr = 10;
        len = a & 0x3fffu;                              /* RX_CMD_A length, FCS included */
        bad = (a & 0x00400000u) != 0;                   /* RX_CMD_A_RED: receive error detected */
    }
    if (!len || p + hdr + len > n) {
        *pos = n;                                       /* a cut-off frame: the rest of the buffer is no good */
        return -1;
    }
    next = (p + hdr + len + 3u) & ~3u;                  /* the next one starts on four bytes */
    *pos = next < n ? next : n;
    *frame = buf + p + hdr;
    *flen = len >= 4 ? len - 4 : 0;
    if (bad || *flen < 14 || *flen > USBNET_FRAME_MAX)
        return 0;
    return 1;
}

void usbnet_link_decode(unsigned bmsr, unsigned adv, unsigned lpa, unsigned ctrl1000, unsigned stat1000,
                        struct usbnet_link *l)
{
    unsigned common = adv & lpa;
    l->up = (bmsr & 0x0004u) != 0;                      /* BMSR link status */
    l->autoneg_done = (bmsr & 0x0020u) != 0;
    l->speed = l->full = 0;
    if (!l->up)
        return;
    if ((ctrl1000 & 0x0200u) && (stat1000 & 0x0800u))          { l->speed = 1000; l->full = 1; }
    else if ((ctrl1000 & 0x0100u) && (stat1000 & 0x0400u))     { l->speed = 1000; l->full = 0; }
    else if (common & 0x0100u)                                  { l->speed = 100;  l->full = 1; }
    else if (common & 0x0080u)                                  { l->speed = 100;  l->full = 0; }
    else if (common & 0x0040u)                                  { l->speed = 10;   l->full = 1; }
    else                                                        { l->speed = 10;   l->full = 0; }
}

int usbnet_filter_pass(enum usbnet_chip chip, const uint8_t dst[6], const uint8_t own[6],
                       const uint32_t *hash, int promisc, int allmulti)
{
    int i, bcast = 1;
    if (promisc)
        return 1;
    for (i = 0; i < 6; i++)
        if (dst[i] != 0xff)
            bcast = 0;
    if (bcast)
        return 1;
    if (dst[0] & 1) {
        unsigned bit;
        if (allmulti)
            return 1;
        bit = usbnet_hash_bit(chip, dst);
        return (hash[bit >> 5] >> (bit & 31u)) & 1u;
    }
    for (i = 0; i < 6; i++)
        if (dst[i] != own[i])
            return 0;
    return 1;
}
