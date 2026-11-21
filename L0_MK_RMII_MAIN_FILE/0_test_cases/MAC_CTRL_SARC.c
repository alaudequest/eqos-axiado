#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* ETH_MACCR.SARC[30:28] source address insertion/replacement test.
 *
 * main.c includes this file AFTER the TX/RX rings are declared; do not add
 * it as a separate build source.
 *
 * Internal MAC loopback, one case per defined SARC encoding:
 *   010 insert  MAC Address0   TX buffer = DA|Type|Payload      -> RX = DA|MAC0|Type|Payload
 *   011 replace MAC Address0   TX buffer = DA|FAKE|Type|Payload -> RX = DA|MAC0|Type|Payload
 *   110 insert  MAC Address1   TX buffer = DA|Type|Payload      -> RX = DA|MAC1|Type|Payload
 *   111 replace MAC Address1   TX buffer = DA|FAKE|Type|Payload -> RX = DA|MAC1|Type|Payload
 * SARC bit 0: 0 insert, 1 replace. SARC bit 2: 0 MAC Address0, 1 MAC Address1.
 * TDES3: CPC=00 (CRC+pad insertion, so FCS covers the new SA) and SAIC=000,
 * so only MACCR.SARC controls the SA (fd_ctrl = 0 in eth_tx_frame_send()).
 * Settings needed only by this test (restored at the end): MACPFR PR=RA=0,
 * MAC Address1 = 02:11:22:33:44:55 with AE=1. */

#define SARC_TEST_RX_LEN      64U      /* expected RX length, FCS stripped (CST) */
#define SARC_TEST_TIMEOUT_US  100000U
#define SARC_TEST_ETHERTYPE   0x88B5U

static uint8_t sarc_tx_buf[SARC_TEST_RX_LEN] __attribute__((aligned(32)));
static uint8_t sarc_rx_frame[EQOS_BUF_SIZE];

int eth_test_mac_ctrl_sarc(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                           struct eqos_desc *rx_desc,
                           uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    static const uint32_t sarc[4] = {EQOS_MAC_CFG_SARC_INS_ADDR0, EQOS_MAC_CFG_SARC_REP_ADDR0,
                                     EQOS_MAC_CFG_SARC_INS_ADDR1, EQOS_MAC_CFG_SARC_REP_ADDR1};
    static const uint8_t mac1[6]    = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    static const uint8_t fake_sa[6] = {0x02, 0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    static const uint8_t pattern[]  = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    uint8_t expected[SARC_TEST_RX_LEN];
    const unsigned int case_count = 4U; //SARC 010, 011, 110, 111
    unsigned int failures = 0U;
    unsigned int passes = 0U;

    printf("\r\n ========== Begin MAC_CONFIGURATION SARC test ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint8_t *mac0 = eth->cfg.mac_addr; /* DA = MAC0: passes perfect filter without PR */

    /* ---------- 1. Setup: MACPFR, MAC Address1, MACCR loopback ---------- */
    const uint32_t maccr_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);
    const uint32_t pfr_saved   = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
    const uint32_t a1h_saved   = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS1_HIGH);
    const uint32_t a1l_saved   = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS1_LOW);
    /* MAC Address1 = SA source for SARC=110/111: AE=1, SA(bit 30)=0 (DA filter entry). */
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER,
                        EQOS_MAC_PKT_FILTER_PR | EQOS_MAC_PKT_FILTER_RA, 0U) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_ADDRESS1_HIGH, 0xFFFFFFFFU,
                        EQOS_MAC_ADDR_HIGH_AE | ((uint32_t)mac1[5] << 8) | mac1[4]) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_ADDRESS1_LOW, 0xFFFFFFFFU,
                        ((uint32_t)mac1[3] << 24) | ((uint32_t)mac1[2] << 16) |
                        ((uint32_t)mac1[1] << 8) | mac1[0]) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, EQOS_MAC_CFG_SARC_MASK,
                        EQOS_MAC_CFG_LM | EQOS_MAC_CFG_FES | EQOS_MAC_CFG_DM |
                        EQOS_MAC_CFG_CST | EQOS_MAC_CFG_ACS |
                        EQOS_MAC_CFG_TE | EQOS_MAC_CFG_RE)) {
        printf("FAIL: setup readback\r\n");
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, maccr_saved);
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS1_HIGH, a1h_saved);
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS1_LOW, a1l_saved);
        return -1;
    }

    for (unsigned int case_idx = 0U; case_idx < case_count; ++case_idx) {
        const uint32_t sarc_val = sarc[case_idx];
        const bool insert = (sarc_val & 1U) == 0U;          /* SARC bit 0: 0 insert, 1 replace */
        const unsigned int slot = (sarc_val >> 2) & 1U;     /* SARC bit 2: MAC Address0 / 1 */
        const uint8_t *expected_sa = (slot == 0U) ? mac0 : mac1;
        bool case_pass = true;

        /* ---------- 2. MACCR.SARC ---------- */
        printf("\r\n--- Case %u/%u: SARC=0b%lu%lu%lu: %s MAC Address%u ---\r\n",
               case_idx + 1U, case_count, (unsigned long)((sarc_val >> 2) & 1U),
               (unsigned long)((sarc_val >> 1) & 1U), (unsigned long)(sarc_val & 1U),
               insert ? "insert" : "replace SA with", slot);
        if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, EQOS_MAC_CFG_SARC_MASK,
                            sarc_val << EQOS_MAC_CFG_SARC_SHIFT)) {
            printf("RESULT: FAIL (MACCR readback)\r\n");
            ++failures;
            continue;
        }

        /* ---------- 3. Expected RX frame and TX buffer ---------- */
        memcpy(expected, mac0, 6U);                         /* DA */
        memcpy(expected + 6U, expected_sa, 6U);             /* SA set by the MAC */
        expected[12] = (uint8_t)(SARC_TEST_ETHERTYPE >> 8);
        expected[13] = (uint8_t)SARC_TEST_ETHERTYPE;
        expected[14] = (uint8_t)(case_idx + 1U);            /* case number */
        for (uint32_t i = 15U; i < SARC_TEST_RX_LEN; ++i)
            expected[i] = pattern[(i - 15U) % sizeof(pattern)];
        uint32_t tx_len;
        if (insert) {
            /* No SA in the buffer: DA | Type | payload (58 B). */
            memcpy(sarc_tx_buf, expected, 6U);
            memcpy(sarc_tx_buf + 6U, expected + 12U, SARC_TEST_RX_LEN - 12U);
            tx_len = SARC_TEST_RX_LEN - 6U;
        } else {
            /* Dummy SA that the MAC must overwrite (64 B). */
            memcpy(sarc_tx_buf, expected, SARC_TEST_RX_LEN);
            memcpy(sarc_tx_buf + 6U, fake_sa, 6U);
            tx_len = SARC_TEST_RX_LEN;
        }

        /* ---------- 4. Send (CPC=00, SAIC=000) and receive ---------- */
        struct eth_tx_frame txf;
        if (eth_tx_frame_send(eth, tx_desc, sarc_tx_buf, tx_len, 0U, 0U,
                              SARC_TEST_TIMEOUT_US, &txf) != EQOS_OK) {
            printf("RESULT: FAIL (TX timeout, DMACSR=0x%08lX)\r\n",
                   (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
            ++failures;
            break; /* DMA may still own a descriptor: stop the test. */
        }
        printf("TX done: %lu B, TDES3=0x%08lX ES=%lu\r\n", (unsigned long)tx_len,
               (unsigned long)txf.tdes3, (unsigned long)((txf.tdes3 & EQOS_TDES3_WB_ES) != 0U));
        if ((txf.tdes3 & EQOS_TDES3_WB_ES) != 0U)
            case_pass = false;

        struct eth_rx_frame rxf;
        const int rx_status = eth_rx_frame_receive(eth, rx_desc, rx_buf, sarc_rx_frame,
                                                   sizeof(sarc_rx_frame),
                                                   SARC_TEST_TIMEOUT_US, &rxf);
        if (rx_status != EQOS_OK) {
            printf("RESULT: FAIL (RX %s, DMACSR=0x%08lX)\r\n",
                   rx_status == EQOS_ETIMEDOUT ? "timeout" : "frame stalled mid-way",
                   (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
            ++failures;
            break; /* A descriptor may still be owned by DMA. */
        }
        printf("RX done: RDES3=0x%08lX ES=%lu PL=%lu\r\n", (unsigned long)rxf.rdes3,
               (unsigned long)((rxf.rdes3 & EQOS_RDES3_WB_ES) != 0U), (unsigned long)rxf.len);

        /* ---------- 5. Check ES, length, whole frame ---------- */
        if ((rxf.rdes3 & EQOS_RDES3_WB_ES) != 0U) {
            printf("FAIL: ES=1\r\n");
            case_pass = false;
        }
        if (rxf.len != SARC_TEST_RX_LEN) {
            printf("FAIL: length %lu, expected %lu\r\n",
                   (unsigned long)rxf.len, (unsigned long)SARC_TEST_RX_LEN);
            case_pass = false;
        } else {
            for (uint32_t i = 0U; i < SARC_TEST_RX_LEN; ++i) {
                if (sarc_rx_frame[i] != expected[i]) {
                    printf("FAIL at byte %lu: expected %02X, got %02X\r\n",
                           (unsigned long)i, (unsigned int)expected[i],
                           (unsigned int)sarc_rx_frame[i]);
                    case_pass = false;
                    break;
                }
            }
        }
        if (case_pass)
            printf("SA %s with MAC Address%u\r\n", insert ? "INSERTED" : "REPLACED", slot);
        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        if (case_pass)
            ++passes;
        else
            ++failures;
    }

    /* ---------- 6. Restore MACCR, MACPFR, MAC Address1 ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, maccr_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS1_HIGH, a1h_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS1_LOW, a1l_saved);

    printf("\r\n========== SARC test: %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, failures);
    if (passes + failures < case_count)
        printf("%u case(s) did not run; reset before rerunning.\r\n",
               case_count - passes - failures);
    return (int)failures;
}
