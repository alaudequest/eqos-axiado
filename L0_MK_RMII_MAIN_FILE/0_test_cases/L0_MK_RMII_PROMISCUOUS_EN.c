#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>


void eth_test_enable_promiscuous(struct eqos_cfg* eth){
    printf("\n ========== Begin EQOS Enable Promiscuous test ==========\n");
    eth->promiscuous = true;


    /* MAC packet filter PR(Promiscuous) accepts frames irrespective of destination-address
    * matching, useful because the test destination differs from this MAC address.
    * Promiscuous mode is not a bypass for every integrity/error check.
    * Current code only || PR when true at struct eqos_cfg ; false does not explicitly clear an
    * already-set PR bit, so cycle board when change struct eqos_cfg parameter.*/

    uint32_t mac_filter = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
    if (eth->promiscuous == true)
        mac_filter |= EQOS_MAC_PKT_FILTER_PR; //Promiscuous Mode
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, mac_filter);
    eqos_udelay(100);
    uint32_t mac_filter_pr = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER) & EQOS_MAC_PKT_FILTER_PR;
    printf("Promiscuous mode set %s, PR bit = %lu \r\n", mac_filter_pr == 1 ? "done" : "failed",
             (unsigned long)mac_filter_pr);
    if (!mac_filter_pr) printf("Cycle board to set bit again \r \n");
    else printf("Waiting for incoming packets... \r \n");
}
