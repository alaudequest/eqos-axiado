#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>

#define DUT_IP_ADDR 0xC0A80114U  /* 192.168.1.20 */

/* Set ARPEN and MACARPAR before TE/RE. Caller must keep ARPEN in mac_control,
 * because main.c rewrites MAC_CONFIGURATION when starting TX/RX. */
void eth_test_enable_arp_offload(void){
    printf("\r\n ========== Begin ARP offload enable test ==========\r\n");
    uint32_t mac_control = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);
    mac_control |= EQOS_MAC_CFG_ARPEN; //Enable ARP offload
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, mac_control);

    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ARP_ADDRESS, DUT_IP_ADDR);

    /* Verify ARP address */
    uint32_t arp_addr = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_ARP_ADDRESS);

    printf("Expected ARP IP : 0x%08lX\r\n", (unsigned long)DUT_IP_ADDR);

    printf("Readback ARP IP : 0x%08lX\r\n", (unsigned long)arp_addr);

    if (arp_addr != DUT_IP_ADDR)
    {
        printf("FAIL: MACARPAR mismatch!\r\n");
        return;
    }
    mac_control =
        AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);

    printf("MAC_CONFIGURATION = 0x%08lX\r\n", (unsigned long)mac_control);

    printf("ARPEN = %lu\r\n", (unsigned long)((mac_control >> 31) & 1U));

    printf("Waiting for incoming ARP request packets...\r\n");
}
