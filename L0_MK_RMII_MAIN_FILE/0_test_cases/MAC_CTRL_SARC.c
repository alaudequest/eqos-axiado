#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* ETH_MACCR.SARC[30:28] source address insertion/replacement test.
 *
 * main.c includes this file AFTER struct eqos_desc and the TX/RX rings are
 * declared; do not add it as a separate build source.
 *
 * One function runs all four defined encodings in internal MAC loopback:
 *   010 insert  MAC Address0   TX buffer = DA|Type|Payload      -> RX = DA|MAC0|Type|Payload
 *   011 replace MAC Address0   TX buffer = DA|FAKE|Type|Payload -> RX = DA|MAC0|Type|Payload
 *   110 insert  MAC Address1   TX buffer = DA|Type|Payload      -> RX = DA|MAC1|Type|Payload
 *   111 replace MAC Address1   TX buffer = DA|FAKE|Type|Payload -> RX = DA|MAC1|Type|Payload
 * Insert: the application frame has no SA; MAC adds 6 bytes after DA.
 * Replace: the application frame carries a dummy SA; MAC overwrites it.
 * TDES3: CPC=00 (CRC+pad insertion, needed so FCS covers the new SA) and
 * SAIC=000, so only the MACCR.SARC field controls the SA. */

#define SARC_TEST_RX_LEN      64U      /* expected RX length, FCS stripped (CST) */
#define SARC_TEST_TIMEOUT_US  100000U
#define SARC_TEST_ETHERTYPE   0x88B5U

static void sarc_test_dump(const char *tag, const uint8_t *p, uint32_t len)
{
    printf("%s (%lu bytes):\r\n", tag, (unsigned long)len);
    for (uint32_t i = 0U; i < len; ++i) {
        printf("%02X ", (unsigned int)p[i]);
        if ((i + 1U) % 16U == 0U)
            printf("\r\n");
    }
    if ((len % 16U) != 0U)
        printf("\r\n");
}

int eth_test_mac_ctrl_sarc(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                           struct eqos_desc *rx_desc,
                           uint8_t tx_buf[][EQOS_BUF_SIZE],
                           uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    static const struct {
        uint32_t sarc;      /* 3-bit SARC value */
        bool insert;        /* true: insert, false: replace */
        uint8_t addr_slot;  /* 0: MAC Address0, 1: MAC Address1 */
        const char *name;
    } cases[] = {
        {EQOS_MAC_CFG_SARC_INS_ADDR0, true,  0U, "Insert MAC Address0 into SA"},
        {EQOS_MAC_CFG_SARC_REP_ADDR0, false, 0U, "Replace SA with MAC Address0"},
        {EQOS_MAC_CFG_SARC_INS_ADDR1, true,  1U, "Insert MAC Address1 into SA"},
        {EQOS_MAC_CFG_SARC_REP_ADDR1, false, 1U, "Replace SA with MAC Address1"},
    };
    static const uint8_t mac1[6]    = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    static const uint8_t fake_sa[6] = {0x02, 0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    static const uint8_t pattern[]  = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    uint8_t expected[SARC_TEST_RX_LEN];
    uint8_t frame[SARC_TEST_RX_LEN];
    const unsigned int case_count = 4; //4 cases in cases[]
    unsigned int failures = 0U;
    unsigned int passes = 0U;

    printf("\r\n ========== Begin MAC_CONFIGURATION SARC test ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL ||
        tx_buf == NULL || rx_buf == NULL)
        return -1;
    const uint8_t *mac0 = eth->cfg.mac_addr;
    const uint8_t *da = mac0; /* DA = MAC0: passes perfect filter without PR. */

    /* Normal DA filtering: no PR, no RA. */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, 0U);
    /* MAC Address1 is the source for SARC=110/111. AE=1 enables the slot;
     * SA(bit 30)=0 keeps it a DA filter entry only. Same byte packing as slot 0. */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS1_HIGH,
                        EQOS_MAC_ADDR_HIGH_AE | ((uint32_t)mac1[5] << 8) | mac1[4]);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS1_LOW,
                        ((uint32_t)mac1[3] << 24) | ((uint32_t)mac1[2] << 16) |
                        ((uint32_t)mac1[1] << 8) | mac1[0]);

    for (unsigned int case_idx = 0U; case_idx < case_count; ++case_idx) {
        const bool insert = cases[case_idx].insert;
        const uint8_t *expected_sa = (cases[case_idx].addr_slot == 0U) ? mac0 : mac1;
        bool case_pass = true;

        /* ---------- 1. Configure MACCR: loopback + SARC, one write ---------- */
        printf("\r\n--- Case %u/%u: SARC=0b%lu%lu%lu: %s ---\r\n",
               case_idx + 1U, case_count,
               (unsigned long)((cases[case_idx].sarc >> 2) & 1U),
               (unsigned long)((cases[case_idx].sarc >> 1) & 1U),
               (unsigned long)(cases[case_idx].sarc & 1U), cases[case_idx].name);
        if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION,
                            EQOS_MAC_CFG_SARC_MASK,
                            EQOS_MAC_CFG_LM | EQOS_MAC_CFG_FES | EQOS_MAC_CFG_DM |
                            EQOS_MAC_CFG_CST | EQOS_MAC_CFG_ACS |
                            EQOS_MAC_CFG_TE | EQOS_MAC_CFG_RE |
                            (cases[case_idx].sarc << EQOS_MAC_CFG_SARC_SHIFT))) {
            printf("RESULT: FAIL (MACCR readback)\r\n");
            ++failures;
            continue;
        }

        /* ---------- 2. Build the frame ---------- */
        /* expected = what RX should see: DA | expected SA | Type | payload. */
        memcpy(expected, da, 6U);
        memcpy(expected + 6U, expected_sa, 6U);
        expected[12] = (uint8_t)(SARC_TEST_ETHERTYPE >> 8); //0x88B5 test type
        expected[13] = (uint8_t)SARC_TEST_ETHERTYPE;
        expected[14] = (uint8_t)(case_idx + 1U); /* case number */
        for (uint32_t i = 15U; i < SARC_TEST_RX_LEN; ++i)
            expected[i] = pattern[(i - 15U) % sizeof(pattern)];

        uint32_t tx_len;
        if (insert) {
            /* No SA in the buffer: DA | Type | payload (58 bytes). */
            memcpy(frame, expected, 6U);
            memcpy(frame + 6U, expected + 12U, SARC_TEST_RX_LEN - 12U);
            tx_len = SARC_TEST_RX_LEN - 6U;
        } else {
            /* Dummy SA that the MAC must overwrite (64 bytes). */
            memcpy(frame, expected, SARC_TEST_RX_LEN);
            memcpy(frame + 6U, fake_sa, 6U);
            tx_len = SARC_TEST_RX_LEN;
        }
        sarc_test_dump("TX buffer", frame, tx_len);

        /* ---------- 3. Send ---------- */
        uint32_t tidx = eth->tx_head;
        struct eqos_desc *tx = &tx_desc[tidx];
        if ((tx->des3 & EQOS_TDES3_OWN) != 0U) {
            printf("RESULT: FAIL (TX descriptor %lu still owned by DMA)\r\n",
                   (unsigned long)tidx);
            ++failures;
            break;
        }
        memcpy(tx_buf[tidx], frame, tx_len);
        tx->des0 = (uint32_t)(uintptr_t)tx_buf[tidx];
        tx->des1 = 0U;
        tx->des2 = tx_len & EQOS_TDES2_B1L_MASK;
        __DSB();
        /* CPC=00 and SAIC=000 (both zero): register SARC alone controls SA. */
        tx->des3 = EQOS_TDES3_OWN | EQOS_TDES3_FD | EQOS_TDES3_LD |
                   (tx_len & EQOS_TDES3_FL_MASK);
        __DSB();
        eth->tx_head = (tidx + 1U) & (EQOS_TX_DESC_COUNT - 1U);
        AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR,
                            (uint32_t)(uintptr_t)&tx_desc[eth->tx_head]);

        uint32_t elapsed = 0U;
        uint32_t tdes3;
        do {
            tdes3 = tx->des3;
            if ((tdes3 & EQOS_TDES3_OWN) == 0U)
                break;
            eqos_udelay(1U);
        } while (++elapsed < SARC_TEST_TIMEOUT_US);
        if ((tdes3 & EQOS_TDES3_OWN) != 0U) {
            printf("RESULT: FAIL (TX timeout, TDES3=0x%08lX DMACSR=0x%08lX)\r\n",
                   (unsigned long)tdes3,
                   (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
            ++failures;
            break; /* DMA still owns the descriptor: stop the test. */
        }
        printf("TX done: TDES3=0x%08lX ES=%lu\r\n", (unsigned long)tdes3,
               (unsigned long)((tdes3 & EQOS_TDES3_WB_ES) != 0U));
        if ((tdes3 & EQOS_TDES3_WB_ES) != 0U)
            case_pass = false;

        /* ---------- 4. Receive the looped-back frame ---------- */
        uint32_t ridx = eth->rx_head;
        struct eqos_desc *rx = &rx_desc[ridx];
        uint32_t rdes3;
        elapsed = 0U;
        do {
            rdes3 = rx->des3;
            if ((rdes3 & EQOS_RDES3_OWN) == 0U)
                break;
            eqos_udelay(1U);
        } while (++elapsed < SARC_TEST_TIMEOUT_US);
        if ((rdes3 & EQOS_RDES3_OWN) != 0U) {
            printf("RESULT: FAIL (RX timeout, RDES3=0x%08lX DMACSR=0x%08lX)\r\n",
                   (unsigned long)rdes3,
                   (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
            ++failures;
            break; /* Do not read a DMA-owned buffer. */
        }
        __DSB();
        uint32_t rx_len = rdes3 & EQOS_RDES3_WB_PL_MASK;
        const uint8_t *rxp = rx_buf[ridx];
        printf("RX done: RDES3=0x%08lX ES=%lu FD=%lu LD=%lu PL=%lu\r\n",
               (unsigned long)rdes3,
               (unsigned long)((rdes3 & EQOS_RDES3_WB_ES) != 0U),
               (unsigned long)((rdes3 & EQOS_RDES3_WB_FD) != 0U),
               (unsigned long)((rdes3 & EQOS_RDES3_WB_LD) != 0U),
               (unsigned long)rx_len);

        if ((rdes3 & (EQOS_RDES3_WB_CTXT | EQOS_RDES3_WB_ES)) != 0U ||
            (rdes3 & (EQOS_RDES3_WB_FD | EQOS_RDES3_WB_LD)) !=
                (EQOS_RDES3_WB_FD | EQOS_RDES3_WB_LD) ||
            rx_len == 0U || rx_len > EQOS_BUF_SIZE) {
            printf("RX descriptor status error\r\n");
            case_pass = false;
        } else {
            sarc_test_dump("RX frame", rxp, rx_len);

            /* ---------- 5. Compare the whole RX frame with the expected frame ---------- */
            if (rx_len != SARC_TEST_RX_LEN) {
                printf("FAIL: length %lu, expected %lu\r\n",
                       (unsigned long)rx_len, (unsigned long)SARC_TEST_RX_LEN);
                case_pass = false;
            } else {
                for (uint32_t i = 0U; i < SARC_TEST_RX_LEN; ++i) {
                    if (rxp[i] != expected[i]) {
                        printf("FAIL at byte %lu: expected %02X, got %02X\r\n",
                               (unsigned long)i, (unsigned int)expected[i],
                               (unsigned int)rxp[i]);
                        case_pass = false;
                        break;
                    }
                }
            }
            if (case_pass)
                printf("SA %s with MAC Address%u\r\n", insert ? "INSERTED" : "REPLACED",
                       (unsigned int)cases[case_idx].addr_slot);
        }
        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        if (case_pass)
            ++passes;
        else
            ++failures;

        /* Re-arm the RX descriptor (all four words) and give it back to DMA. */
        rx->des0 = (uint32_t)(uintptr_t)rx_buf[ridx];
        rx->des1 = 0U;
        rx->des2 = 0U;
        __DSB();
        rx->des3 = EQOS_RDES3_OWN | EQOS_RDES3_BUF1V;
        __DSB();
        AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_TAIL_PTR,
                            (uint32_t)(uintptr_t)rx);
        eth->rx_head = (ridx + 1U) & (EQOS_RX_DESC_COUNT - 1U);
    }

    /* ---------- 6. Restore SARC=000 ---------- */
    uint32_t maccr = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION,
                        maccr & ~EQOS_MAC_CFG_SARC_MASK);

    printf("\r\n========== SARC test: %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, failures);
    if (passes + failures < case_count)
        printf("%u case(s) did not run; reset before rerunning.\r\n",
               case_count - passes - failures);
    return (int)failures;
}
