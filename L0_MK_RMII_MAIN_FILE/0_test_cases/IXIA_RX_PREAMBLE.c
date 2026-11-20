#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* Ixia IxTest8i_Preamble: RX frames with a short or a long preamble (test ID 42).
 *
 * main.c includes this file AFTER the TX/RX rings are declared, brings the
 * link up with PHY auto-negotiation (MACCR.LM=0) and runs it once after USER;
 * do not add it as a separate build source.
 *
 * The Ixia sends 1000 good frames (standard 7 B preamble + SFD), then 1000
 * good frames with a changed preamble length, to the DUT MAC (static MAC on
 * the Ixia). Run the test twice: once with the Ixia short-preamble script
 * variant, once with the long one. The EQOS has no preamble status bit: the
 * PHY (RMII) passes the frame after the SFD, so a frame the MAC sees is a
 * normal good frame. Pass = every frame is received:
 *   MMCRUPGR   (unicast good) = 2000
 *   MMCRCRCEPR (CRC errors)   = 0, MMCRAEPR (alignment errors) = 0
 * A short-preamble fail (unicast < 2000, no CRC error) means the PHY did not
 * pass the frame: record the shortest preamble that still passes.
 * Frames delivered to software are printed for information only.
 * Run: wait up to PREAMBLE_START_MS for the first frame, then receive for
 * PREAMBLE_WINDOW_MS, then read the counters. */

#define PREAMBLE_NORMAL_FRAMES  1000U
#define PREAMBLE_TEST_FRAMES    1000U
#define PREAMBLE_START_MS       60000U   /* time to start the Ixia script */
#define PREAMBLE_WINDOW_MS      10000U   /* receive window after the first frame */

static uint8_t preamble_rx_frame[EQOS_BUF_SIZE];

int eth_test_ixia_rx_preamble(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                              struct eqos_desc *rx_desc,
                              uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    unsigned int failures = 0U;
    uint32_t delivered = 0U;

    printf("\r\n ========== Begin Ixia IxTest8i_Preamble (receive) ==========\r\n");
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
    printf("Start IxTest8i_Preamble on the Ixia now (waiting %lu s).\r\n",
           (unsigned long)(PREAMBLE_START_MS / 1000U));
    eth_pkt_set_timeout(ETH_PKT_DEFAULT_TIMEOUT_US, PREAMBLE_START_MS * 1000U);
    if (eth_ingress_packet(0, preamble_rx_frame, sizeof(preamble_rx_frame)) < 0) {
        printf("FAIL: no frame received\r\n");
        ++failures;
    } else {
        /* ---------- 4. Receive for PREAMBLE_WINDOW_MS (no printf) ---------- */
        delivered = 1U;
        eth_pkt_set_timeout(ETH_PKT_DEFAULT_TIMEOUT_US, 0U);    /* RX: check once */
        const uint32_t t0 = HAL_GetTick();
        while (HAL_GetTick() - t0 < PREAMBLE_WINDOW_MS) {
            if (eth_ingress_packet(0, preamble_rx_frame, sizeof(preamble_rx_frame)) >= 0)
                ++delivered;
        }

        /* ---------- 5. Check the MMC counters ---------- */
        const uint32_t uc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_UNICAST_GOOD) - uc0;
        const uint32_t crc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_CRC_ERROR_PACKETS) - crc0;
        const uint32_t align = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_ALIGN_ERROR_PACKETS) - align0;
        const uint32_t missed = AX_BASE_READ_REG32(EQOS_DMA_BASE,
                                                   EQOS_DMA_CH0_MISSED_FRAME_COUNT);
        printf("MMC RX unicast good:      %lu (expect %lu)\r\n",
               (unsigned long)uc, (unsigned long)(PREAMBLE_NORMAL_FRAMES + PREAMBLE_TEST_FRAMES));
        printf("MMC RX CRC errors:        %lu (expect 0)\r\n", (unsigned long)crc);
        printf("MMC RX alignment errors:  %lu (expect 0)\r\n", (unsigned long)align);
        printf("Info: delivered to software %lu, DMA missed %lu%s\r\n",
               (unsigned long)delivered, (unsigned long)(missed & 0x7FFU),
               (missed & (1U << 15)) ? " (overflow)" : "");
        if (uc != PREAMBLE_NORMAL_FRAMES + PREAMBLE_TEST_FRAMES) {
            printf("FAIL: unicast good count\r\n");
            ++failures;
        }
        if (crc != 0U) {
            printf("FAIL: CRC error count\r\n");
            ++failures;
        }
        if (align != 0U) {
            printf("FAIL: alignment error count\r\n");
            ++failures;
        }
    }
    printf("RESULT: %s\r\n", failures == 0U ? "PASS" : "FAIL");

    /* ---------- 6. Restore MACPFR ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
    printf("\r\n========== IxTest8i_Preamble: %s ==========\r\n",
           failures == 0U ? "PASS" : "FAIL");
    return (int)failures;
}
