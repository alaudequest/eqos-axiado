#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* Ixia IxTest8h_NoCrc: RX frames sent without a CRC (test ID 41).
 *
 * main.c includes this file AFTER the TX/RX rings are declared, brings the
 * link up with PHY auto-negotiation (MACCR.LM=0) and runs it once after USER;
 * do not add it as a separate build source.
 *
 * The Ixia sends 1000 good frames, then 1000 frames of 128 B with no CRC
 * appended (the last 4 bytes on the wire are data), to the DUT MAC (static MAC
 * on the Ixia). The MAC checks those last 4 bytes as the FCS, so each frame is
 * a CRC error. With RSF=1 and FEP=0 the MTL drops them (about 1000 delivered).
 * The STM32 empties RX with eth_ingress_packet() and checks the MMC RX
 * counters over the run:
 *   MMCRUPGR   (unicast good) = 1000
 *   MMCRCRCEPR (CRC errors)   = 1000
 * Frames delivered to software are printed for information only.
 * Run: wait up to NOCRC_START_MS for the first frame, then receive for
 * NOCRC_WINDOW_MS, then read the counters. */

#define NOCRC_NORMAL_FRAMES  1000U
#define NOCRC_TEST_FRAMES    1000U
#define NOCRC_START_MS       60000U   /* time to start the Ixia script */
#define NOCRC_WINDOW_MS      10000U   /* receive window after the first frame */

static uint8_t nocrc_rx_frame[EQOS_BUF_SIZE];

int eth_test_ixia_rx_no_crc(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                            struct eqos_desc *rx_desc,
                            uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    unsigned int failures = 0U;
    uint32_t delivered = 0U;

    printf("\r\n ========== Begin Ixia IxTest8h_NoCrc (receive) ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;

    /* ---------- 1. Setup: bind CH0 rings, no promiscuous mode ---------- */
    if (eth_pkt_ring_init(0, tx_desc, EQOS_TX_DESC_COUNT,
                          rx_desc, EQOS_RX_DESC_COUNT, rx_buf) != EQOS_OK) {
        printf("FAIL: eth_pkt_ring_init\r\n");
        return -1;
    }
    const uint32_t pfr_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, EQOS_MAC_PKT_FILTER_PR, 0U)) {
        printf("FAIL: MACPFR setup readback\r\n");
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
        return -1;
    }

    /* ---------- 2. Counter start values (MMC: snapshot; DMA missed: read clears) ---------- */
    const uint32_t uc0 = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_UNICAST_GOOD);
    const uint32_t crc0 = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_CRC_ERROR_PACKETS);
    const uint32_t align0 = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_ALIGN_ERROR_PACKETS);
    (void)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_MISSED_FRAME_COUNT);

    /* ---------- 3. Wait for the first frame ---------- */
    printf("Start IxTest8h_NoCrc on the Ixia now (waiting %lu s).\r\n",
           (unsigned long)(NOCRC_START_MS / 1000U));
    eth_pkt_set_timeout(ETH_PKT_DEFAULT_TIMEOUT_US, NOCRC_START_MS * 1000U);
    if (eth_ingress_packet(0, nocrc_rx_frame, sizeof(nocrc_rx_frame)) < 0) {
        printf("FAIL: no frame received\r\n");
        ++failures;
    } else {
        /* ---------- 4. Receive for NOCRC_WINDOW_MS (no printf) ---------- */
        delivered = 1U;
        eth_pkt_set_timeout(ETH_PKT_DEFAULT_TIMEOUT_US, 0U);    /* RX: check once */
        const uint32_t t0 = HAL_GetTick();
        while (HAL_GetTick() - t0 < NOCRC_WINDOW_MS) {
            if (eth_ingress_packet(0, nocrc_rx_frame, sizeof(nocrc_rx_frame)) >= 0)
                ++delivered;
        }

        /* ---------- 5. Check the MMC counters ---------- */
        const uint32_t uc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_UNICAST_GOOD) - uc0;
        const uint32_t crc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_CRC_ERROR_PACKETS) - crc0;
        const uint32_t align = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_ALIGN_ERROR_PACKETS) - align0;
        const uint32_t missed = AX_BASE_READ_REG32(EQOS_DMA_BASE,
                                                   EQOS_DMA_CH0_MISSED_FRAME_COUNT);
        printf("MMC RX unicast good:      %lu (expect %lu)\r\n",
               (unsigned long)uc, (unsigned long)(NOCRC_NORMAL_FRAMES));
        printf("MMC RX CRC errors:        %lu (expect %lu)\r\n",
               (unsigned long)crc, (unsigned long)(NOCRC_TEST_FRAMES));
        printf("MMC RX alignment errors:  %lu (info)\r\n", (unsigned long)align);
        printf("Info: delivered to software %lu, DMA missed %lu%s\r\n",
               (unsigned long)delivered, (unsigned long)(missed & 0x7FFU),
               (missed & (1U << 15)) ? " (overflow)" : "");
        if (uc != NOCRC_NORMAL_FRAMES) {
            printf("FAIL: unicast good count\r\n");
            ++failures;
        }
        if (crc != NOCRC_TEST_FRAMES) {
            printf("FAIL: CRC error count\r\n");
            ++failures;
        }
    }
    printf("RESULT: %s\r\n", failures == 0U ? "PASS" : "FAIL");

    /* ---------- 6. Restore MACPFR ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
    printf("\r\n========== IxTest8h_NoCrc: %s ==========\r\n",
           failures == 0U ? "PASS" : "FAIL");
    return (int)failures;
}
