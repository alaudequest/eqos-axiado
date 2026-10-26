#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* ETH_MACVTR double VLAN: filter on the inner C-VLAN tag (ERIVLT=1).
 * Flow: MAC_VTR_DVLAN_flow.md.
 *
 * main.c includes this file AFTER the TX/RX rings are declared; do not add
 * it as a separate build source.
 *
 * MACVTR: EDVLP=1 ESVL=1 ETV=1 ERIVLT=1 ERSVLM=0 VL=100; MACPFR.VTFE=1.
 * Frame (68 B, FCS excluded):
 *   DA=SA=MAC0 | 0x88A8 | S-TCI | 0x8100 | C-TCI | 0x88B5 | pattern
 *   Case 1: inner VID 100 -> received, PL=68, data matches
 *   Case 2: inner VID 101 -> dropped (VID mismatch)
 * Path: internal MAC loopback (LM=1). */

#define DVLAN_INNER_FRAME_LEN      68U      /* FCS excluded (CST=1) */
#define DVLAN_INNER_TX_TIMEOUT_US  100000U
#define DVLAN_INNER_RX_TIMEOUT_US  10000U   /* dropped: no frame within 10 ms */
#define DVLAN_INNER_VID            100U
#define DVLAN_INNER_S_TCI          0x000AU  /* outer tag: PCP 0, DEI 0, VID 10 */
#define DVLAN_INNER_C_TCI_PCP_DEI  0xB000U  /* inner tag: PCP 5, DEI 1 */

/* ETH_MACVTR bits not defined in eqos_hw.h */
#define DVLAN_INNER_VTR_ESVL       (1u << 18)
#define DVLAN_INNER_VTR_EDVLP      (1u << 26)
#define DVLAN_INNER_VTR_ERIVLT     (1u << 27)

static uint8_t dvlan_inner_tx_buf[DVLAN_INNER_FRAME_LEN] __attribute__((aligned(32)));
static uint8_t dvlan_inner_rx_frame[EQOS_BUF_SIZE];

int eth_test_mac_vtr_dvlan_inner(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                                 struct eqos_desc *rx_desc,
                                 uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    static const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    const unsigned int case_count = 2; //case 1 received, case 2 dropped
    unsigned int failures = 0U;
    unsigned int passes = 0U;

    printf("\r\n ========== Begin MACVTR double VLAN inner filter test ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint8_t *mac0 = eth->cfg.mac_addr;
    const uint32_t saved_vtr = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG);
    const uint32_t saved_pfr = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);

    /* ---------- 1. Setup: MAC loopback, VLAN tag filter on inner tag ---------- */
    /* MACVTR whole register: VTIM=VTHM=EVLS=0, ERSVLM=0 (inner tag is C-VLAN). */
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, 0U,
                        EQOS_MAC_CFG_LM | EQOS_MAC_CFG_FES | EQOS_MAC_CFG_DM |
                        EQOS_MAC_CFG_CST | EQOS_MAC_CFG_ACS |
                        EQOS_MAC_CFG_TE | EQOS_MAC_CFG_RE) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER,
                        EQOS_MAC_PKT_FILTER_PR | EQOS_MAC_PKT_FILTER_RA,
                        EQOS_MAC_PKT_FILTER_VTFE) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG, 0xFFFFFFFFU,
                        DVLAN_INNER_VTR_EDVLP | DVLAN_INNER_VTR_ESVL |
                        EQOS_MAC_VLAN_TAG_ETV | DVLAN_INNER_VTR_ERIVLT |
                        DVLAN_INNER_VID)) {
        printf("FAIL: MAC setup readback\r\n");
        return -1;
    }

    /* ---------- 2. Frame: outer tag 0x88A8, inner tag 0x8100 ---------- */
    const uint32_t tx_len = eth_construct_frame(dvlan_inner_tx_buf, sizeof(dvlan_inner_tx_buf),
                                                DVLAN_INNER_FRAME_LEN, mac0, mac0,
                                                0x88A8U, pattern, sizeof(pattern));
    if (tx_len == 0U) {
        printf("FAIL: eth_construct_frame\r\n");
        return -1;
    }
    dvlan_inner_tx_buf[14] = (uint8_t)(DVLAN_INNER_S_TCI >> 8);  /* outer TCI */
    dvlan_inner_tx_buf[15] = (uint8_t)DVLAN_INNER_S_TCI;
    dvlan_inner_tx_buf[16] = 0x81U;                              /* inner TPID */
    dvlan_inner_tx_buf[17] = 0x00U;
    dvlan_inner_tx_buf[20] = 0x88U;                              /* EtherType 0x88B5 */
    dvlan_inner_tx_buf[21] = 0xB5U;

    for (unsigned int case_idx = 0U; case_idx < case_count; ++case_idx) {
        const uint32_t inner = DVLAN_INNER_VID + case_idx;  /* 100, then 101 */
        const uint32_t c_tci = DVLAN_INNER_C_TCI_PCP_DEI | inner;
        const bool expect_rx = (case_idx == 0U);
        bool case_pass = true;

        /* ---------- 3. Inner TCI ---------- */
        printf("\r\n--- Case %u/%u: VL=%lu inner VID %lu: expect %s ---\r\n",
               case_idx + 1U, case_count, (unsigned long)DVLAN_INNER_VID,
               (unsigned long)inner, expect_rx ? "RECEIVED" : "DROPPED");
        dvlan_inner_tx_buf[18] = (uint8_t)(c_tci >> 8);
        dvlan_inner_tx_buf[19] = (uint8_t)c_tci;

        /* ---------- 4. Send ---------- */
        struct eth_tx_frame txf;
        if (eth_tx_frame_send(eth, tx_desc, dvlan_inner_tx_buf, tx_len, 0U, 0U,
                              DVLAN_INNER_TX_TIMEOUT_US, &txf) != EQOS_OK) {
            printf("RESULT: FAIL (TX timeout, DMACSR=0x%08lX)\r\n",
                   (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
            ++failures;
            break; /* DMA may still own a descriptor: stop the test. */
        }
        printf("TX done: TDES3=0x%08lX ES=%lu\r\n", (unsigned long)txf.tdes3,
               (unsigned long)((txf.tdes3 & EQOS_TDES3_WB_ES) != 0U));
        if ((txf.tdes3 & EQOS_TDES3_WB_ES) != 0U)
            case_pass = false;

        /* ---------- 5. Receive ---------- */
        struct eth_rx_frame rxf;
        int rx_status = eth_rx_frame_receive(eth, rx_desc, rx_buf, dvlan_inner_rx_frame,
                                             sizeof(dvlan_inner_rx_frame),
                                             DVLAN_INNER_RX_TIMEOUT_US, &rxf);
        if (rx_status == EQOS_EIO) {
            printf("RESULT: FAIL (RX frame stalled mid-way)\r\n");
            ++failures;
            break;
        }
        if (rx_status == EQOS_ETIMEDOUT) {
            printf("RX: no frame within %lu us\r\n",
                   (unsigned long)DVLAN_INNER_RX_TIMEOUT_US);
            if (expect_rx) {
                printf("FAIL: frame dropped, expected received\r\n");
                case_pass = false;
            } else {
                printf("Frame DROPPED by VLAN filter as expected\r\n");
            }
        } else {
            const uint32_t lt = rxf.rdes3 & EQOS_RDES3_WB_LT_MASK;
            printf("RX done: RDES3=0x%08lX PL=%lu LT=0b%lu%lu%lu ES=%lu\r\n",
                   (unsigned long)rxf.rdes3, (unsigned long)rxf.len,
                   (unsigned long)((lt >> 18) & 1U), (unsigned long)((lt >> 17) & 1U),
                   (unsigned long)((lt >> 16) & 1U),
                   (unsigned long)((rxf.rdes3 & EQOS_RDES3_WB_ES) != 0U));
            if (!expect_rx) {
                printf("FAIL: frame received, expected dropped\r\n");
                case_pass = false;
            } else {
                /* ---------- 6. Received: ES, length, whole frame ---------- */
                if ((rxf.rdes3 & EQOS_RDES3_WB_ES) != 0U) {
                    printf("FAIL: ES=1\r\n");
                    case_pass = false;
                }
                if (rxf.len != tx_len) {
                    printf("FAIL: length %lu, expected %lu\r\n",
                           (unsigned long)rxf.len, (unsigned long)tx_len);
                    case_pass = false;
                } else {
                    for (uint32_t i = 0U; i < tx_len; ++i) {
                        if (dvlan_inner_rx_frame[i] != dvlan_inner_tx_buf[i]) {
                            printf("FAIL at byte %lu: expected %02X, got %02X\r\n",
                                   (unsigned long)i, (unsigned int)dvlan_inner_tx_buf[i],
                                   (unsigned int)dvlan_inner_rx_frame[i]);
                            case_pass = false;
                            break;
                        }
                    }
                }
                if (case_pass)
                    printf("Frame RECEIVED through VLAN filter as expected\r\n");
            }
        }
        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        if (case_pass)
            ++passes;
        else
            ++failures;
    }

    /* ---------- 7. Restore MACVTR and MACPFR ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG, saved_vtr);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, saved_pfr);

    printf("\r\n========== DVLAN inner filter test: %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, failures);
    if (passes + failures < case_count)
        printf("%u case(s) did not run; reset before rerunning.\r\n",
               case_count - passes - failures);
    return (int)failures;
}
