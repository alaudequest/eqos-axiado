#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* ETH_MACPFR.HUC (bit 1) unicast hash table filtering test.
 *
 * main.c includes this file AFTER the TX/RX rings are declared; do not add
 * it as a separate build source.
 *
 * HUC=1, HPF=0: unicast frames are filtered ONLY by the 64-bit hash table
 * (ETH_MACHT1R:HT0R); the perfect filter (MAC Address0) is not used.
 * DA = 02:00:00:00:00:01 (not MAC0), so the hash table is the only way in.
 * idx = eth_mac_hash_index(DA).
 *   Case 1 Hit : only bit idx set           -> RX, RDES2.HF=1, MADRM == idx
 *   Case 2 Miss: only bit (idx + 1) & 63 set -> dropped (RX timeout)
 * Path: internal MAC loopback (LM=1). */

#define HASH_TEST_FRAME_LEN      64U      /* FCS excluded */
#define HASH_TEST_TX_TIMEOUT_US  100000U
#define HASH_TEST_RX_TIMEOUT_US  10000U   /* miss case: no frame within 10 ms */
#define HASH_TEST_ETHERTYPE      0x88B5U

static uint8_t hash_tx_buf[HASH_TEST_FRAME_LEN] __attribute__((aligned(32)));
static uint8_t hash_rx_frame[EQOS_BUF_SIZE];

int eth_test_mac_pfr_hash_uc(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                             struct eqos_desc *rx_desc,
                             uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    static const uint8_t da[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    static const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    const unsigned int case_count = 2; //case 1 hit, case 2 miss
    unsigned int failures = 0U;
    unsigned int passes = 0U;

    printf("\r\n ========== Begin MACPFR HUC hash filter test ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint8_t *mac0 = eth->cfg.mac_addr;

    /* ---------- 1. Setup: MAC loopback, hash-only unicast filtering ---------- */
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, 0U,
                        EQOS_MAC_CFG_LM | EQOS_MAC_CFG_FES | EQOS_MAC_CFG_DM |
                        EQOS_MAC_CFG_CST | EQOS_MAC_CFG_ACS |
                        EQOS_MAC_CFG_TE | EQOS_MAC_CFG_RE) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER,
                        EQOS_MAC_PKT_FILTER_PR | EQOS_MAC_PKT_FILTER_RA |
                        EQOS_MAC_PKT_FILTER_PM | EQOS_MAC_PKT_FILTER_HPF |
                        EQOS_MAC_PKT_FILTER_HMC | EQOS_MAC_PKT_FILTER_DAIF,
                        EQOS_MAC_PKT_FILTER_HUC)) {
        printf("FAIL: MAC setup readback\r\n");
        return -1;
    }

    const uint32_t tx_len = eth_construct_frame(hash_tx_buf, sizeof(hash_tx_buf),
                                                HASH_TEST_FRAME_LEN, da, mac0,
                                                HASH_TEST_ETHERTYPE,
                                                pattern, sizeof(pattern));
    if (tx_len == 0U) {
        printf("FAIL: eth_construct_frame\r\n");
        return -1;
    }
    const uint32_t idx = eth_mac_hash_index(da);
    printf("DA %02X:%02X:%02X:%02X:%02X:%02X -> hash index %lu (HT%lu bit %lu)\r\n",
           da[0], da[1], da[2], da[3], da[4], da[5], (unsigned long)idx,
           (unsigned long)(idx >> 5), (unsigned long)(idx & 31U));

    for (unsigned int case_idx = 0U; case_idx < case_count; ++case_idx) {
        const bool expect_rx = (case_idx == 0U);
        const uint32_t ht_bit = expect_rx ? idx : ((idx + 1U) & 63U);
        bool case_pass = true;

        /* ---------- 2. Hash table: only ht_bit set ---------- */
        printf("\r\n--- Case %u/%u: HUC=1 HT bit %lu: expect %s ---\r\n",
               case_idx + 1U, case_count, (unsigned long)ht_bit,
               expect_rx ? "RECEIVED (hit)" : "DROPPED (miss)");
        const uint32_t ht0 = (ht_bit < 32U) ? (1UL << ht_bit) : 0U;
        const uint32_t ht1 = (ht_bit >= 32U) ? (1UL << (ht_bit - 32U)) : 0U;
        if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_HASH_TABLE0, 0xFFFFFFFFU, ht0) ||
            !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_HASH_TABLE1, 0xFFFFFFFFU, ht1)) {
            printf("RESULT: FAIL (hash table readback)\r\n");
            ++failures;
            continue;
        }

        /* ---------- 3. Send ---------- */
        struct eth_tx_frame txf;
        if (eth_tx_frame_send(eth, tx_desc, hash_tx_buf, tx_len, 0U, 0U,
                              HASH_TEST_TX_TIMEOUT_US, &txf) != EQOS_OK) {
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
        int rx_status = eth_rx_frame_receive(eth, rx_desc, rx_buf, hash_rx_frame,
                                             sizeof(hash_rx_frame),
                                             HASH_TEST_RX_TIMEOUT_US, &rxf);
        if (rx_status == EQOS_EIO) {
            printf("RESULT: FAIL (RX frame stalled mid-way)\r\n");
            ++failures;
            break;
        }
        if (rx_status == EQOS_ETIMEDOUT) {
            printf("RX: no frame within %lu us\r\n",
                   (unsigned long)HASH_TEST_RX_TIMEOUT_US);
            if (expect_rx) {
                printf("FAIL: frame dropped, expected hash hit\r\n");
                case_pass = false;
            } else {
                printf("Frame DROPPED by hash filter as expected\r\n");
            }
        } else {
            const uint32_t madrm = (rxf.rdes2 & EQOS_RDES2_WB_MADRM_MASK) >>
                                   EQOS_RDES2_WB_MADRM_SHIFT;
            const bool hf = (rxf.rdes2 & EQOS_RDES2_WB_HF) != 0U;
            printf("RX done: RDES2=0x%08lX RDES3=0x%08lX\r\n",
                   (unsigned long)rxf.rdes2, (unsigned long)rxf.rdes3);
            printf("  RS2V=%lu HF=%lu MADRM=%lu ES=%lu PL=%lu\r\n",
                   (unsigned long)((rxf.rdes3 & EQOS_RDES3_WB_RS2V) != 0U),
                   (unsigned long)hf, (unsigned long)madrm,
                   (unsigned long)((rxf.rdes3 & EQOS_RDES3_WB_ES) != 0U),
                   (unsigned long)rxf.len);
            if (!expect_rx) {
                printf("FAIL: frame received, expected hash miss\r\n");
                case_pass = false;
            } else {
                /* ---------- 5. Hit checks, then the whole frame ---------- */
                if ((rxf.rdes3 & EQOS_RDES3_WB_RS2V) == 0U || !hf) {
                    printf("FAIL: RS2V/HF not set\r\n");
                    case_pass = false;
                }
                if (madrm != idx) {
                    printf("FAIL: MADRM=%lu, computed index %lu\r\n",
                           (unsigned long)madrm, (unsigned long)idx);
                    case_pass = false;
                }
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
                        if (hash_rx_frame[i] != hash_tx_buf[i]) {
                            printf("FAIL at byte %lu: expected %02X, got %02X\r\n",
                                   (unsigned long)i, (unsigned int)hash_tx_buf[i],
                                   (unsigned int)hash_rx_frame[i]);
                            case_pass = false;
                            break;
                        }
                    }
                }
                if (case_pass)
                    printf("Frame RECEIVED through hash filter as expected\r\n");
            }
        }
        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        if (case_pass)
            ++passes;
        else
            ++failures;
    }

    /* ---------- 6. Restore: HUC=0, hash table cleared ---------- */
    uint32_t pfr = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER,
                        pfr & ~EQOS_MAC_PKT_FILTER_HUC);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_HASH_TABLE0, 0U);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_HASH_TABLE1, 0U);

    printf("\r\n========== HUC hash filter test: %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, failures);
    if (passes + failures < case_count)
        printf("%u case(s) did not run; reset before rerunning.\r\n",
               case_count - passes - failures);
    return (int)failures;
}
