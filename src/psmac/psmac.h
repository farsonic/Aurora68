/* psmac: Macintosh host layer for the Emu68 fork (PiStorm-SE). MPL-2.0. */
#ifndef PSMAC_H
#define PSMAC_H
#include <stdint.h>

/* Bridge protocol (docs/05-bridge-protocol.md) */
#define PSMAC_OP_READ      1
#define PSMAC_OP_WRITE     2
#define PSMAC_OP_POLL      3
#define PSMAC_OP_RESET_ACK 4
#define PSMAC_FLAG_POSTED  1

/* IIci physical map handled locally vs forwarded to the bridge */
#define PSMAC_FB_SIZE      0x00080000u   /* start of bank A doubles as RBV frame buffer: write-through */
#define PSMAC_ROM_BASE     0x40800000u
#define PSMAC_ROM_MIRROR   0x40000000u
#define PSMAC_ROM_SIZE     0x00080000u
#define PSMAC_IO_BASE      0x50000000u
#define PSMAC_IO_END       0x51000000u
#define PSMAC_SLOT_BASE    0xF0000000u

struct psmac_reply { uint8_t status, ipl, flags; uint32_t data; };

void     psmac_setup_memory(void *rom, uint32_t rom_size);
int      psmac_bus_write(uint64_t value, uint64_t value2, int size, uint64_t far);
int      psmac_bus_read(uint64_t *value, uint64_t *value2, int size, uint64_t far);
uint32_t psmac_io_read(uint32_t addr, int size);
void     psmac_io_write(uint32_t addr, int size, uint32_t data, int posted);
void     psmac_service_core(void);     /* runs forever on the service core */
void     psmac_pmove(uint32_t ea, uint32_t ext);
uint32_t psmac_ram_size(void);

#endif
