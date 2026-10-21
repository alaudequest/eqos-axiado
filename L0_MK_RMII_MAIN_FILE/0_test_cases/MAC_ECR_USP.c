#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* ETH_MACECR.USP (bit 18) Unicast Slow Protocol packet detect test.
 *
 * main.c includes this file AFTER the TX/RX rings are declared and enables
 * LAN8742 PHY loopback (eth_test_enable_phy_loopback) for this test; do not
 * add it as a separate build source.
 *
 * SPEN=1 makes the MAC detect Slow Protocol frames (EtherType 0x8809) and
 * report them as RDES3.LT=0b111 (OAM), with subtype/code in RDES1.OPC[31:16].
 * USP=0: only DA = 01:80:C2:00:00:02 is detected.
 * USP=1: DA = MAC Address0 (unicast) is detected too.
 *   Case 1: SPEN=1 USP=0, DA=MAC0 -> LT=0b001 (ordinary type packet)
 *   Case 2: SPEN=1 USP=1, DA=MAC0 -> LT=0b111 (Slow Protocol detected)
 * Path: MAC TX -> RMII -> LAN8742 loopback -> RMII -> MAC RX (MACCR.LM=0). */

#define USP_TEST_FRAME_LEN    60U      /* min frame, FCS excluded */
#define USP_TEST_TIMEOUT_US   100000U
#define USP_TEST_ETHERTYPE    0x8809U  /* Slow Protocol */
#define USP_TEST_SUBTYPE      0x03U    /* OAM */

static uint8_t usp_tx_buf[USP_TEST_FRAME_LEN] __attribute__((aligned(32)));
static uint8_t usp_rx_frame[EQOS_BUF_SIZE];

int eth_test_mac_ecr_usp(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                         struct eqos_desc *rx_desc,
                         uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    static const struct {
        bool usp;               /* ETH_MACECR.USP */
        uint32_t expected_lt;   /* expected RDES3.LT */
        const char *name;
    } cases[] = {
        {false, EQOS_RDES3_WB_LT_TYPE, "unicast not detected (LT=0b001)"},
        {true,  EQOS_RDES3_WB_LT_OAM,  "unicast detected (LT=0b111)"},
    };
    /* Slow Protocol subtype 0x03 (OAM), flags 0x0000, code 0x00, then zeros. */
    static const uint8_t pattern[] = {USP_TEST_SUBTYPE, 0x00, 0x00, 0x00};
    const unsigned int case_count = 2; //2 cases in cases[]
    unsigned int failures = 0U;
    unsigned int passes = 0U;

    printf("\r\n ========== Begin MACECR USP test (PHY loopback) ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint8_t *mac0 = eth->cfg.mac_addr; /* DA = SA = MAC0 */

    /* ---------- 1. MAC setup: no MAC loopback, normal DA filtering ---------- */
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION,
                        EQOS_MAC_CFG_LM,
                        EQOS_MAC_CFG_FES | EQOS_MAC_CFG_DM |
                        EQOS_MAC_CFG_CST | EQOS_MAC_CFG_ACS |
                        EQOS_MAC_CFG_TE | EQOS_MAC_CFG_RE) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER,
                        EQOS_MAC_PKT_FILTER_PR, 0U)) {
        printf("FAIL: MAC setup readback\r\n");
        return -1;
    }

    /* ---------- 2. Build the Slow Protocol frame (same for both cases) ---------- */
    const uint32_t tx_len = eth_construct_frame(usp_tx_buf, sizeof(usp_tx_buf),
                                                USP_TEST_FRAME_LEN, mac0, mac0,
                                                USP_TEST_ETHERTYPE,
                                                pattern, sizeof(pattern));
    if (tx_len == 0U) {
        printf("FAIL: eth_construct_frame\r\n");
        return -1;
    }

    for (unsigned int case_idx = 0U; case_idx < case_count; ++case_idx) {
        const bool usp = cases[case_idx].usp;
        bool case_pass = true;

        /* ---------- 3. Configure ETH_MACECR: SPEN=1, USP per case ---------- */
        printf("\r\n--- Case %u/%u: SPEN=1 USP=%u: %s ---\r\n",
               case_idx + 1U, case_count, (unsigned int)usp, cases[case_idx].name);
        if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_EXT_CONFIGURATION,
                            EQOS_MAC_EXT_CFG_SPEN | EQOS_MAC_EXT_CFG_USP,
                            EQOS_MAC_EXT_CFG_SPEN | (usp ? EQOS_MAC_EXT_CFG_USP : 0U))) {
            printf("RESULT: FAIL (MACECR readback)\r\n");
            ++failures;
            continue;
        }

        /* ---------- 4. Send ---------- */
        struct eth_tx_frame txf;
        if (eth_tx_frame_send(eth, tx_desc, usp_tx_buf, tx_len, 0U, 0U,
                              USP_TEST_TIMEOUT_US, &txf) != EQOS_OK) {
            printf("RESULT: FAIL (TX timeout, DMACSR=0x%08lX)\r\n",
                   (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
            ++failures;
            break; /* DMA may still own a descriptor: stop the test. */
        }
        printf("TX done: TDES3=0x%08lX ES=%lu\r\n", (unsigned long)txf.tdes3,
               (unsigned long)((txf.tdes3 & EQOS_TDES3_WB_ES) != 0U));
        if ((txf.tdes3 & EQOS_TDES3_WB_ES) != 0U)
            case_pass = false;

        /* ---------- 5. Receive the looped-back frame ---------- */
        struct eth_rx_frame rxf;
        int rx_status = eth_rx_frame_receive(eth, rx_desc, rx_buf, usp_rx_frame,
                                             sizeof(usp_rx_frame),
                                             USP_TEST_TIMEOUT_US, &rxf);
        if (rx_status != EQOS_OK) {
            printf("RESULT: FAIL (RX %s, DMACSR=0x%08lX)\r\n",
                   rx_status == EQOS_ETIMEDOUT ? "timeout" : "frame stalled mid-way",
                   (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
            ++failures;
            break;
        }
        const uint32_t lt = rxf.rdes3 & EQOS_RDES3_WB_LT_MASK;
        printf("RX done: RDES1=0x%08lX RDES3=0x%08lX\r\n",
               (unsigned long)rxf.rdes1, (unsigned long)rxf.rdes3);
        printf("  LT=0b%lu%lu%lu OPC=0x%04lX ES=%lu PL=%lu\r\n",
               (unsigned long)((lt >> 18) & 1U), (unsigned long)((lt >> 17) & 1U),
               (unsigned long)((lt >> 16) & 1U),
               (unsigned long)(rxf.rdes1 >> EQOS_RDES1_WB_OPC_SHIFT),
               (unsigned long)((rxf.rdes3 & EQOS_RDES3_WB_ES) != 0U),
               (unsigned long)rxf.len);
        printf("RX frame (%lu bytes):\r\n", (unsigned long)rxf.len);
        for (uint32_t i = 0U; i < rxf.len && i < sizeof(usp_rx_frame); ++i) {
            printf("%02X ", (unsigned int)usp_rx_frame[i]);
            if ((i + 1U) % 16U == 0U)
                printf("\r\n");
        }
        printf("\r\n");

        /* ---------- 6. Check LT, then the whole frame ---------- */
        if (lt != cases[case_idx].expected_lt) {
            printf("FAIL: LT=0x%lX, expected 0x%lX\r\n", (unsigned long)(lt >> 16),
                   (unsigned long)(cases[case_idx].expected_lt >> 16));
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
                if (usp_rx_frame[i] != usp_tx_buf[i]) {
                    printf("FAIL at byte %lu: expected %02X, got %02X\r\n",
                           (unsigned long)i, (unsigned int)usp_tx_buf[i],
                           (unsigned int)usp_rx_frame[i]);
                    case_pass = false;
                    break;
                }
            }
        }
        if (case_pass)
            printf("Unicast Slow Protocol frame %s as expected\r\n",
                   usp ? "DETECTED" : "NOT detected");
        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        if (case_pass)
            ++passes;
        else
            ++failures;
    }

    /* ---------- 7. Restore: SPEN=0, USP=0 ---------- */
    uint32_t macecr = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_EXT_CONFIGURATION);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_EXT_CONFIGURATION,
                        macecr & ~(EQOS_MAC_EXT_CFG_SPEN | EQOS_MAC_EXT_CFG_USP));

    printf("\r\n========== USP test: %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, failures);
    if (passes + failures < case_count)
        printf("%u case(s) did not run; reset before rerunning.\r\n",
               case_count - passes - failures);
    return (int)failures;
}
