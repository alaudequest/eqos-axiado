#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* ETH_MACCR.GPSLCE (bit 23) giant packet size limit test.
 *
 * main.c includes this file AFTER the TX/RX rings are declared; do not add
 * it as a separate build source.
 *
 * ETH_MACECR.GPSL = 3000. With GPSLCE=1 a received packet longer than GPSL
 * (FCS included, as for the default 1518 limit) is reported as giant in the
 * last RX descriptor: RDES3.GP=1 (and ES=1). The frame is still delivered.
 * STM32H7 has no MMC giant/oversize counter, so RDES3.GP is the check.
 *   Case 1: 2996 B + FCS = 3000 B on the wire -> GP=0
 *   Case 2: 2997 B + FCS = 3001 B on the wire -> GP=1
 *
 * Settings needed only by this test (restored at the end):
 *  - MACCR.WD=1 / JD=1: otherwise RX watchdog and TX jabber cut frames at
 *    2048 bytes (JE=0) before the GPSL check. JE=0, S2KP=0.
 *  - MTL TXQ0/RXQ0 threshold mode (TSF=0 TTC=64, RSF=0 RTC=64): the frame is
 *    larger than the MTL FIFOs, so store-and-forward cannot hold it.
 *  - MACPFR: PR=RA=0, normal DA filtering.
 *  - TX uses a separate 4 KB buffer (tx_buf is only EQOS_BUF_SIZE), sent
 *    zero-copy by eth_tx_frame_send().
 *  - RX spans several EQOS_BUF_SIZE descriptors: eth_rx_frame_receive()
 *    reassembles FD..LD; status and total length come from the LD descriptor. */

#define GPSL_TEST_LIMIT       3000U    /* ETH_MACECR.GPSL, bytes incl. FCS */
#define GPSL_TEST_BUF_SIZE    4096U
#define GPSL_TEST_TIMEOUT_US  100000U
#define GPSL_TEST_ETHERTYPE   0x88B5U

static uint8_t gpsl_tx_buf[GPSL_TEST_BUF_SIZE] __attribute__((aligned(32)));
static uint8_t gpsl_rx_frame[GPSL_TEST_BUF_SIZE];

int eth_test_mac_ctrl_gpslce(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                             struct eqos_desc *rx_desc,
                             uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    /* frame size FCS excluded: case 1 = GPSL on the wire, case 2 = GPSL + 1 */
    static const uint32_t frame_size[2] = {GPSL_TEST_LIMIT - 4U, GPSL_TEST_LIMIT - 3U};
    static const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    const unsigned int case_count = 2U; //case 1 not giant, case 2 giant
    unsigned int failures = 0U;
    unsigned int passes = 0U;

    printf("\r\n ========== Begin MAC_CONFIGURATION GPSLCE test ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint8_t *mac0 = eth->cfg.mac_addr; /* DA = SA = MAC0 */

    /* ---------- 1. Setup: GPSL, MTL threshold mode, MACPFR, MACCR ---------- */
    const uint32_t macecr_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_EXT_CONFIGURATION);
    const uint32_t maccr_saved  = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);
    const uint32_t pfr_saved    = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
    const uint32_t mtl_tx_saved = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE);
    const uint32_t mtl_rx_saved = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE);
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_EXT_CONFIGURATION,
                        EQOS_MAC_EXT_CFG_GPSL_MASK, GPSL_TEST_LIMIT) ||
        !ETH_REG_UPDATE(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE,
                        EQOS_MTL_TXQ_TSF | EQOS_MTL_TXQ_TTC_MASK, EQOS_MTL_TXQ_TTC_64) ||
        !ETH_REG_UPDATE(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE,
                        EQOS_MTL_RXQ_RSF | EQOS_MTL_RXQ_RTC_MASK, EQOS_MTL_RXQ_RTC_64) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER,
                        EQOS_MAC_PKT_FILTER_PR | EQOS_MAC_PKT_FILTER_RA, 0U) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION,
                        EQOS_MAC_CFG_JE | EQOS_MAC_CFG_S2KP,
                        EQOS_MAC_CFG_LM | EQOS_MAC_CFG_FES | EQOS_MAC_CFG_DM |
                        EQOS_MAC_CFG_CST | EQOS_MAC_CFG_ACS |
                        EQOS_MAC_CFG_WD | EQOS_MAC_CFG_JD | EQOS_MAC_CFG_GPSLCE |
                        EQOS_MAC_CFG_TE | EQOS_MAC_CFG_RE)) {
        printf("FAIL: setup readback\r\n");
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, maccr_saved);
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
        AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE, mtl_tx_saved);
        AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, mtl_rx_saved);
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_EXT_CONFIGURATION, macecr_saved);
        return -1;
    }

    for (unsigned int case_idx = 0U; case_idx < case_count; ++case_idx) {
        const uint32_t size = frame_size[case_idx];
        const bool expect_giant = (case_idx == 1U);
        bool case_pass = true;

        printf("\r\n--- Case %u/%u: GPSLCE=1 GPSL=%lu, %lu+4 bytes on wire: expect %s ---\r\n",
               case_idx + 1U, case_count, (unsigned long)GPSL_TEST_LIMIT,
               (unsigned long)size, expect_giant ? "GIANT" : "NOT giant");

        /* ---------- 2. Build the frame ---------- */
        const uint32_t tx_len = eth_construct_frame(gpsl_tx_buf, sizeof(gpsl_tx_buf), size,
                                                    mac0, mac0, GPSL_TEST_ETHERTYPE,
                                                    pattern, sizeof(pattern));
        if (tx_len == 0U) {
            printf("RESULT: FAIL (eth_construct_frame)\r\n");
            ++failures;
            continue;
        }
        gpsl_tx_buf[14] = (uint8_t)(case_idx + 1U); /* case number */
        const uint32_t uni_before = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_UNI_GOOD);

        /* ---------- 3. Send: zero copy from the 4 KB buffer ---------- */
        struct eth_tx_frame txf;
        if (eth_tx_frame_send(eth, tx_desc, gpsl_tx_buf, tx_len, 0U, 0U,
                              GPSL_TEST_TIMEOUT_US, &txf) != EQOS_OK) {
            printf("RESULT: FAIL (TX timeout, DMACSR=0x%08lX)\r\n",
                   (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
            ++failures;
            break; /* DMA may still own a descriptor: stop the test. */
        }
        printf("TX done: TDES3=0x%08lX ES=%lu\r\n", (unsigned long)txf.tdes3,
               (unsigned long)((txf.tdes3 & EQOS_TDES3_WB_ES) != 0U));
        if ((txf.tdes3 & EQOS_TDES3_WB_ES) != 0U)
            case_pass = false;

        /* ---------- 4. Receive: multi-descriptor, status from LD ---------- */
        struct eth_rx_frame rxf;
        const int rx_status = eth_rx_frame_receive(eth, rx_desc, rx_buf, gpsl_rx_frame,
                                                   sizeof(gpsl_rx_frame),
                                                   GPSL_TEST_TIMEOUT_US, &rxf);
        if (rx_status != EQOS_OK) {
            printf("RESULT: FAIL (RX %s after %lu descriptor(s), DMACSR=0x%08lX)\r\n",
                   rx_status == EQOS_ETIMEDOUT ? "timeout" : "frame stalled mid-way",
                   (unsigned long)rxf.desc_used,
                   (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
            ++failures;
            break; /* A descriptor may still be owned by DMA. */
        }
        const uint32_t rdes3 = rxf.rdes3;
        const bool gp = (rdes3 & EQOS_RDES3_WB_GP) != 0U;
        const bool es = (rdes3 & EQOS_RDES3_WB_ES) != 0U;
        printf("RX done: %lu descriptor(s), %lu dropped, last RDES3=0x%08lX\r\n",
               (unsigned long)rxf.desc_used, (unsigned long)rxf.desc_dropped,
               (unsigned long)rdes3);
        printf("  GP=%lu ES=%lu RWT=%lu OE=%lu CE=%lu PL=%lu\r\n",
               (unsigned long)gp, (unsigned long)es,
               (unsigned long)((rdes3 & EQOS_RDES3_WB_RWT) != 0U),
               (unsigned long)((rdes3 & EQOS_RDES3_WB_OE) != 0U),
               (unsigned long)((rdes3 & EQOS_RDES3_WB_CE) != 0U),
               (unsigned long)rxf.len);
        printf("  Rx_Unicast_Packet_Count_Good [0x7C4]: %lu -> %lu (info only)\r\n",
               (unsigned long)uni_before,
               (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_UNI_GOOD));

        /* ---------- 5. Check GP/ES status, then the whole frame ---------- */
        if (gp != expect_giant || es != expect_giant) {          /* ES is set by GP */
            printf("FAIL: GP=%lu ES=%lu, expected %lu\r\n", (unsigned long)gp,
                   (unsigned long)es, (unsigned long)expect_giant);
            case_pass = false;
        }
        if ((rdes3 & (EQOS_RDES3_WB_RWT | EQOS_RDES3_WB_OE | EQOS_RDES3_WB_CE)) != 0U) {
            printf("FAIL: RWT/OE/CE error status\r\n");
            case_pass = false;
        }
        if (rxf.len != tx_len) {
            printf("FAIL: length %lu, expected %lu\r\n",
                   (unsigned long)rxf.len, (unsigned long)tx_len);
            case_pass = false;
        } else {
            for (uint32_t i = 0U; i < tx_len; ++i) {
                if (gpsl_rx_frame[i] != gpsl_tx_buf[i]) {
                    printf("FAIL at byte %lu: expected %02X, got %02X\r\n",
                           (unsigned long)i, (unsigned int)gpsl_tx_buf[i],
                           (unsigned int)gpsl_rx_frame[i]);
                    case_pass = false;
                    break;
                }
            }
        }
        if (case_pass)
            printf("Frame %s as expected\r\n", gp ? "flagged GIANT" : "NOT giant");
        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        if (case_pass)
            ++passes;
        else
            ++failures;
    }

    /* ---------- 6. Restore MACCR, MACPFR, MTL and GPSL ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, maccr_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
    AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE, mtl_tx_saved);
    AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, mtl_rx_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_EXT_CONFIGURATION, macecr_saved);

    printf("\r\n========== GPSLCE test: %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, failures);
    if (passes + failures < case_count)
        printf("%u case(s) did not run; reset before rerunning.\r\n",
               case_count - passes - failures);
    return (int)failures;
}
