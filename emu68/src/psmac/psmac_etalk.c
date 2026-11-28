// SPDX-License-Identifier: MPL-2.0
//
// psmac_etalk.c - the A/UX network card: a NuBus Ethernet card with a DP8390 and shared buffer memory, as
// A/UX's own ae6 driver expects one, with the Pi's network board behind it. mac.auxnet=on puts it in a slot.
//
// A/UX does not open Mac OS slot drivers, so the Pi's .ENET cards in slot 9 are nothing to it. Its kernel is
// linked with ae6 when the Slot Manager shows a card with board ID 8, and ae6 then drives a DP8390 at fixed
// places in that slot. This file is that card: the declaration ROM (psmac_etalk_declrom.h), the buffer memory,
// the chip's registers, its slot interrupt, and the frames moved to and from a unit of the network board
// (alice_net.h), which this file uses as the Mac's own Ethernet driver does. docs/24-aux-network.md has the
// story and what the driver was seen to need.

#include <stdint.h>
#include "support.h"
#include "mmu.h"
#include "psmac.h"
#include "../alice/alice_net.h"
#include "psmac_etalk_declrom.h"

// The card in its slot's space, $Fs000000. The offsets are the ones ae6 computes from the slot number.
#define ETALK_RAM_OFF 0x000D0000u       // buffer memory: the DP8390's page 0 is its first byte
#define ETALK_RAM_SIZE 0x00010000u      // 64 KiB with no alias inside: ae6 then takes pages 6 to $FE as its ring
#define ETALK_REG_OFF 0x000E0000u       // the DP8390: register n at +$3C - 4n, one byte on the high lane
#define ETALK_REG_SIZE 0x00010000u
#define ETALK_ROM_OFF 0x00FFF000u       // the declaration ROM's page, its format block at the slot's top
#define ETALK_ROM_SIZE 0x00001000u

// DP8390 bits (National Semiconductor's data sheet names)
#define CR_STP 0x01u
#define CR_STA 0x02u
#define CR_TXP 0x04u
#define ISR_PRX 0x01u
#define ISR_PTX 0x02u
#define ISR_RST 0x80u
#define RCR_AB 0x04u
#define RCR_AM 0x08u
#define RCR_PRO 0x10u
#define RCR_MON 0x20u
#define TCR_LOOP 0x06u
#define RSR_PRX 0x01u
#define RSR_PHY 0x20u
#define TSR_PTX 0x01u

#define FRAME_MIN 60u                   // without the checksum: a shorter frame is padded, as the wire would have it
#define FRAME_MAX 1514u

struct EtalkChip {
    uint8_t cr;                         // the page and remote-DMA bits as written; STA, STP and TXP are kept apart
    uint8_t running;
    uint8_t tx_waiting;                 // TXP given while the board's transmit ring was full: the poll finishes it
    volatile uint8_t isr, imr;
    uint8_t dcr, tcr, rcr, tsr, rsr;
    uint8_t pstart, pstop, bnry, curr, tpsr;
    uint16_t tbcr, rbcr, rsar;
    uint8_t par[6], mar[8];
};

static struct {
    int slot;                           // 0: no card; -1: mac.auxnet not read yet
    uint32_t base;                      // $Fs000000
    uintptr_t ram_pi, rom_pi;           // the Pi's physical addresses of the two pieces, 0 when they cannot be mapped
    volatile uint8_t lock;
    struct EtalkChip chip;
    volatile int unit;                  // the network board's unit the driver's frames go through, -1: none yet
    int stage;                          // the unit's hand-over: 0 not asked, 1 START sent, 2 RXON sent, 3 frames come
    uint32_t filter;                    // the AN_FILT_ bits last sent
    uint8_t mac[6];                     // the address in the ROM's cell
    uint32_t trace_left;                // mac.auxnettrace=N
    uint32_t tx, rx, rx_filtered, rx_held, tx_lost, starts;
    uint32_t said_tx, said_rx;          // the counts at the last report
    int first_tx_said, first_rx_said;
    uint64_t report_at;
} et = { .slot = -1, .unit = -1 };

// the card's memory: the buffer first, then the ROM's page (the kernel's own data, mapped a second time for the 68k)
static uint8_t etalk_mem[ETALK_RAM_SIZE + ETALK_ROM_SIZE] __attribute__((aligned(4096)));
#define ETALK_RAM (etalk_mem)
#define ETALK_ROM (etalk_mem + ETALK_RAM_SIZE)
#define ETALK_MAC_CELL (ETALK_ROM + ETALK_ROM_SIZE - sizeof(psmac_etalk_declrom) - PSMAC_ETALK_MAC_BELOW)

extern const char *psmac_option(const char *key);
extern volatile uint8_t *alice_net_board(void);
extern int psmac_board_se, psmac_board_solo, psmac_hdmi_mode;
extern uint32_t psmac_videocard_size(void);
#ifdef PSMAC_PC
extern uint32_t psmac_pccard_size(void);
#endif
#ifdef PSMAC_A2
extern uint32_t psmac_a2card_size(void);
#endif

#ifdef __aarch64__
#define ETALK_BARRIER() __asm__ volatile("dmb ish" ::: "memory")
#define ETALK_YIELD() __asm__ volatile("yield")

static inline uint64_t etalk_now(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v));
    return v;
}

static inline uint64_t etalk_hz(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v));
    return v;
}
#else
// the host's test of the chip (tools/aux/etalk_test.sh): one thread, no clock
#define ETALK_BARRIER() ((void)0)
#define ETALK_YIELD() ((void)0)

static inline uint64_t etalk_now(void)
{
    return 0;
}

static inline uint64_t etalk_hz(void)
{
    return 1;
}
#endif

static inline void etalk_lock(void)
{
    while (__atomic_test_and_set(&et.lock, __ATOMIC_ACQUIRE)) {
        ETALK_YIELD();
    }
}

static inline void etalk_unlock(void)
{
    __atomic_clear(&et.lock, __ATOMIC_RELEASE);
}

// Is another card of the Pi's in this slot?
static int etalk_slot_taken(int slot)
{
    switch (slot) {
    case 0xA:
        return psmac_board_se && !psmac_board_solo && psmac_hdmi_mode == 1;
    case 0xC:
        return psmac_videocard_size() != 0;
#ifdef PSMAC_PC
    case 0xD:
        return psmac_pccard_size() != 0;
#endif
#ifdef PSMAC_A2
    case 0xE:
        return psmac_a2card_size() != 0;
#endif
    default:
        return 0;
    }
}

// mac.auxnet=on|off|a|c|d|e, read once: the first caller is the memory map's set-up, on the boot core.
static int etalk_slot(void)
{
    if (et.slot >= 0) {
        return et.slot;
    }
    const char *o = psmac_option("mac.auxnet");
    int slot = 0;
    if (o && (o[0] | 32) == 'o' && (o[1] | 32) == 'n') {
        // The first free of D, A, C, E. D first: the owner's A/UX was linked with the card there. E last:
        // A/UX's autoconfig asks the Slot Manager about slots 9 to D itself (docs/24 sections 2 and 4).
        static const uint8_t order[4] = { 0xD, 0xA, 0xC, 0xE };
        slot = -2;
        for (int i = 0; i < 4 && slot < 0; i++) {
            if (!etalk_slot_taken(order[i])) {
                slot = order[i];
            }
        }
    } else if (o && o[0] && !o[1] && ((o[0] | 32) == 'a' || ((o[0] | 32) >= 'c' && (o[0] | 32) <= 'e'))) {
        slot = (o[0] | 32) - 'a' + 10;
        if (etalk_slot_taken(slot)) {
            slot = -2;
        }
    } else if (o && o[0] && !((o[0] | 32) == 'o' && (o[1] | 32) == 'f') && o[0] != '0') {
        kprintf("[AUXNET] mac.auxnet=%s is not on, off or a slot (a, c, d, e): no A/UX network card\n", o);
    }
    if (slot == -2) {
        kprintf("[AUXNET] mac.auxnet=%s: no free slot (another of the Pi's cards is there): no A/UX network card\n", o);
        slot = 0;
    }
    if (slot && !psmac_board_se) {
        // the IIci board's slot interrupt register is the motherboard's own (psmac_bus.c): not done there yet
        kprintf("[AUXNET] mac.auxnet: the A/UX network card is for the SE, Plus and solo boards: none on this one\n");
        slot = 0;
    }
    if (slot) {
        uintptr_t first = mmu_virt2phys((uintptr_t)etalk_mem);
        uintptr_t last = mmu_virt2phys((uintptr_t)etalk_mem + sizeof(etalk_mem) - 4096);
        if (first && last == first + sizeof(etalk_mem) - 4096) {
            et.ram_pi = first;
            et.rom_pi = first + ETALK_RAM_SIZE;
        }
        et.base = 0xF0000000u | ((uint32_t)slot << 24);
    }
    et.slot = slot;
    return slot;
}

// The card decodes A0-A19 for its buffer and its chip (28 November 2026, docs/27): the same megabyte answers in each of
// the slot's sixteen, the ROM's page aside. A/UX's ae6 addresses them from the slot's base ($Fs0D0000); Linux's mac8390
// from base | slot << 20 ($FsSD0000, "how the Apple driver does it"), which reached nothing before and the driver said
// "Don't know how to access card memory".
static uint32_t etalk_decode(uint32_t off)
{
    if (off >= ETALK_ROM_OFF) {
        return off;
    }
    return off & 0x000fffffu;
}

// psmac_mmu.c: the Pi's memory behind a 68k physical page of the card (its buffer or its ROM), 0 for any other
uintptr_t psmac_etalk_phys(uint32_t p)
{
    if ((p >> 28) != 0xF || !etalk_slot() || (p & 0xff000000u) != et.base || !et.ram_pi) {
        return 0;
    }
    uint32_t off = etalk_decode(p & 0x00ffffffu);
    if (off >= ETALK_RAM_OFF && off < ETALK_RAM_OFF + ETALK_RAM_SIZE) {
        return et.ram_pi + (off - ETALK_RAM_OFF);
    }
    if (off >= ETALK_ROM_OFF) {
        return et.rom_pi + (off - ETALK_ROM_OFF);
    }
    return 0;
}

// psmac_mmu.c: the ROM's page is read-only for the 68k
int psmac_etalk_ro(uint32_t p)
{
    return (p >> 28) == 0xF && et.slot > 0 && (p & 0xff000000u) == et.base && (p & 0x00ffffffu) >= ETALK_ROM_OFF;
}

// psmac_board_se.c: the card's bit in the RBV's slot register (bit 0 is slot 9) while its interrupt is asserted
unsigned psmac_etalk_slot_irq(void)
{
    if (et.slot <= 0 || !(et.chip.isr & et.chip.imr & 0x7f)) {
        return 0;
    }
    return 1u << (et.slot - 9);
}

static void etalk_chip_reset(void)
{
    struct EtalkChip *c = &et.chip;
    for (unsigned i = 0; i < sizeof(*c); i++) {
        ((volatile uint8_t *)c)[i] = 0;
    }
    c->cr = 0x20;
    c->isr = ISR_RST;
}

static volatile struct an_unit *etalk_unit(volatile uint8_t *board, int u)
{
    return (volatile struct an_unit *)(board + AN_OFF_UNIT(u));
}

static int etalk_mac_set(const volatile uint8_t *m)
{
    return (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0;
}

// The unit A/UX should use now: the wired port with its link up, else the WiFi once the chip has given its
// address, else the wired port without a cable. -1: the board has no address to give yet.
static int etalk_pick(volatile uint8_t *board)
{
    volatile struct an_unit *wifi = etalk_unit(board, 0), *wired = etalk_unit(board, 1);
    if (wired->kind == AN_KIND_ETHER && wired->link && etalk_mac_set(wired->mac)) {
        return 1;
    }
    if (wifi->kind == AN_KIND_WIFI && etalk_mac_set(wifi->mac)) {
        return 0;
    }
    if (wired->kind == AN_KIND_ETHER && etalk_mac_set(wired->mac)) {
        return 1;
    }
    return -1;
}

// The unit for a driver that has just started the chip: the one whose address it programmed, which it read
// from the ROM's cell; failing that the one etalk_pick names. Called under the lock, from the 68k's core.
static void etalk_bind(void)
{
    volatile uint8_t *board = alice_net_board();
    et.unit = -1;
    if (!board) {
        return;
    }
    int pick = etalk_pick(board);
    for (int k = 0; k < 2; k++) {
        int u = k ? !pick : pick;
        if (u < 0 || !etalk_unit(board, u)->kind) {
            continue;
        }
        volatile struct an_unit *un = etalk_unit(board, u);
        int same = 1;
        for (int i = 0; i < 6; i++) {
            same &= un->mac[i] == et.chip.par[i];
        }
        if (same) {
            et.unit = u;
            return;
        }
    }
    et.unit = pick;
    if (pick >= 0) {
        kprintf("[AUXNET] the driver's address %02x:%02x:%02x:%02x:%02x:%02x is not one of the network board's: "
                "its frames go out through the %s all the same\n", et.chip.par[0], et.chip.par[1], et.chip.par[2],
                et.chip.par[3], et.chip.par[4], et.chip.par[5], pick ? "wired port" : "WiFi");
    }
}

// One frame from the buffer memory into the unit's transmit ring. 0: the ring is full, try again later.
static int etalk_send(void)
{
    struct EtalkChip *c = &et.chip;
    volatile uint8_t *board = alice_net_board();
    uint32_t len = c->tbcr;
    if (len > FRAME_MAX) {
        len = FRAME_MAX;
    }
    if (!board || et.unit < 0 || len < 14 || (c->tcr & TCR_LOOP)) {
        et.tx_lost++;
        return 1;
    }
    volatile struct an_unit *u = etalk_unit(board, et.unit);
    if (!u->link) {
        et.tx_lost++;
        return 1;
    }
    uint32_t head = u->tx_head;
    if (head - u->tx_tail >= AN_TX_SLOTS) {
        return 0;
    }
    volatile struct an_slot *s =
        (volatile struct an_slot *)(board + AN_OFF_TX(et.unit) + (head % AN_TX_SLOTS) * AN_SLOT_BYTES);
    uint32_t from = (uint32_t)c->tpsr << 8;
    for (uint32_t i = 0; i < len; i++) {
        s->data[i] = ETALK_RAM[(from + i) & (ETALK_RAM_SIZE - 1)];
    }
    s->len = len;
    s->flags = 0;
    ETALK_BARRIER();
    u->tx_head = head + 1;
    et.tx++;
    return 1;
}

static void etalk_sent(void)
{
    struct EtalkChip *c = &et.chip;
    c->tx_waiting = 0;
    c->tsr = TSR_PTX;
    c->isr |= ISR_PTX;
}

static void etalk_cr_write(uint8_t v)
{
    struct EtalkChip *c = &et.chip;
    c->cr = v & 0xf8;
    if (v & CR_STP) {
        // the chip's software reset: off line at once, and the reset flag for the driver's wait
        if (c->running && et.starts <= 8) {
            kprintf("[AUXNET] the driver stopped the chip (%u sent, %u received)\n", et.tx, et.rx);
        }
        c->running = 0;
        c->tx_waiting = 0;
        c->isr |= ISR_RST;
        return;
    }
    // a command with neither STA nor STP leaves the chip as it is: ae6 selects register pages that way
    if ((v & CR_STA) && !c->running) {
        c->running = 1;
        c->isr &= (uint8_t)~ISR_RST;
        etalk_bind();
        et.stage = 0;
        if (et.starts++ < 8) {
            kprintf("[AUXNET] the driver started the chip: address %02x:%02x:%02x:%02x:%02x:%02x, ring pages %02x to "
                    "%02x, receive configuration %02x; frames through %s\n", c->par[0], c->par[1], c->par[2], c->par[3],
                    c->par[4], c->par[5], c->pstart, (uint8_t)(c->pstop - 1), c->rcr,
                    et.unit < 0 ? "nothing: the network board has no unit yet" : et.unit ? "the wired port"
                                                                                           : "the WiFi");
        }
    }
    if ((v & CR_TXP) && c->running && !c->tx_waiting) {
        if (etalk_send()) {
            etalk_sent();
        } else {
            c->tx_waiting = 1;
        }
    }
}

static uint8_t etalk_reg_read(unsigned r)
{
    struct EtalkChip *c = &et.chip;
    unsigned page = c->cr >> 6;
    if (r == 0) {
        return (uint8_t)(c->cr | (c->running ? CR_STA : CR_STP) | (c->tx_waiting ? CR_TXP : 0));
    }
    if (page == 1) {
        return r <= 6 ? c->par[r - 1] : r == 7 ? c->curr : c->mar[r - 8];
    }
    if (page == 2) {
        switch (r) {
        case 1: return c->pstart;
        case 2: return c->pstop;
        case 4: return c->tpsr;
        case 12: return c->rcr;
        case 13: return c->tcr;
        case 14: return c->dcr;
        case 15: return c->imr;
        default: return 0;
        }
    }
    switch (r) {
    case 3: return c->bnry;
    case 4: return c->tsr;
    case 7: return c->isr;
    case 8: return (uint8_t)c->rsar;
    case 9: return (uint8_t)(c->rsar >> 8);
    case 12: return c->rsr;
    default: return 0;               // CLDA, the collision count, the FIFO and the three tally counters
    }
}

static void etalk_reg_write(unsigned r, uint8_t v)
{
    struct EtalkChip *c = &et.chip;
    unsigned page = c->cr >> 6;
    if (r == 0) {
        etalk_cr_write(v);
        return;
    }
    if (page == 1) {
        if (r <= 6) {
            c->par[r - 1] = v;
        } else if (r == 7) {
            c->curr = v;
        } else {
            c->mar[r - 8] = v;
        }
        return;
    }
    if (page != 0) {
        return;
    }
    switch (r) {
    case 1: c->pstart = v; break;
    case 2: c->pstop = v; break;
    case 3: c->bnry = v; break;
    case 4: c->tpsr = v; break;
    case 5: c->tbcr = (uint16_t)((c->tbcr & 0xff00) | v); break;
    case 6: c->tbcr = (uint16_t)((c->tbcr & 0x00ff) | (v << 8)); break;
    case 7: c->isr &= (uint8_t)~(v & 0x7f); break;       // a 1 clears its flag; the reset flag is the chip's own
    case 8: c->rsar = (uint16_t)((c->rsar & 0xff00) | v); break;
    case 9: c->rsar = (uint16_t)((c->rsar & 0x00ff) | (v << 8)); break;
    case 10: c->rbcr = (uint16_t)((c->rbcr & 0xff00) | v); break;
    case 11: c->rbcr = (uint16_t)((c->rbcr & 0x00ff) | (v << 8)); break;
    case 12: c->rcr = v; break;
    case 13: c->tcr = v; break;
    case 14: c->dcr = v; break;
    case 15: c->imr = v & 0x7f; break;
    default: break;
    }
}

static const char *etalk_reg_name(unsigned r, unsigned page, int write)
{
    static const char *const p0r[16] = { "CR", "CLDA0", "CLDA1", "BNRY", "TSR", "NCR", "FIFO", "ISR",
                                          "CRDA0", "CRDA1", "-", "-", "RSR", "CNTR0", "CNTR1", "CNTR2" };
    static const char *const p0w[16] = { "CR", "PSTART", "PSTOP", "BNRY", "TPSR", "TBCR0", "TBCR1", "ISR",
                                          "RSAR0", "RSAR1", "RBCR0", "RBCR1", "RCR", "TCR", "DCR", "IMR" };
    static const char *const p1[16] = { "CR", "PAR0", "PAR1", "PAR2", "PAR3", "PAR4", "PAR5", "CURR",
                                         "MAR0", "MAR1", "MAR2", "MAR3", "MAR4", "MAR5", "MAR6", "MAR7" };
    return page == 1 ? p1[r] : write ? p0w[r] : p0r[r];
}

// psmac_bus.c: one access by the 68k that was not mapped memory. 1: it was the card's, answered here.
int psmac_etalk_access(uint32_t addr, int size, int write, uint32_t data, uint32_t *val)
{
    if ((addr >> 28) != 0xF || et.slot <= 0 || (addr & 0xff000000u) != et.base) {
        return 0;
    }
    uint32_t off = etalk_decode(addr & 0x00ffffffu), v = 0;
    if (off >= ETALK_REG_OFF && off < ETALK_REG_OFF + ETALK_REG_SIZE) {
        etalk_lock();
        for (int i = 0; i < size; i++) {
            uint32_t a = off + (uint32_t)i;
            uint8_t b = (uint8_t)(data >> (8 * (size - 1 - i)));
            unsigned r = 15 - ((a >> 2) & 15), page = et.chip.cr >> 6;
            if (a & 3) {
                b = 0xff;                       // the chip is on the high byte lane only
            } else if (write) {
                etalk_reg_write(r, b);
            } else {
                b = etalk_reg_read(r);
            }
            if (!(a & 3) && et.trace_left) {
                et.trace_left--;
                kprintf("[AUXNET] %s %s%s %02x\n", write ? "W" : "R", etalk_reg_name(r, page, write),
                        page == 2 ? " (page 2)" : "", b);
            }
            v = v << 8 | b;
        }
        etalk_unlock();
    } else if (off >= ETALK_RAM_OFF && off + (uint32_t)size <= ETALK_RAM_OFF + ETALK_RAM_SIZE) {
        // only when the page is not mapped for the 68k (the stock path with translation off)
        uint8_t *p = ETALK_RAM + (off - ETALK_RAM_OFF);
        for (int i = 0; i < size; i++) {
            if (write) {
                p[i] = (uint8_t)(data >> (8 * (size - 1 - i)));
            }
            v = v << 8 | p[i];
        }
    } else if (off >= ETALK_ROM_OFF && !write) {
        for (int i = 0; i < size; i++) {
            v = v << 8 | (off + (uint32_t)i < 0x01000000u ? ETALK_ROM[off + (uint32_t)i - ETALK_ROM_OFF] : 0xff);
        }
    } else if (off >= ETALK_ROM_OFF) {
        v = 0;
    } else {
        return 0;                               // the rest of the slot is empty, as the board has it
    }
    if (!write) {
        *val = v;
    }
    return 1;
}

// the DP8390's group address filter: the top six bits of the address's CRC name a bit of MAR0-7
static int etalk_group_wanted(const uint8_t *dst)
{
    uint32_t crc = 0xffffffffu;
    for (int i = 0; i < 6; i++) {
        uint8_t b = dst[i];
        for (int k = 0; k < 8; k++) {
            uint32_t carry = (crc >> 31) ^ (b & 1);
            crc <<= 1;
            b >>= 1;
            if (carry) {
                crc = (crc ^ 0x04c11db6u) | carry;
            }
        }
    }
    return (et.chip.mar[crc >> 29] >> ((crc >> 26) & 7)) & 1;
}

static int etalk_wanted(const uint8_t *dst)
{
    const struct EtalkChip *c = &et.chip;
    if (dst[0] & 1) {
        int all = (dst[0] & dst[1] & dst[2] & dst[3] & dst[4] & dst[5]) == 0xff;
        return all ? (c->rcr & RCR_AB) != 0 : (c->rcr & RCR_AM) && etalk_group_wanted(dst);
    }
    if (c->rcr & RCR_PRO) {
        return 1;
    }
    for (int i = 0; i < 6; i++) {
        if (dst[i] != c->par[i]) {
            return 0;
        }
    }
    return 1;
}

// One received frame into the chip's ring, as the DP8390 stores it: four bytes (status, next page, length low
// and high, the length counting these four and the frame's checksum), then the frame, wrapping from PSTOP to
// PSTART. 0: no room before the boundary page; the frame then waits in the board's ring, nothing is overwritten.
static int etalk_store(const volatile uint8_t *frame, uint32_t len, int group)
{
    struct EtalkChip *c = &et.chip;
    uint32_t padded = len < FRAME_MIN ? FRAME_MIN : len;
    uint32_t count = padded + 8, pages = (count + 255) >> 8;
    if (c->pstop <= c->pstart || c->curr < c->pstart || c->curr >= c->pstop) {
        return 1;                               // no ring to speak of: the frame is lost, as on the chip
    }
    uint32_t ring = (uint32_t)(c->pstop - c->pstart);
    uint32_t room = c->bnry >= c->pstart && c->bnry < c->pstop ? (c->bnry + ring - c->curr) % ring : ring - 1;
    if (pages >= room) {
        return 0;
    }
    uint32_t next = c->curr + pages;
    if (next >= c->pstop) {
        next -= ring;
    }
    uint32_t at = (uint32_t)c->curr << 8, stop = (uint32_t)c->pstop << 8, start = (uint32_t)c->pstart << 8;
    uint8_t head[4] = { (uint8_t)(RSR_PRX | (group ? RSR_PHY : 0)), (uint8_t)next, (uint8_t)count,
                        (uint8_t)(count >> 8) };
    for (uint32_t i = 0; i < count; i++) {
        uint8_t b = i < 4 ? head[i] : i - 4 < len ? frame[i - 4] : 0;
        ETALK_RAM[at & (ETALK_RAM_SIZE - 1)] = b;
        if (++at == stop) {
            at = start;
        }
    }
    c->rsr = head[0];
    ETALK_BARRIER();
    c->curr = (uint8_t)next;
    c->isr |= ISR_PRX;
    et.rx++;
    return 1;
}

static int etalk_command(volatile struct an_unit *u, uint32_t cmd, uint32_t arg)
{
    if (u->cmd_done != u->cmd_seq) {
        return 0;                               // the board is still at the last one
    }
    u->arg[0] = (uint8_t)(arg >> 24);
    u->arg[1] = (uint8_t)(arg >> 16);
    u->arg[2] = (uint8_t)(arg >> 8);
    u->arg[3] = (uint8_t)arg;
    u->cmd = cmd;
    ETALK_BARRIER();
    u->cmd_seq = u->cmd_seq + 1;
    return 1;
}

// the address for the ROM's cell, whenever the board's changes while no driver runs the chip
static void etalk_address(volatile uint8_t *board)
{
    int pick = etalk_pick(board);
    if (pick < 0 || et.chip.running) {
        return;
    }
    volatile struct an_unit *u = etalk_unit(board, pick);
    int same = 1;
    for (int i = 0; i < 6; i++) {
        same &= u->mac[i] == et.mac[i];
    }
    if (same) {
        return;
    }
    uint8_t *cell = ETALK_MAC_CELL;
    for (int i = 0; i < 6; i++) {
        et.mac[i] = u->mac[i];
        cell[i] = et.mac[i];
    }
    arm_flush_cache((uintptr_t)cell & ~63ull, 64);
    kprintf("[AUXNET] Ethernet address %02x:%02x:%02x:%02x:%02x:%02x, the network board's %s, in the card's ROM\n",
            et.mac[0], et.mac[1], et.mac[2], et.mac[3], et.mac[4], et.mac[5], pick ? "wired port" : "WiFi");
}

// psmac_linux.c, as the 68k is released into Linux: the unit the card would give its frames to now (-1: no address
// yet, -2: no card), *settled when that is the one to stay with (the wired port with its link, or the WiFi), and its
// address into the ROM's cell at once. Linux's mac8390 reads the cell once, at its probe, seconds after the start,
// where A/UX reads it minutes after the Mac's ROM has run: with the WiFi's chip not up yet the cell held the wired
// port's address, which the access point drops (docs/27).
int psmac_etalk_settle(int *settled, uint8_t mac[6])
{
    *settled = 0;
    if (et.slot <= 0) {
        return -2;
    }
    volatile uint8_t *board = alice_net_board();
    if (!board) {
        return -1;
    }
    int pick = etalk_pick(board);
    etalk_address(board);
    int same = pick >= 0;
    for (int i = 0; i < 6; i++) {
        mac[i] = et.mac[i];
        same &= pick >= 0 && etalk_unit(board, pick)->mac[i] == et.mac[i];
    }
    if (same && (pick == 0 || (pick == 1 && etalk_unit(board, 1)->link))) {
        *settled = 1;
    }
    return same ? pick : -1;
}

// alice_psmac.c, core 3's service loop, after the network board's own turn
void psmac_etalk_poll(void)
{
    if (et.slot <= 0) {
        return;
    }
    volatile uint8_t *board = alice_net_board();
    if (!board) {
        return;
    }
    struct EtalkChip *c = &et.chip;
    etalk_address(board);
    etalk_lock();
    int unit = et.unit;
    volatile struct an_unit *u = unit >= 0 ? etalk_unit(board, unit) : 0;
    if (c->tx_waiting && etalk_send()) {
        etalk_sent();
    }
    if (!c->running) {
        if (et.stage && u && etalk_command(u, AN_CMD_RXOFF, 0)) {
            et.stage = 0;
        }
    } else if (u) {
        // the board's unit, taken over as the Mac's Ethernet driver takes it: START, RXON, then the filter
        uint32_t filter = ((c->rcr & RCR_PRO) ? AN_FILT_PROMISC : 0) | ((c->rcr & RCR_AM) ? AN_FILT_ALLMULTI : 0);
        if (et.stage == 0 && (u->admin_off || etalk_command(u, AN_CMD_START, 0))) {
            et.stage = 1;
        } else if (et.stage == 1 && u->cmd_done == u->cmd_seq) {
            u->rx_tail = u->rx_head;            // whatever came before the driver existed is not its to take
            if (etalk_command(u, AN_CMD_RXON, 0)) {
                et.stage = 2;
            }
        } else if (et.stage == 2 && etalk_command(u, AN_CMD_FILTER, filter)) {
            et.stage = 3;
            et.filter = filter;
            kprintf("[AUXNET] the network board's %s gives its frames to A/UX's driver now\n",
                    unit ? "wired port" : "WiFi");
        } else if (et.stage == 3 && !u->rx_on && u->cmd_done == u->cmd_seq) {
            et.stage = 1;                       // the board began a new session (a 68k RESET): ask again
        } else if (et.stage == 3 && filter != et.filter && etalk_command(u, AN_CMD_FILTER, filter)) {
            et.filter = filter;
        }
        while (et.stage == 3 && u->rx_tail != u->rx_head && !(c->rcr & RCR_MON) && !(c->tcr & TCR_LOOP)) {
            uint32_t tail = u->rx_tail;
            volatile struct an_slot *s =
                (volatile struct an_slot *)(board + AN_OFF_RX(unit) + (tail % AN_RX_SLOTS) * AN_SLOT_BYTES);
            uint32_t len = s->len;
            uint8_t dst[6];
            for (int i = 0; i < 6; i++) {
                dst[i] = s->data[i];
            }
            if (len < 14 || len > FRAME_MAX || !etalk_wanted(dst)) {
                et.rx_filtered++;
            } else if (!etalk_store(s->data, len, dst[0] & 1)) {
                et.rx_held++;
                break;
            }
            ETALK_BARRIER();
            u->rx_tail = tail + 1;
        }
    }
    uint32_t tx = et.tx, rx = et.rx;
    uint8_t isr = c->isr, imr = c->imr, curr = c->curr, bnry = c->bnry;
    int running = c->running;
    etalk_unlock();
    if (tx && !et.first_tx_said) {
        et.first_tx_said = 1;
        kprintf("[AUXNET] the first frame from A/UX's driver went to the network board\n");
    }
    if (rx && !et.first_rx_said) {
        et.first_rx_said = 1;
        kprintf("[AUXNET] the first frame for A/UX's driver is in the card's ring\n");
    }
    uint64_t now = etalk_now();
    if (running && now >= et.report_at) {
        if (tx != et.said_tx || rx != et.said_rx) {
            kprintf("[AUXNET] %u sent (%u lost), %u received (%u not for the driver, %u waits for room); ISR %02x IMR "
                    "%02x, ring at %02x, boundary %02x\n", tx, et.tx_lost, rx, et.rx_filtered, et.rx_held, isr, imr,
                    curr, bnry);
        }
        et.said_tx = tx;
        et.said_rx = rx;
        et.report_at = now + 30 * etalk_hz();
    }
}

// alice_psmac.c: the 68k's RESET, and the hand-over to A/UX's tables: what a card's /RESET does to its chip.
// The interrupt goes with it; the board's unit is given back on the service core's next turn.
void psmac_etalk_reset(void)
{
    if (et.slot <= 0) {
        return;
    }
    etalk_lock();
    int was = et.chip.running;
    etalk_chip_reset();
    etalk_unlock();
    if (was) {
        kprintf("[AUXNET] the chip is reset: stopped, its interrupt off\n");
    }
}

// psmac_bus.c, before the 68k starts: the ROM into its page, the chip in its power-on state
void psmac_etalk_init(void)
{
    if (etalk_slot() <= 0) {
        return;
    }
    const char *o = psmac_option("mac.auxnettrace");
    for (; o && *o >= '0' && *o <= '9'; o++) {
        et.trace_left = et.trace_left * 10 + (uint32_t)(*o - '0');
    }
    for (uint32_t i = 0; i < sizeof(etalk_mem); i++) {
        etalk_mem[i] = 0;
    }
    uint8_t *rom = ETALK_ROM + ETALK_ROM_SIZE - sizeof(psmac_etalk_declrom);
    for (uint32_t i = 0; i < sizeof(psmac_etalk_declrom); i++) {
        rom[i] = psmac_etalk_declrom[i];
    }
    arm_flush_cache((uintptr_t)etalk_mem, sizeof(etalk_mem));
    etalk_chip_reset();
    kprintf("[AUXNET] the A/UX network card in slot $%X: board ID 8, a DP8390 at $%08x, 64 KiB of buffer at $%08x, "
            "ROM %u bytes at the slot's top%s\n", et.slot, et.base + ETALK_REG_OFF, et.base + ETALK_RAM_OFF,
            (uint32_t)sizeof(psmac_etalk_declrom),
            et.ram_pi ? "" : " (its memory is not mapped: every access is emulated)");
    if (et.slot != 0xD && etalk_slot_taken(0xD)) {
        kprintf("[AUXNET] slot $D is the PC card's (pc=on): an A/UX kernel linked with the card in another slot must "
                "be linked again (newconfig)\n");
    }
}
