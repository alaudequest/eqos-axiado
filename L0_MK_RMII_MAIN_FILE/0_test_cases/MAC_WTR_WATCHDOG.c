#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* MAC_Watchdog_Timeout (ETH_MACWTR) receive watchdog test.
 *
 * main.c includes this file AFTER the TX/RX rings are declared; do not add
 * it as a separate build source.
 *
 * PWE=1: a received frame longer than (WTO + 2) x 1024 bytes is cut and
 * reported in RDES3.RWT, MAC_Rx_Tx_Status.RWT and DMA_CH0_Status.RWT.
 *   Case 1: WTO=1  (L=3072):  3008 B -> RWT=0,  3136 B -> RWT=1
 *   Case 2: WTO=13 (L=15360): 15296 B -> RWT=0, 15424 B -> RWT=1
 * Path: internal MAC loopback (main.c), WD=0 (reset value).
 * Settings needed only by this test (restored at the end): JD=1 (TX jabber
 * off), MTL threshold mode TTC/RTC=64, FEP=1 (forward the cut frame). */

#define WTR_TEST_TIMEOUT_US  100000U
#define WTR_TEST_BUF_SIZE    16384U

static uint8_t wtr_tx_buf[WTR_TEST_BUF_SIZE] __attribute__((aligned(32)));
static uint8_t wtr_rx_frame[WTR_TEST_BUF_SIZE];

int eth_test_mac_wtr_watchdog(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                              struct eqos_desc *rx_desc,
                              uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    static const uint32_t wto[2] = {1U, 9U};
    static const uint32_t frame_size[2][2] = {{3008U, 3136U}, {11200U, 11328U}};
    static const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    const unsigned int case_count = 2U;
    unsigned int failures = 0U;
    unsigned int passes = 0U;
    bool stop = false;

    printf("\r\n ========== Begin MAC_Watchdog_Timeout test ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint8_t *mac0 = eth->cfg.mac_addr; /* DA = SA = MAC0 */

    /* ---------- 1. Setup: TX jabber off, MTL threshold mode, FEP ---------- */
    const uint32_t maccr_saved  = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);
    const uint32_t wtr_saved    = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_WATCHDOG_TIMEOUT);
    const uint32_t mtl_tx_saved = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE);
    const uint32_t mtl_rx_saved = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE);
    ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, 0U, EQOS_MAC_CFG_JD);
    ETH_REG_UPDATE(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE,
                   EQOS_MTL_TXQ_TSF | EQOS_MTL_TXQ_TTC_MASK, EQOS_MTL_TXQ_TTC_64);
    ETH_REG_UPDATE(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE,
                   EQOS_MTL_RXQ_RSF | EQOS_MTL_RXQ_RTC_MASK,
                   EQOS_MTL_RXQ_RTC_64 | EQOS_MTL_RXQ_FEP);

    for (unsigned int case_idx = 0U; case_idx < case_count && !stop; ++case_idx) {
        const uint32_t limit = (wto[case_idx] + 2U) * 1024U;
        bool case_pass = true;

        /* ---------- 2. MAC_Watchdog_Timeout: PWE=1, WTO per case ---------- */
        printf("\r\n--- Case %u/%u: PWE=1 WTO=%lu, limit %lu bytes ---\r\n",
               case_idx + 1U, case_count, (unsigned long)wto[case_idx],
               (unsigned long)limit);
        if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_WATCHDOG_TIMEOUT,
                            EQOS_MAC_WTO_PWE | EQOS_MAC_WTO_MASK,
                            EQOS_MAC_WTO_PWE | wto[case_idx])) {
            printf("RESULT: FAIL (MAC_Watchdog_Timeout readback)\r\n");
            ++failures;
            continue;
        }

        for (unsigned int f = 0U; f < 2U; ++f) {
            const uint32_t size = frame_size[case_idx][f];
            const bool expect_cut = (f == 1U);

            /* ---------- 3. Clear old flags (MAC: read clears, DMA: W1C) ---------- */
            (void)AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_TX_STATUS);
            AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS,
                                EQOS_DMA_CH_STATUS_RWT | EQOS_DMA_CH_STATUS_AIS);

            /* ---------- 4. Send and receive ---------- */
            eth_construct_frame(wtr_tx_buf, sizeof(wtr_tx_buf), size, mac0, mac0,
                                0x88B5U, pattern, sizeof(pattern));
            struct eth_tx_frame txf;
            struct eth_rx_frame rxf;
            if (eth_tx_frame_send(eth, tx_desc, wtr_tx_buf, size, 0U, 0U,
                                  WTR_TEST_TIMEOUT_US, &txf) != EQOS_OK ||
                eth_rx_frame_receive(eth, rx_desc, rx_buf, wtr_rx_frame,
                                     sizeof(wtr_rx_frame), WTR_TEST_TIMEOUT_US,
                                     &rxf) != EQOS_OK) {
                printf("RESULT: FAIL (TX/RX timeout, DMA_CH0_Status=0x%08lX)\r\n",
                       (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
                ++failures;
                stop = true; /* DMA may still own a descriptor: stop the test. */
                break;
            }

            /* ---------- 5. Check RWT in descriptor, MAC and DMA status ---------- */
            const bool rwt_desc = (rxf.rdes3 & EQOS_RDES3_WB_RWT) != 0U;
            const bool rwt_mac = (AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_TX_STATUS) &
                                  EQOS_MAC_RXTX_STS_RWT) != 0U;
            const bool rwt_dma = (AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS) &
                                  EQOS_DMA_CH_STATUS_RWT) != 0U;
            const bool pass = rwt_desc == expect_cut && rwt_mac == expect_cut &&
                              rwt_dma == expect_cut &&
                              (!expect_cut || rxf.len <= limit + 4U);
            printf("TX %lu B -> PL=%lu, RWT desc/MAC/DMA=%u/%u/%u, expect %s: %s\r\n",
                   (unsigned long)size, (unsigned long)rxf.len,
                   (unsigned int)rwt_desc, (unsigned int)rwt_mac, (unsigned int)rwt_dma,
                   expect_cut ? "CUT" : "RECEIVED", pass ? "OK" : "FAIL");
            if (!pass)
                case_pass = false;
        }
        if (stop)
            break;
        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        if (case_pass)
            ++passes;
        else
            ++failures;
    }

    /* ---------- 6. Restore ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_WATCHDOG_TIMEOUT, wtr_saved);
    AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE, mtl_tx_saved);
    AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, mtl_rx_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, maccr_saved);

    printf("\r\n========== MAC_Watchdog_Timeout test: %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, failures);
    if (passes + failures < case_count)
        printf("%u case(s) did not run; reset before rerunning.\r\n",
               case_count - passes - failures);
    return (int)failures;
}
