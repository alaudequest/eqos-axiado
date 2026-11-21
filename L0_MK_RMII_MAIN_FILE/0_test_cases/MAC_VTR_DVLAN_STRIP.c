#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* ETH_MACVTR double VLAN: inner tag stripping and RDES0 tag status.
 * Flow: MAC_VTR_DVLAN_flow.md.
 *
 * main.c includes this file AFTER the TX/RX rings are declared; do not add
 * it as a separate build source.
 *
 * MACVTR: EDVLP=1 ESVL=1 ETV=1 ERIVLT=1 ERSVLM=0 VL=100,
 *         EIVLS=11 (always strip inner), EIVLRXS=1, EVLRXS=1; MACPFR.VTFE=1.
 * Frame (68 B, FCS excluded):
 *   DA=SA=MAC0 | 0x88A8 | S-TCI | 0x8100 | C-TCI | 0x88B5 | pattern
 *   Case 1: outer VID 10 / inner VID 100 -> received, PL=64,
 *           inner tag (bytes 16..19) removed, RS0V=1.
 *           IVT/OVT and LT are logged only (DVLAN_STRIP_CHECK_TAGS=1 makes
 *           IVT/OVT pass/fail once the RDES0 layout is confirmed).
 * Path: internal MAC loopback (LM=1). */

#define DVLAN_STRIP_FRAME_LEN      68U      /* FCS excluded (CST=1) */
#define DVLAN_STRIP_TX_TIMEOUT_US  100000U
#define DVLAN_STRIP_RX_TIMEOUT_US  10000U
#define DVLAN_STRIP_VID            100U
#define DVLAN_STRIP_S_TCI          0x000AU  /* outer tag: PCP 0, DEI 0, VID 10 */
#define DVLAN_STRIP_C_TCI          0xB064U  /* inner tag: PCP 5, DEI 1, VID 100 */
#define DVLAN_STRIP_TAG_OFFSET     16U      /* inner tag: bytes 16..19 */
#define DVLAN_STRIP_CHECK_TAGS     0        /* 1: IVT/OVT pass/fail */

/* ETH_MACVTR / RDES3 bits not defined in eqos_hw.h */
#define DVLAN_STRIP_VTR_ESVL       (1u << 18)
#define DVLAN_STRIP_VTR_EDVLP      (1u << 26)
#define DVLAN_STRIP_VTR_ERIVLT     (1u << 27)
#define DVLAN_STRIP_VTR_EIVLS_ALL  (3u << 28)  /* 11: always strip inner tag */
#define DVLAN_STRIP_VTR_EIVLRXS    (1u << 31)
#define DVLAN_STRIP_RDES3_RS0V     (1u << 25)

static uint8_t dvlan_strip_tx_buf[DVLAN_STRIP_FRAME_LEN] __attribute__((aligned(32)));
static uint8_t dvlan_strip_rx_frame[EQOS_BUF_SIZE];

int eth_test_mac_vtr_dvlan_strip(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                                 struct eqos_desc *rx_desc,
                                 uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    static const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    const unsigned int case_count = 1; //case 1 received, inner tag stripped
    unsigned int failures = 0U;
    unsigned int passes = 0U;

    printf("\r\n ========== Begin MACVTR double VLAN inner strip test ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint8_t *mac0 = eth->cfg.mac_addr;
    const uint32_t saved_vtr = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG);
    const uint32_t saved_pfr = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);

    /* ---------- 1. Setup: MAC loopback, inner filter + strip + RX status ---------- */
    /* MACVTR whole register: VTIM=VTHM=0, EVLS=00 (outer tag kept), ERSVLM=0. */
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, 0U,
                        EQOS_MAC_CFG_LM | EQOS_MAC_CFG_FES | EQOS_MAC_CFG_DM |
                        EQOS_MAC_CFG_CST | EQOS_MAC_CFG_ACS |
                        EQOS_MAC_CFG_TE | EQOS_MAC_CFG_RE) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER,
                        EQOS_MAC_PKT_FILTER_PR | EQOS_MAC_PKT_FILTER_RA,
                        EQOS_MAC_PKT_FILTER_VTFE) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG, 0xFFFFFFFFU,
                        DVLAN_STRIP_VTR_EDVLP | DVLAN_STRIP_VTR_ESVL |
                        EQOS_MAC_VLAN_TAG_ETV | DVLAN_STRIP_VTR_ERIVLT |
                        DVLAN_STRIP_VTR_EIVLS_ALL | DVLAN_STRIP_VTR_EIVLRXS |
                        EQOS_MAC_VLAN_TAG_EVLRXS | DVLAN_STRIP_VID)) {
        printf("FAIL: MAC setup readback\r\n");
        return -1;
    }

    /* ---------- 2. Frame: outer tag 0x88A8, inner tag 0x8100 ---------- */
    const uint32_t tx_len = eth_construct_frame(dvlan_strip_tx_buf, sizeof(dvlan_strip_tx_buf),
                                                DVLAN_STRIP_FRAME_LEN, mac0, mac0,
                                                0x88A8U, pattern, sizeof(pattern));
    if (tx_len == 0U) {
        printf("FAIL: eth_construct_frame\r\n");
        return -1;
    }
    dvlan_strip_tx_buf[14] = (uint8_t)(DVLAN_STRIP_S_TCI >> 8);  /* outer TCI */
    dvlan_strip_tx_buf[15] = (uint8_t)DVLAN_STRIP_S_TCI;
    dvlan_strip_tx_buf[16] = 0x81U;                              /* inner TPID */
    dvlan_strip_tx_buf[17] = 0x00U;
    dvlan_strip_tx_buf[18] = (uint8_t)(DVLAN_STRIP_C_TCI >> 8);  /* inner TCI */
    dvlan_strip_tx_buf[19] = (uint8_t)DVLAN_STRIP_C_TCI;
    dvlan_strip_tx_buf[20] = 0x88U;                              /* EtherType 0x88B5 */
    dvlan_strip_tx_buf[21] = 0xB5U;

    for (unsigned int case_idx = 0U; case_idx < case_count; ++case_idx) {
        const uint32_t exp_len = tx_len - 4U;  /* inner tag removed */
        bool case_pass = true;

        printf("\r\n--- Case %u/%u: EIVLS=11 outer VID 10 / inner VID 100: expect "
               "RECEIVED, PL=%lu ---\r\n", case_idx + 1U, case_count,
               (unsigned long)exp_len);

        /* ---------- 3. Send ---------- */
        struct eth_tx_frame txf;
        if (eth_tx_frame_send(eth, tx_desc, dvlan_strip_tx_buf, tx_len, 0U, 0U,
                              DVLAN_STRIP_TX_TIMEOUT_US, &txf) != EQOS_OK) {
            printf("RESULT: FAIL (TX timeout, DMACSR=0x%08lX)\r\n",
                   (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
            ++failures;
            break; /* DMA may still own a descriptor: stop the test. */
        }
        printf("TX done: TDES3=0x%08lX ES=%lu\r\n", (unsigned long)txf.tdes3,
               (unsigned long)((txf.tdes3 & EQOS_TDES3_WB_ES) != 0U));
        if ((txf.tdes3 & EQOS_TDES3_WB_ES) != 0U)
            case_pass = false;

        /* ---------- 4. Receive ---------- */
        struct eth_rx_frame rxf;
        int rx_status = eth_rx_frame_receive(eth, rx_desc, rx_buf, dvlan_strip_rx_frame,
                                             sizeof(dvlan_strip_rx_frame),
                                             DVLAN_STRIP_RX_TIMEOUT_US, &rxf);
        if (rx_status != EQOS_OK) {
            printf("RESULT: FAIL (RX %s)\r\n",
                   rx_status == EQOS_ETIMEDOUT ? "timeout, frame dropped" :
                                                 "frame stalled mid-way");
            ++failures;
            break;
        }
        const uint32_t lt = rxf.rdes3 & EQOS_RDES3_WB_LT_MASK;
        const bool rs0v = (rxf.rdes3 & DVLAN_STRIP_RDES3_RS0V) != 0U;
        printf("RX done: RDES0=0x%08lX RDES3=0x%08lX\r\n",
               (unsigned long)rxf.rdes0, (unsigned long)rxf.rdes3);
        printf("  PL=%lu RS0V=%lu IVT=0x%04lX OVT=0x%04lX LT=0b%lu%lu%lu ES=%lu\r\n",
               (unsigned long)rxf.len, (unsigned long)rs0v,
               (unsigned long)(rxf.rdes0 >> 16), (unsigned long)(rxf.rdes0 & 0xFFFFU),
               (unsigned long)((lt >> 18) & 1U), (unsigned long)((lt >> 17) & 1U),
               (unsigned long)((lt >> 16) & 1U),
               (unsigned long)((rxf.rdes3 & EQOS_RDES3_WB_ES) != 0U));

        /* ---------- 5. ES, RS0V, length, frame without the inner tag ---------- */
        if ((rxf.rdes3 & EQOS_RDES3_WB_ES) != 0U) {
            printf("FAIL: ES=1\r\n");
            case_pass = false;
        }
        if (!rs0v) {
            printf("FAIL: RS0V=0\r\n");
            case_pass = false;
        }
        if (rxf.len != exp_len) {
            printf("FAIL: length %lu, expected %lu\r\n",
                   (unsigned long)rxf.len, (unsigned long)exp_len);
            case_pass = false;
        } else {
            for (uint32_t i = 0U; i < exp_len; ++i) {
                const uint32_t src = (i < DVLAN_STRIP_TAG_OFFSET) ? i : i + 4U;
                if (dvlan_strip_rx_frame[i] != dvlan_strip_tx_buf[src]) {
                    printf("FAIL at byte %lu: expected %02X, got %02X\r\n",
                           (unsigned long)i, (unsigned int)dvlan_strip_tx_buf[src],
                           (unsigned int)dvlan_strip_rx_frame[i]);
                    case_pass = false;
                    break;
                }
            }
        }

        /* ---------- 6. RDES0 tags: IVT = inner TCI, OVT = outer TCI ---------- */
        const bool tags_ok = (rxf.rdes0 >> 16) == DVLAN_STRIP_C_TCI &&
                             (rxf.rdes0 & 0xFFFFU) == DVLAN_STRIP_S_TCI;
        printf("  IVT/OVT %s expected 0x%04lX/0x%04lX%s\r\n",
               tags_ok ? "match" : "DIFFER from", (unsigned long)DVLAN_STRIP_C_TCI,
               (unsigned long)DVLAN_STRIP_S_TCI,
               DVLAN_STRIP_CHECK_TAGS ? "" : " (log only)");
        if (DVLAN_STRIP_CHECK_TAGS && !tags_ok)
            case_pass = false;
        if (case_pass)
            printf("Inner tag STRIPPED and reported as expected\r\n");

        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        if (case_pass)
            ++passes;
        else
            ++failures;
    }

    /* ---------- 7. Restore MACVTR and MACPFR ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG, saved_vtr);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, saved_pfr);

    printf("\r\n========== DVLAN inner strip test: %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, failures);
    if (passes + failures < case_count)
        printf("%u case(s) did not run; reset before rerunning.\r\n",
               case_count - passes - failures);
    return (int)failures;
}
