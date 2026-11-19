/* SPDX-License-Identifier: MPL-2.0
 *
 * usbnet_frame.h - the byte-level half of the Pi 3 family's USB Ethernet chips: how a frame is
 * wrapped for the chip and unwrapped from it, how the multicast hash is made, and what the PHY's
 * registers say about the link. Nothing here touches hardware, so the same file builds into the
 * kernel (psmac_usbnet.c) and into a native test (tools/usbnet_frame_test.c, run by make -C mac
 * check).
 *
 * The chips, from their datasheets (Microchip LAN9512/LAN9514 and LAN7800/LAN7515):
 *   SMSC95XX  LAN9512/9514, the Pi 3B (and 2B): every received frame comes after a 32-bit status
 *             word (length in bits 16-29, the error summary in bit 15), padded to four bytes; every
 *             frame sent goes after two command words (TX_CMD_A: length, first and last segment;
 *             TX_CMD_B: length). The multicast filter is a 64-bit hash: the top six bits of the
 *             Ethernet CRC of the address.
 *   LAN78XX   LAN7800/7515, the Pi 3B+: received frames after RX_CMD_A (length bits 0-13, receive
 *             error bit 22), RX_CMD_B and the 16-bit RX_CMD_C, padded so that the next starts on four
 *             bytes; frames sent after TX_CMD_A (length, bit 22: the chip adds the FCS) and TX_CMD_B.
 *             The multicast hash has 512 bits: the top nine bits of the CRC.
 *   CDC_ECM   a USB Communications Class Ethernet device: frames as they are, no headers. QEMU's
 *             usb-net is one, which is how the testbed exercises the USB bulk path (docs/08 item 49).
 * All words on the wire are little-endian.
 */
#ifndef USBNET_FRAME_H
#define USBNET_FRAME_H
#include <stdint.h>

enum usbnet_chip { USBNET_NONE = 0, USBNET_SMSC95XX, USBNET_LAN78XX, USBNET_CDC_ECM };

#define USBNET_FRAME_MAX   1518u       /* the longest Ethernet frame without its FCS */
#define USBNET_TX_HDR_MAX  8u

/* Ethernet's CRC-32 of the address, most significant bit first (the order the hash filters index by) */
uint32_t usbnet_ether_crc(const uint8_t *data, unsigned len);

/* the hash filter bit an address sets: 0..63 (SMSC95XX: HASHH holds 32..63), 0..511 (LAN78XX) */
unsigned usbnet_hash_bit(enum usbnet_chip chip, const uint8_t mac[6]);

/* the headers before a frame of len bytes; returns their size (0 for CDC_ECM) */
unsigned usbnet_tx_header(enum usbnet_chip chip, uint8_t *hdr, unsigned len);

/* One bulk-in transfer holds one frame or several. usbnet_rx_next walks them: from offset *pos in
   buf (n bytes), the next frame's start and length (without the FCS) in *frame and *flen, and *pos
   moved past it. Returns 1 for a good frame, 0 for one the chip marked bad or that is too short or
   too long (skipped, *pos moved on), -1 when the buffer holds no more. */
int usbnet_rx_next(enum usbnet_chip chip, const uint8_t *buf, unsigned n, unsigned *pos,
                   const uint8_t **frame, unsigned *flen);

/* the link from the PHY's registers: BMSR (1), ADVERTISE (4), LPA (5), and for a gigabit PHY CTRL1000
   (9) and STAT1000 (10), else 0. Speed 10, 100 or 1000; full 1 or 0; 0 if no link. */
struct usbnet_link { unsigned up, speed, full, autoneg_done; };
void usbnet_link_decode(unsigned bmsr, unsigned adv, unsigned lpa, unsigned ctrl1000, unsigned stat1000,
                        struct usbnet_link *l);

/* the frame-filter verdict the chip would give: 1 if a frame to dst passes (own address, broadcast,
   a multicast group whose hash bit is set, or everything when promiscuous); used by the CDC-ECM
   path, whose device filters only coarsely, and by the test to check the hash tables */
int usbnet_filter_pass(enum usbnet_chip chip, const uint8_t dst[6], const uint8_t own[6],
                       const uint32_t *hash, int promisc, int allmulti);

#endif
