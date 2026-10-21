#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>

uint32_t eth_test_vlan_filter(struct eqos_dev *eth, uint8_t *buffer,
                              size_t capacity, uint16_t tx_vlan_id)
{
    printf("\n ========== Begin EQOS VLAN ID Filter test ==========\n");
    if (eth == NULL)
        return 0U;

    const uint16_t expected_vid = 100U;
    const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    /* Destination matches source address slot 0: no promiscuous mode needed.
     * Nonzero PCP/DEI exercise ETV's VID-only comparison. 64 bytes exclude FCS. */
    uint32_t length = eth_construct_vlan_frame(buffer, capacity, 64U,
        eth->cfg.mac_addr, eth->cfg.mac_addr, 0x88B5U,
        tx_vlan_id, 5U, 1U, pattern, sizeof(pattern));
    if (length == 0U)
        return 0U;

    eth->cfg.loopback = true;
    eth->cfg.promiscuous = false;
    eth->speed = EQOS_SPEED_100;
    eth->duplex_full = true;

    /* VL=100, ETV=1, VTIM=0, EVLS=0, EVLRXS=1.
     * Also leave hash matching, S-VLAN and double-tag processing disabled. */
    const uint32_t vlan_tag = expected_vid | EQOS_MAC_VLAN_TAG_ETV |
                              EQOS_MAC_VLAN_TAG_EVLRXS;
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG, vlan_tag);
    /* Enable VLAN filtering with normal destination-address filtering.
     * Clear PR/RA and unrelated L3/L4 filters for this isolated test. */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER,
                        EQOS_MAC_PKT_FILTER_VTFE);
    eqos_udelay(100);
    uint32_t actual_tag = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG);
    uint32_t actual_filter = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
    printf("MAC VLAN Tag [0x050] = 0x%08lX, Packet Filter = 0x%08lX\r\n",
           (unsigned long)actual_tag, (unsigned long)actual_filter);
    if (actual_tag != vlan_tag || actual_filter != EQOS_MAC_PKT_FILTER_VTFE) {
        printf("VLAN filter register readback failed\r\n");
        return 0U;
    }
    printf("VLAN filter VID=%u, TX VID=%u, PCP=5, DEI=1: expect %s\r\n",
           (unsigned int)expected_vid, (unsigned int)tx_vlan_id,
           tx_vlan_id == expected_vid ? "RX with tag intact" : "no RX (VLAN mismatch)");
    printf("Press USER to transmit in MAC loopback.\r\n");
    return length;
}
