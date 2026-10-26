#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* MAC_Q0_Tx_Flow_Ctrl (ETH_MACTFCR) pause frame test, measured with the CPU
 * cycle counter (DWT->CYCCNT). Timestamp version: MAC_Q0_TX_FLOW_CTRL_PAUSE_TIMESTAMP.c
 *
 * main.c includes this file AFTER the TX/RX rings are declared; do not add
 * it as a separate build source.
 *
 * Path: internal MAC loopback, 100 Mbps full duplex (main.c).
 * 1. TFE=1, PT=pause time; write FCB_BPA=1 -> the MAC sends a pause frame
 *    (DA 01-80-C2-00-00-01, type 0x8808, opcode 0x0001, PT). FCB_BPA self-clears.
 * 2. RFE=1: MAC RX decodes the looped-back pause frame and holds MAC TX for
 *    PT x 512 bit times (5.12 us per quantum at 100 Mbps). PCF=10 forwards
 *    the pause frame to software: T0 = DWT when it is received (pause started).
 * 3. A data frame is queued at once; eth_tx_frame_send() returns after it is
 *    sent: T1 = DWT.
 * 4. delta = T1 - T0 = PT x 5.12 us (+ a few us of RX/TX polling).
 *   Case 1: PT=0xFFFF -> 335539 us. The pause timer runs on the RMII reference
 *           clock (PHY crystal), DWT on the core clock (HSI here): case 1
 *           measures the core clock error in ppm (limit +-2%).
 *   Case 2: PT=0x0100 -> 1311 us x (1 + case 1 error), window -20/+200 us:
 *           proves the pause time follows the PT value.
 * Nothing is printed between T0 and queuing the data frame (UART is slow). */

#define TXFC_TX_TIMEOUT_US     1000000U /* longer than the 335 ms pause */
#define TXFC_RX_TIMEOUT_US     10000U
#define TXFC_FCB_TIMEOUT_US    10000U
#define TXFC_FRAME_LEN         64U
#define TXFC_WIN_LOW_NS        20000    /* case 2 window: -20 us */
#define TXFC_WIN_HIGH_NS       200000   /*                +200 us */
#define TXFC_CLK_TOL_PPM       20000    /* +-2 %: HSI tolerance (case 1) */

static uint8_t txfc_tx_buf[TXFC_FRAME_LEN] __attribute__((aligned(32)));
static uint8_t txfc_rx_frame[EQOS_BUF_SIZE];

int eth_test_mac_txfc_pause(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                            struct eqos_desc *rx_desc,
                            uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    static const uint16_t pause_time[2] = {0xFFFFU, 0x0010U};
    static const uint8_t pause_da[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x01};
    static const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    const unsigned int case_count = 2U;
    unsigned int failures = 0U;
    unsigned int passes = 0U;
    int64_t clk_err_ppm = 0;      /* core clock error, measured in case 1 */
    bool clk_measured = false;

    printf("\r\n ========== Begin MAC_Q0_Tx_Flow_Ctrl pause test (DWT) ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint8_t *mac0 = eth->cfg.mac_addr;
    printf("Core clock %lu Hz, 100 Mbps: 1 pause quantum = 5.12 us\r\n",
           (unsigned long)SystemCoreClock);

    /* ---------- 1. Setup: RFE, PCF=10 (forward pause frame to software) ---------- */
    const uint32_t txfc_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_Q0_TX_FLOW_CTRL);
    const uint32_t rxfc_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_FLOW_CTRL);
    const uint32_t pfr_saved  = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
    ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_RX_FLOW_CTRL, 0U, EQOS_MAC_RXFC_RFE);
    ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER,
                   EQOS_MAC_PKT_FILTER_PCF_MASK, EQOS_MAC_PKT_FILTER_PCF_ALL);

    for (unsigned int case_idx = 0U; case_idx < case_count; ++case_idx) {
        const uint32_t pt = pause_time[case_idx];
        const uint64_t expected_ns = (uint64_t)pt * 5120U; /* 512 bit times x 10 ns */
        bool case_pass = true;

        /* ---------- 2. MAC_Q0_Tx_Flow_Ctrl: PT, TFE ---------- */
        printf("\r\n--- Case %u/%u: PT=0x%04lX, expected pause %lu ns ---\r\n",
               case_idx + 1U, case_count, (unsigned long)pt, (unsigned long)expected_ns);
        if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_Q0_TX_FLOW_CTRL,
                            (EQOS_MAC_TXFC_PT_MASK << EQOS_MAC_TXFC_PT_SHIFT) |
                            EQOS_MAC_TXFC_TFE | EQOS_MAC_TXFC_FCB_BPA,
                            (pt << EQOS_MAC_TXFC_PT_SHIFT) | EQOS_MAC_TXFC_TFE)) {
            printf("RESULT: FAIL (MAC_Q0_Tx_Flow_Ctrl readback)\r\n");
            ++failures;
            continue;
        }

        /* ---------- 3. FCB_BPA=1: MAC sends the pause frame ---------- */
        const uint32_t txfc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_Q0_TX_FLOW_CTRL);
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_Q0_TX_FLOW_CTRL,
                            txfc | EQOS_MAC_TXFC_FCB_BPA);
        uint32_t elapsed = 0U;
        while ((AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_Q0_TX_FLOW_CTRL) &
                EQOS_MAC_TXFC_FCB_BPA) != 0U && elapsed < TXFC_FCB_TIMEOUT_US) {
            eqos_udelay(1U);
            ++elapsed;
        }
        if (elapsed >= TXFC_FCB_TIMEOUT_US) {
            printf("RESULT: FAIL (FCB_BPA did not clear: pause frame not sent)\r\n");
            ++failures;
            continue;
        }

        /* ---------- 4. Receive the pause frame: T0 = DWT ---------- */
        struct eth_rx_frame rxf;
        if (eth_rx_frame_receive(eth, rx_desc, rx_buf, txfc_rx_frame, sizeof(txfc_rx_frame),
                                 TXFC_RX_TIMEOUT_US, &rxf) != EQOS_OK) {
            printf("RESULT: FAIL (pause frame not received)\r\n");
            ++failures;
            continue;
        }
        const uint32_t t0 = DWT->CYCCNT;
        /* Check now, print after T1: no printf before the data frame is queued. */
        const uint32_t pause_pl = rxf.len;
        uint8_t hdr[18];
        memcpy(hdr, txfc_rx_frame, sizeof(hdr));
        const uint32_t rx_pt = ((uint32_t)hdr[16] << 8) | hdr[17];
        const bool fields_ok = memcmp(hdr, pause_da, 6U) == 0 &&
                               memcmp(hdr + 6U, mac0, 6U) == 0 &&
                               hdr[12] == 0x88U && hdr[13] == 0x08U &&
                               hdr[14] == 0x00U && hdr[15] == 0x01U && rx_pt == pt;

        /* ---------- 5. Data frame queued during the pause: T1 = DWT ---------- */
        eth_construct_frame(txfc_tx_buf, sizeof(txfc_tx_buf), TXFC_FRAME_LEN, mac0, mac0,
                            0x88B5U, pattern, sizeof(pattern));
        struct eth_tx_frame txf;
        if (eth_tx_frame_send(eth, tx_desc, txfc_tx_buf, TXFC_FRAME_LEN, 0U, 0U,
                              TXFC_TX_TIMEOUT_US, &txf) != EQOS_OK) {
            printf("RESULT: FAIL (data frame TX timeout, DMA_CH0_Status=0x%08lX)\r\n",
                   (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
            ++failures;
            break; /* DMA may still own a descriptor: stop the test. */
        }
        const uint32_t t1 = DWT->CYCCNT;

        /* T1 taken: printing is safe now. */
        printf("Pause frame RX: DA %02X:%02X:%02X:%02X:%02X:%02X type %02X%02X "
               "opcode %02X%02X PT=0x%04lX PL=%lu: %s\r\n",
               hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[12], hdr[13],
               hdr[14], hdr[15], (unsigned long)rx_pt, (unsigned long)pause_pl,
               fields_ok ? "OK" : "FAIL");
        if (!fields_ok)
            case_pass = false;

        /* ---------- 6. Receive the data frame, check it is intact ---------- */
        if (eth_rx_frame_receive(eth, rx_desc, rx_buf, txfc_rx_frame, sizeof(txfc_rx_frame),
                                 TXFC_RX_TIMEOUT_US, &rxf) != EQOS_OK ||
            rxf.len != TXFC_FRAME_LEN ||
            memcmp(txfc_rx_frame, txfc_tx_buf, TXFC_FRAME_LEN) != 0) {
            printf("FAIL: data frame not received intact (PL=%lu)\r\n",
                   (unsigned long)rxf.len);
            case_pass = false;
        }

        /* ---------- 7. Pause time = T1 - T0 ---------- */
        const int64_t delta_ns = (int64_t)((uint64_t)(t1 - t0) * 1000000000ULL /
                                           SystemCoreClock);
        bool time_ok;
        if (case_idx == 0U) {
            /* Case 1: core clock error vs the pause timer (RMII clock). */
            const int64_t err_ppm = (delta_ns - (int64_t)expected_ns) * 1000000LL /
                                    (int64_t)expected_ns;
            time_ok = err_ppm >= -TXFC_CLK_TOL_PPM && err_ppm <= TXFC_CLK_TOL_PPM;
            if (time_ok) {
                clk_err_ppm = err_ppm;
                clk_measured = true;
            }
            printf("T1 - T0 = %ld ns, expected %lu ns, core clock error %ld ppm "
                   "(limit +-%d): %s\r\n", (long)delta_ns, (unsigned long)expected_ns,
                   (long)err_ppm, TXFC_CLK_TOL_PPM, time_ok ? "OK" : "FAIL");
        } else {
            /* Case 2: expected scaled by the case 1 clock error, -20/+200 us. */
            const int64_t exp_cal = (int64_t)expected_ns +
                                    (int64_t)expected_ns * clk_err_ppm / 1000000LL;
            const int64_t diff_ns = delta_ns - exp_cal;
            time_ok = clk_measured && diff_ns >= -TXFC_WIN_LOW_NS &&
                      diff_ns <= TXFC_WIN_HIGH_NS;
            if (!clk_measured)
                printf("FAIL: no clock error from case 1\r\n");
            printf("T1 - T0 = %ld ns, expected %lu ns x (1 %+ld ppm) = %ld ns, diff %ld ns "
                   "(window -%d/+%d): %s\r\n", (long)delta_ns, (unsigned long)expected_ns,
                   (long)clk_err_ppm, (long)exp_cal, (long)diff_ns, TXFC_WIN_LOW_NS,
                   TXFC_WIN_HIGH_NS, time_ok ? "OK" : "FAIL");
        }
        if (!time_ok)
            case_pass = false;

        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        if (case_pass)
            ++passes;
        else
            ++failures;
    }

    /* ---------- 8. Restore ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_Q0_TX_FLOW_CTRL,
                        txfc_saved & ~EQOS_MAC_TXFC_FCB_BPA);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_FLOW_CTRL, rxfc_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);

    printf("\r\n========== MAC_Q0_Tx_Flow_Ctrl pause test (DWT): %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, failures);
    if (passes + failures < case_count)
        printf("%u case(s) did not run; reset before rerunning.\r\n",
               case_count - passes - failures);
    return (int)failures;
}
