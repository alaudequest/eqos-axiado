#include "eth_functions_utils.h"
#include "eth_config_test_case.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>

/* VLAN ID filter test (test ID 4), MAC loopback (LM=1), 100M full duplex.
 * Called by eth_rmii_mac_lb() after mac_start(); every run starts from a MAC reset.
 *   Setup: MAC_VLAN_Tag VL=100, ETV=1 (compare VID only), EVLRXS=1; VTIM=0, EVLS=0
 *          MAC_Packet_Filter = VTFE only (normal DA filter, no PR/RA)
 *   Frame: 64 bytes, DA = SA = MAC0, tag VID = VLAN_FILTER_TX_VID, PCP=5, DEI=1
 *   Expect VID 100: frame received with the tag intact (81 00, VID 100)
 *          other VID: no frame (dropped by the VLAN filter)
 * Returns the failure count, -1 on setup error. */

static uint8_t vlan_tx_frame[64] __attribute__((aligned(32)));
static uint8_t vlan_rx_frame[EQOS_BUF_SIZE];

int eth_test_vlan_filter(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                         struct eqos_desc *rx_desc, uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    printf("\r\n ========== Begin EQOS VLAN ID Filter test ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint16_t expected_vid = 100U;
    const bool expect_rx = (VLAN_FILTER_TX_VID == expected_vid);
    const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};

    /* ---------- 1. Setup: frame, MAC_VLAN_Tag, MAC_Packet_Filter ---------- */
    uint32_t length = eth_construct_vlan_frame(vlan_tx_frame, sizeof(vlan_tx_frame), 64U,
        eth->cfg.mac_addr, eth->cfg.mac_addr, 0x88B5U,
        VLAN_FILTER_TX_VID, 5U, 1U, pattern, sizeof(pattern));
    if (length == 0U)
        return -1;
    const uint32_t vlan_tag = expected_vid | EQOS_MAC_VLAN_TAG_ETV |
                              EQOS_MAC_VLAN_TAG_EVLRXS;
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG, vlan_tag);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER,
                        EQOS_MAC_PKT_FILTER_VTFE);
    eqos_udelay(100);
    uint32_t actual_tag = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG);
    uint32_t actual_filter = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
    printf("MAC_VLAN_Tag = 0x%08lX, MAC_Packet_Filter = 0x%08lX\r\n",
           (unsigned long)actual_tag, (unsigned long)actual_filter);
    if (actual_tag != vlan_tag || actual_filter != EQOS_MAC_PKT_FILTER_VTFE) {
        printf("FAIL: VLAN filter register readback\r\n");
        return -1;
    }
    printf("VLAN filter VID=%u, TX VID=%u, PCP=5, DEI=1: expect %s\r\n",
           (unsigned int)expected_vid, (unsigned int)VLAN_FILTER_TX_VID,
           expect_rx ? "RX with tag intact" : "no RX (VLAN mismatch)");

    /* ---------- 2. Send one frame, receive ---------- */
    const int tx = eth_egress_packet(0, vlan_tx_frame, (int)length);
    const uint32_t tdes3 = eth_pkt_last_tx(0)->tdes3;
    const int rx = eth_ingress_packet(0, vlan_rx_frame, sizeof(vlan_rx_frame));
    printf("TX ret=%d TDES3=0x%08lX, RX ret=%d RDES3=0x%08lX\r\n", tx, (unsigned long)tdes3,
           rx, (unsigned long)eth_pkt_last_rx(0)->rdes3);

    /* ---------- 3. Check ---------- */
    bool pass = (tx == (int)length) && ((tdes3 & EQOS_TDES3_WB_ES) == 0U);
    if (expect_rx) {
        const uint16_t tci = ((uint16_t)vlan_rx_frame[14] << 8) | vlan_rx_frame[15];
        pass = pass && (rx == (int)length) &&
               vlan_rx_frame[12] == 0x81U && vlan_rx_frame[13] == 0x00U &&
               (tci & 0x0FFFU) == expected_vid;
        if (rx >= 16)
            printf("RX tag %02X %02X: VID=%u PCP=%u DEI=%u\r\n",
                   (unsigned int)vlan_rx_frame[12], (unsigned int)vlan_rx_frame[13],
                   (unsigned int)(tci & 0x0FFFU), (unsigned int)(tci >> 13),
                   (unsigned int)((tci >> 12) & 1U));
    } else {
        pass = pass && (rx == ETH_PKT_ETIMEOUT);
    }

    printf("\r\n========== VLAN ID Filter test: %s ==========\r\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
