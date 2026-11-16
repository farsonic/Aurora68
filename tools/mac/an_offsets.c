/* Prints the offsets of the network board's structures (emu68/src/alice/alice_net.h) in the form of
   the equates the Macintosh drivers use, so that `make -C mac check` can compare them with
   mac/enet.s. The first version of enet.s was written against an older Alice68 header and nobody
   noticed until the command box and the rings were addressed 36 bytes off (docs/08, fix 15). */
#include <stdio.h>
#include <stddef.h>
#include "alice_net.h"
#define P(name, f) printf("U_%s\t%u\n", name, (unsigned)offsetof(struct an_unit, f))
int main(void)
{
    P("KIND", kind); P("STATE", state); P("LINK", link); P("MAC", mac); P("RX_PKTS", rx_pkts);
    P("CMD_SEQ", cmd_seq); P("CMD", cmd); P("CMD_DONE", cmd_done); P("CMD_RESULT", cmd_result); P("ARG", arg);
    P("RX_HEAD", rx_head); P("RX_TAIL", rx_tail); P("TX_HEAD", tx_head); P("TX_TAIL", tx_tail); P("RX_ON", rx_on);
    printf("AN_OFF_UNIT1\t%u\nAN_OFF_RX1\t%u\nAN_OFF_TX1\t%u\nAN_HDR_IRQ\t%u\n",
           AN_OFF_UNIT(1), AN_OFF_RX(1), AN_OFF_TX(1), (unsigned)offsetof(struct an_hdr, irq));
    return 0;
}
