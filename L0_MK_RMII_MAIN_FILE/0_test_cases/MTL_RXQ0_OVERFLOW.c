#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* MTL_RxQ0 RX FIFO overflow while the RX DMA has no free descriptor, then recovery.
 * Replaces 2_MTL_DMA_test/L1_RMII_MTL_RX_OVERFLOW.c (ID 10, queue runner).
 *
 * main.c includes this file AFTER the TX/RX rings are declared; do not add
 * it as a separate build source.
 *
 * MTL_RxQ0_Operation_Mode: RSF=1, EHFC=0 (no flow control), FEP=FUP=0.
 * Frame (512 B): DA=SA=MAC0 | 0x88B5 | sequence number (bytes 14..17) | pattern.
 * Frames are sent without reading RX: the RX ring fills, the DMA stops (RBU),
 * later frames wait in the RX FIFO until it overflows. Then RX is read.
 *   Case 1: 7 frames  (fit in the ring) -> all received in order, overflow count 0
 *   Case 2: 64 frames (ring + FIFO full) -> RBU=1, overflow count > 0,
 *           first K frames received in order (ring + FIFO), 7 <= K < 64
 *   Each case ends with one new 128 B frame received intact (recovery).
 * Path: internal MAC loopback (LM=1). */

#define MTLOVF_FRAME_LEN        512U
#define MTLOVF_RECOVERY_LEN     128U
#define MTLOVF_TX_TIMEOUT_US    100000U
#define MTLOVF_RX_TIMEOUT_US    2000U    /* drain ends after 2 ms without a frame */
#define MTLOVF_CHECK_SUM        0        /* 1: received + overflow + missed == sent is pass/fail */

/* Registers and bits not defined in eqos_hw.h */
#define MTLOVF_MTL_RXQ0_MISSED  0x34u      /* MTL_RxQ0_Missed_Packet_Overflow_Cnt (0xD34), clear on read */
#define MTLOVF_OVFPKTCNT_MASK   0x7FFu     /* [10:0] packets dropped by RX FIFO overflow */
#define MTLOVF_OVFCNTOVF        (1u << 11) /* overflow counter wrapped */
#define MTLOVF_MISPKTCNT_SHIFT  16U        /* [26:16] packets missed */
#define MTLOVF_MISPKTCNT_MASK   0x7FFu
#define MTLOVF_RXQ_FUP          (1u << 3)  /* forward undersized good packets */
#define MTLOVF_DMA_STS_RBU      (1u << 7)  /* DMA_CH0_Status: receive buffer unavailable, W1C */

static uint8_t mtlovf_tx_buf[MTLOVF_FRAME_LEN] __attribute__((aligned(32)));
static uint8_t mtlovf_rx_frame[EQOS_BUF_SIZE];

int eth_test_mtl_rxq0_overflow(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                               struct eqos_desc *rx_desc,
                               uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    static const uint32_t frame_count[2] = {EQOS_RX_DESC_COUNT - 1U, 64U};
    static const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    const unsigned int case_count = 2U; //case 1 no overflow, case 2 overflow
    unsigned int failures = 0U;
    unsigned int passes = 0U;
    bool stop = false;

    printf("\r\n ========== Begin MTL_RxQ0 overflow test ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint8_t *mac0 = eth->cfg.mac_addr; /* DA = SA = MAC0 */

    /* ---------- 1. Setup: RX store-and-forward, no flow control ---------- */
    const uint32_t mtl_rx_saved = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE);
    if (!ETH_REG_UPDATE(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE,
                        EQOS_MTL_RXQ_EHFC | EQOS_MTL_RXQ_FEP | MTLOVF_RXQ_FUP,
                        EQOS_MTL_RXQ_RSF)) {
        printf("FAIL: MTL setup readback\r\n");
        AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, mtl_rx_saved);
        return -1;
    }

    for (unsigned int case_idx = 0U; case_idx < case_count && !stop; ++case_idx) {
        const uint32_t count = frame_count[case_idx];
        const bool expect_ovf = (case_idx == 1U);
        bool case_pass = true;

        printf("\r\n--- Case %u/%u: %lu frames without reading RX: expect %s ---\r\n",
               case_idx + 1U, case_count, (unsigned long)count,
               expect_ovf ? "OVERFLOW" : "NO OVERFLOW");

        /* ---------- 2. Clear old counters (read clears) and RBU (W1C) ---------- */
        (void)AX_BASE_READ_REG32(EQOS_MTL_BASE, MTLOVF_MTL_RXQ0_MISSED);
        AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS, MTLOVF_DMA_STS_RBU);

        /* ---------- 3. Send all frames, RX not read ---------- */
        eth_construct_frame(mtlovf_tx_buf, sizeof(mtlovf_tx_buf), MTLOVF_FRAME_LEN,
                            mac0, mac0, 0x88B5U, pattern, sizeof(pattern));
        for (uint32_t n = 0U; n < count; ++n) {
            struct eth_tx_frame txf;
            memcpy(&mtlovf_tx_buf[14], &n, 4U);               /* sequence number of this frame */
            if (eth_tx_frame_send(eth, tx_desc, mtlovf_tx_buf, MTLOVF_FRAME_LEN, 0U, 0U,
                                  MTLOVF_TX_TIMEOUT_US, &txf) != EQOS_OK ||
                (txf.tdes3 & EQOS_TDES3_WB_ES) != 0U) {
                printf("RESULT: FAIL (TX frame %lu, TDES3=0x%08lX)\r\n",
                       (unsigned long)n, (unsigned long)txf.tdes3);
                stop = true; /* DMA may still own a descriptor: stop the test. */
                break;
            }
        }
        if (stop) {
            ++failures;
            break;
        }
        eqos_udelay(MTLOVF_RX_TIMEOUT_US);                     /* last frame reaches the RX FIFO */

        const uint32_t sts = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS);
        const uint32_t cnt = AX_BASE_READ_REG32(EQOS_MTL_BASE, MTLOVF_MTL_RXQ0_MISSED); /* read once: clears */
        const uint32_t ovf = cnt & MTLOVF_OVFPKTCNT_MASK;
        const uint32_t mis = (cnt >> MTLOVF_MISPKTCNT_SHIFT) & MTLOVF_MISPKTCNT_MASK;
        printf("After TX: DMA_CH0_Status=0x%08lX RBU=%lu, "
               "MTL_RxQ0_Missed_Packet_Overflow_Cnt=0x%08lX (overflow %lu, missed %lu)\r\n",
               (unsigned long)sts, (unsigned long)((sts & MTLOVF_DMA_STS_RBU) != 0U),
               (unsigned long)cnt, (unsigned long)ovf, (unsigned long)mis);

        /* ---------- 4. Drain: frames must come out in order, no gap or repeat ---------- */
        uint32_t received = 0U;
        while (received <= count) {
            struct eth_rx_frame rxf;
            const int rx = eth_rx_frame_receive(eth, rx_desc, rx_buf, mtlovf_rx_frame,
                                                sizeof(mtlovf_rx_frame),
                                                MTLOVF_RX_TIMEOUT_US, &rxf);
            if (rx == EQOS_ETIMEDOUT)
                break;                                         /* no more frames */
            memcpy(&mtlovf_tx_buf[14], &received, 4U);        /* expected: next sequence number */
            if (rx != EQOS_OK || (rxf.rdes3 & EQOS_RDES3_WB_ES) != 0U ||
                rxf.len != MTLOVF_FRAME_LEN ||
                memcmp(mtlovf_rx_frame, mtlovf_tx_buf, MTLOVF_FRAME_LEN) != 0) {
                printf("FAIL: drained frame %lu (expected sequence %lu) PL=%lu RDES3=0x%08lX\r\n",
                       (unsigned long)received, (unsigned long)received,
                       (unsigned long)rxf.len, (unsigned long)rxf.rdes3);
                case_pass = false;
                break;
            }
            ++received;
        }
        printf("Drained %lu of %lu frames in order\r\n",
               (unsigned long)received, (unsigned long)count);

        /* ---------- 5. Overflow / no-overflow checks ---------- */
        if (expect_ovf) {
            if ((sts & MTLOVF_DMA_STS_RBU) == 0U) {
                printf("FAIL: RBU=0, RX DMA did not run out of descriptors\r\n");
                case_pass = false;
            }
            if ((cnt & (MTLOVF_OVFPKTCNT_MASK | MTLOVF_OVFCNTOVF)) == 0U) {
                printf("FAIL: overflow count 0\r\n");
                case_pass = false;
            }
            if (received < EQOS_RX_DESC_COUNT - 1U || received >= count) {  /* ring .. sent-1 */
                printf("FAIL: drained count outside %lu..%lu\r\n",
                       (unsigned long)(EQOS_RX_DESC_COUNT - 1U), (unsigned long)(count - 1U));
                case_pass = false;
            }
        } else {
            if (ovf != 0U || received != count) {
                printf("FAIL: expected overflow 0 and %lu frames received\r\n",
                       (unsigned long)count);
                case_pass = false;
            }
        }
        printf("received + overflow + missed = %lu, sent %lu%s\r\n",
               (unsigned long)(received + ovf + mis), (unsigned long)count,
               MTLOVF_CHECK_SUM ? "" : " (log only)");
        if (MTLOVF_CHECK_SUM && received + ovf + mis != count)
            case_pass = false;

        /* ---------- 6. Recovery: one new 128 B frame ---------- */
        struct eth_tx_frame txf;
        struct eth_rx_frame rxf;
        eth_construct_frame(mtlovf_tx_buf, sizeof(mtlovf_tx_buf), MTLOVF_RECOVERY_LEN,
                            mac0, mac0, 0x88B5U, pattern, sizeof(pattern));
        if (eth_tx_frame_send(eth, tx_desc, mtlovf_tx_buf, MTLOVF_RECOVERY_LEN, 0U, 0U,
                              MTLOVF_TX_TIMEOUT_US, &txf) != EQOS_OK ||
            eth_rx_frame_receive(eth, rx_desc, rx_buf, mtlovf_rx_frame,
                                 sizeof(mtlovf_rx_frame), MTLOVF_RX_TIMEOUT_US,
                                 &rxf) != EQOS_OK ||
            rxf.len != MTLOVF_RECOVERY_LEN ||
            memcmp(mtlovf_rx_frame, mtlovf_tx_buf, MTLOVF_RECOVERY_LEN) != 0) {
            printf("FAIL: recovery frame not received intact (PL=%lu)\r\n",
                   (unsigned long)rxf.len);
            case_pass = false;
        } else {
            printf("Recovery frame received intact\r\n");
        }

        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        if (case_pass)
            ++passes;
        else
            ++failures;
    }

    /* ---------- 7. Restore ---------- */
    AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, mtl_rx_saved);

    printf("\r\n========== MTL_RxQ0 overflow test: %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, failures);
    if (passes + failures < case_count)
        printf("%u case(s) did not run; reset before rerunning.\r\n",
               case_count - passes - failures);
    return (int)failures;
}
