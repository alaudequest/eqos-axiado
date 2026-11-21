#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* Ixia IxTest8f_Vlan: RX frames tagged with VLAN ID 100 (test ID 39).
 *
 * main.c includes this file AFTER the TX/RX rings are declared, brings the
 * link up with PHY auto-negotiation (MACCR.LM=0) and runs it once after USER;
 * do not add it as a separate build source.
 *
 * The Ixia sends 1000 good frames, then 1000 frames tagged with VLAN ID 100, to
 * the DUT MAC 00:80:E1:00:00:00 (static MAC on the Ixia: the STM32 does not
 * answer ARP). No VLAN filter (MACPFR.VTFE=0 in setup), so all 2000 are
 * accepted (about 2000 delivered).
 * The STM32 empties RX with eth_ingress_packet() and checks the MMC RX
 * counters over the run:
 *   MMCRUPGR (unicast good) = 2000
 * and the last-descriptor write-back of each delivered frame:
 *   RDES3.LT = 3'b100 (VLAN tagged)                  -> 1000 frames
 *   RDES3.RS0V = 1 and RDES0.OVT[11:0] = 100 (VID)   -> 1000 frames
 * MAC_VLAN_Tag.EVLRXS=1 puts the outer tag in RDES0; EVLS=00 keeps the tag in
 * the frame (not stripped). Restored at the end.
 * Frames delivered to software are printed for information only.
 * Run: wait up to VLAN_START_MS for the first frame, then receive for
 * VLAN_WINDOW_MS, then read the counters. */

#define VLAN_NORMAL_FRAMES  1000U
#define VLAN_TEST_FRAMES    1000U
#define VLAN_START_MS       60000U   /* time to start the Ixia script */
#define VLAN_WINDOW_MS      10000U   /* receive window after the first frame */
#define VLAN_TEST_VID       100U
#define VLAN_VID_MASK       0x0FFFU  /*RDES0 [11:0] is Outer VLAN VID, [12] DEI, [15:13] PCP*/
#define VLAN_RDES3_RS0V     (1u << 25)   /* RDES0 valid */
#define VLAN_RDES3_LT_VLAN  (4u << 16)   /* LT = 3'b100: type packet with VLAN tag */

static uint8_t vlan_rx_frame[EQOS_BUF_SIZE];

int eth_test_ixia_rx_vlan(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                          struct eqos_desc *rx_desc,
                          uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    unsigned int failures = 0U;
    uint32_t delivered = 0U;
    uint32_t tagged = 0U;       /* LT = VLAN */
    uint32_t vid_match = 0U;    /* RS0V=1 and RDES0.OVT VID = VLAN_TEST_VID */

    printf("\r\n ========== Begin Ixia IxTest8f_Vlan (receive) ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;

    /* ---------- 1. Setup: bind CH0 rings, no promiscuous mode, no VLAN filter, tag in RX status ---------- */
    if (eth_pkt_ring_init(0, tx_desc, EQOS_TX_DESC_COUNT,
                          rx_desc, EQOS_RX_DESC_COUNT, rx_buf) != EQOS_OK) {
        printf("FAIL: eth_pkt_ring_init\r\n");
        return -1;
    }
    const uint32_t pfr_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, EQOS_MAC_PKT_FILTER_PR | EQOS_MAC_PKT_FILTER_VTFE, 0U)) {
        printf("FAIL: MACPFR setup readback\r\n");
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
        return -1;
    }
    const uint32_t vtr_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG);
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG,
                        EQOS_MAC_VLAN_TAG_EVLS_MASK, EQOS_MAC_VLAN_TAG_EVLRXS)) {
        printf("FAIL: MAC_VLAN_Tag setup readback\r\n");
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG, vtr_saved);
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
        return -1;
    }

    /* ---------- 2. Counter start values (MMC: snapshot; DMA missed: read clears) ---------- */
    const uint32_t uc0 = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_UNICAST_GOOD);
    const uint32_t crc0 = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_CRC_ERROR_PACKETS);
    const uint32_t align0 = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_ALIGN_ERROR_PACKETS);
    (void)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_MISSED_FRAME_COUNT);

    /* ---------- 3. Wait for the first frame ---------- */
    printf("Start IxTest8f_Vlan on the Ixia now (waiting %lu s).\r\n",
           (unsigned long)(VLAN_START_MS / 1000U));
    eth_pkt_set_timeout(ETH_PKT_DEFAULT_TIMEOUT_US, VLAN_START_MS * 1000U);
    if (eth_ingress_packet(0, vlan_rx_frame, sizeof(vlan_rx_frame)) < 0) {
        printf("FAIL: no frame received\r\n");
        ++failures;
    } else {
        /* ---------- 4. Receive for VLAN_WINDOW_MS (no printf) ---------- */
        delivered = 1U;
        if ((eth_pkt_last_rx(0)->rdes3 & EQOS_RDES3_WB_LT_MASK) == VLAN_RDES3_LT_VLAN) {
            ++tagged;
            if ((eth_pkt_last_rx(0)->rdes3 & VLAN_RDES3_RS0V) != 0U &&
                (eth_pkt_last_rx(0)->rdes0 & 0x0FFFU) == VLAN_TEST_VID)
                ++vid_match;
        }
        eth_pkt_set_timeout(ETH_PKT_DEFAULT_TIMEOUT_US, 0U);    /* RX: check once */
        const uint32_t t0 = HAL_GetTick();
        while (HAL_GetTick() - t0 < VLAN_WINDOW_MS) {
            if (eth_ingress_packet(0, vlan_rx_frame, sizeof(vlan_rx_frame)) >= 0) {
                ++delivered;
                if ((eth_pkt_last_rx(0)->rdes3 & EQOS_RDES3_WB_LT_MASK) == VLAN_RDES3_LT_VLAN) {
                    ++tagged;
                    if ((eth_pkt_last_rx(0)->rdes3 & VLAN_RDES3_RS0V) != 0U &&
                        (eth_pkt_last_rx(0)->rdes0 & VLAN_VID_MASK) == VLAN_TEST_VID)
                        ++vid_match;
                }
            }
        }

        /* ---------- 5. Check the MMC counters ---------- */
        const uint32_t uc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_UNICAST_GOOD) - uc0;
        const uint32_t crc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_CRC_ERROR_PACKETS) - crc0;
        const uint32_t align = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_ALIGN_ERROR_PACKETS) - align0;
        const uint32_t missed = AX_BASE_READ_REG32(EQOS_DMA_BASE,
                                                   EQOS_DMA_CH0_MISSED_FRAME_COUNT);
        printf("MMC RX unicast good:      %lu (expect %lu)\r\n",
               (unsigned long)uc, (unsigned long)(VLAN_NORMAL_FRAMES + VLAN_TEST_FRAMES));
        printf("MMC RX CRC errors:        %lu (info)\r\n", (unsigned long)crc);
        printf("MMC RX alignment errors:  %lu (info)\r\n", (unsigned long)align);
        printf("Info: delivered to software %lu, DMA missed %lu%s\r\n",
               (unsigned long)delivered, (unsigned long)(missed & 0x7FFU),
               (missed & (1U << 15)) ? " (overflow)" : "");
        printf("VLAN tagged (LT=100):     %lu (expect %lu)\r\n",
               (unsigned long)tagged, (unsigned long)VLAN_TEST_FRAMES);
        printf("VID %lu match (RS0V, OVT): %lu (expect %lu)\r\n",
               (unsigned long)VLAN_TEST_VID, (unsigned long)vid_match,
               (unsigned long)VLAN_TEST_FRAMES);
        if (uc != VLAN_NORMAL_FRAMES + VLAN_TEST_FRAMES) {
            printf("FAIL: unicast good count\r\n");
            ++failures;
        }
        if (tagged != VLAN_TEST_FRAMES) {
            printf("FAIL: tagged count\r\n");
            ++failures;
        }
        if (vid_match != VLAN_TEST_FRAMES) {
            printf("FAIL: OVT VID mismatch\r\n");
            ++failures;
        }
    }
    printf("RESULT: %s\r\n", failures == 0U ? "PASS" : "FAIL");

    /* ---------- 6. Restore MAC_VLAN_Tag and MACPFR ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG, vtr_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
    printf("\r\n========== IxTest8f_Vlan: %s ==========\r\n",
           failures == 0U ? "PASS" : "FAIL");
    return (int)failures;
}
