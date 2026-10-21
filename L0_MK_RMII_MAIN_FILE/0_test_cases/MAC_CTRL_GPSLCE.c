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
 *  - TX uses a separate 4 KB buffer (tx_buf is only EQOS_BUF_SIZE), sent
 *    zero-copy by eth_tx_frame_send().
 *  - RX spans several EQOS_BUF_SIZE descriptors: eth_rx_frame_receive()
 *    reassembles FD..LD; status and total length come from the LD descriptor. */

#define GPSL_TEST_LIMIT       3000U    /* ETH_MACECR.GPSL, bytes incl. FCS */
#define GPSL_TEST_BUF_SIZE    4096U
#define GPSL_TEST_TIMEOUT_US  100000U
#define GPSL_TEST_ETHERTYPE   0x88B5U
#define GPSL_TEST_DUMP_BYTES  32U      /* bytes printed from the start/end */

static uint8_t gpsl_tx_buf[GPSL_TEST_BUF_SIZE] __attribute__((aligned(32)));
static uint8_t gpsl_rx_frame[GPSL_TEST_BUF_SIZE];

static void gpsl_test_dump(const char *tag, const uint8_t *p, uint32_t len)
{
    /* Print only the head and tail: a full 3000-byte dump is slow on UART. */
    uint32_t head = (len < GPSL_TEST_DUMP_BYTES) ? len : GPSL_TEST_DUMP_BYTES;
    printf("%s (%lu bytes), first %lu:\r\n", tag, (unsigned long)len,
           (unsigned long)head);
    for (uint32_t i = 0U; i < head; ++i) {
        printf("%02X ", (unsigned int)p[i]);
        if ((i + 1U) % 16U == 0U)
            printf("\r\n");
    }
    if (len > 2U * GPSL_TEST_DUMP_BYTES) {
        printf("... last %lu:\r\n", (unsigned long)GPSL_TEST_DUMP_BYTES);
        for (uint32_t i = len - GPSL_TEST_DUMP_BYTES; i < len; ++i) {
            printf("%02X ", (unsigned int)p[i]);
            if ((i + 1U) % 16U == 0U)
                printf("\r\n");
        }
    }
    printf("\r\n");
}

int eth_test_mac_ctrl_gpslce(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                             struct eqos_desc *rx_desc,
                             uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    static const struct {
        uint32_t frame_size;   /* eth_construct_frame size, FCS excluded */
        bool expect_giant;     /* expected RDES3.GP */
        const char *name;
    } cases[] = {
        {GPSL_TEST_LIMIT - 4U, false, "frame = GPSL (not giant)"},
        {GPSL_TEST_LIMIT - 3U, true,  "frame = GPSL + 1 (giant)"},
    };
    static const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    const unsigned int case_count = 2; //2 cases in cases[]
    unsigned int failures = 0U;
    unsigned int passes = 0U;

    printf("\r\n ========== Begin MAC_CONFIGURATION GPSLCE test ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint8_t *mac0 = eth->cfg.mac_addr; /* DA = SA = MAC0 */

    /* ---------- 1. Program GPSL in ETH_MACECR ---------- */
    const uint32_t macecr_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_EXT_CONFIGURATION);
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_EXT_CONFIGURATION,
                        EQOS_MAC_EXT_CFG_GPSL_MASK, GPSL_TEST_LIMIT)) {
        printf("FAIL: GPSL readback\r\n");
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_EXT_CONFIGURATION, macecr_saved);
        return -1;
    }

    /* ---------- 2. MTL threshold mode: frame is larger than the FIFOs ---------- */
    const uint32_t mtl_tx_saved = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE);
    const uint32_t mtl_rx_saved = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE);
    /* TSF=0, TTC=64 bytes / RSF=0, RTC=64 bytes. */
    bool mtl_ok = ETH_REG_UPDATE(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE,
                                 EQOS_MTL_TXQ_TSF | EQOS_MTL_TXQ_TTC_MASK,
                                 EQOS_MTL_TXQ_TTC_64);
    mtl_ok &= ETH_REG_UPDATE(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE,
                             EQOS_MTL_RXQ_RSF | EQOS_MTL_RXQ_RTC_MASK,
                             EQOS_MTL_RXQ_RTC_64);
    if (!mtl_ok) {
        printf("FAIL: MTL readback\r\n");
        AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE, mtl_tx_saved);
        AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, mtl_rx_saved);
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_EXT_CONFIGURATION, macecr_saved);
        return -1;
    }

    /* Normal DA filtering: no PR, no RA. */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, 0U);

    for (unsigned int case_idx = 0U; case_idx < case_count; ++case_idx) {
        const uint32_t size = cases[case_idx].frame_size;
        const bool expect_giant = cases[case_idx].expect_giant;
        bool case_pass = true;

        /* ---------- 3. Configure MACCR: loopback + GPSLCE, one write ---------- */
        printf("\r\n--- Case %u/%u: GPSLCE=1 GPSL=%lu, %lu+4 bytes on wire: %s ---\r\n",
               case_idx + 1U, case_count, (unsigned long)GPSL_TEST_LIMIT,
               (unsigned long)size, cases[case_idx].name);
        if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION,
                            EQOS_MAC_CFG_JE | EQOS_MAC_CFG_S2KP,
                            EQOS_MAC_CFG_LM | EQOS_MAC_CFG_FES | EQOS_MAC_CFG_DM |
                            EQOS_MAC_CFG_CST | EQOS_MAC_CFG_ACS |
                            EQOS_MAC_CFG_WD | EQOS_MAC_CFG_JD | EQOS_MAC_CFG_GPSLCE |
                            EQOS_MAC_CFG_TE | EQOS_MAC_CFG_RE)) {
            printf("RESULT: FAIL (MACCR readback)\r\n");
            ++failures;
            continue;
        }

        /* ---------- 4. Build the frame ---------- */
        uint32_t tx_len = eth_construct_frame(gpsl_tx_buf, sizeof(gpsl_tx_buf), size,
                                              mac0, mac0, GPSL_TEST_ETHERTYPE,
                                              pattern, sizeof(pattern));
        if (tx_len == 0U) {
            printf("RESULT: FAIL (eth_construct_frame)\r\n");
            ++failures;
            continue;
        }
        gpsl_tx_buf[14] = (uint8_t)(case_idx + 1U); /* case number */
        gpsl_test_dump("TX frame", gpsl_tx_buf, tx_len);
        const uint32_t uni_before = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_UNI_GOOD);

        /* ---------- 5. Send: generic TX, zero copy from the 4 KB buffer ---------- */
        struct eth_tx_frame txf;
        int tx_status = eth_tx_frame_send(eth, tx_desc, gpsl_tx_buf, tx_len, 0U, 0U,
                                          GPSL_TEST_TIMEOUT_US, &txf);
        if (tx_status != EQOS_OK) {
            printf("RESULT: FAIL (TX timeout, DMACSR=0x%08lX)\r\n",
                   (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
            ++failures;
            break; /* DMA may still own a descriptor: stop the test. */
        }
        const uint32_t tdes3 = txf.tdes3;
        printf("TX done: TDES3=0x%08lX ES=%lu\r\n", (unsigned long)tdes3,
               (unsigned long)((tdes3 & EQOS_TDES3_WB_ES) != 0U));
        if ((tdes3 & EQOS_TDES3_WB_ES) != 0U)
            case_pass = false;

        /* ---------- 6. Receive: generic multi-descriptor receive ---------- */
        struct eth_rx_frame rxf;
        int rx_status = eth_rx_frame_receive(eth, rx_desc, rx_buf, gpsl_rx_frame,
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
        if (rxf.desc_dropped != 0U)
            printf("RX: %lu stray descriptor(s) dropped before this frame\r\n",
                   (unsigned long)rxf.desc_dropped);
        const uint32_t last_rdes3 = rxf.rdes3;
        const uint32_t used = rxf.desc_used;

        const uint32_t uni_after = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_UNI_GOOD);
        const uint32_t rx_len = last_rdes3 & EQOS_RDES3_WB_PL_MASK;
        const bool gp = (last_rdes3 & EQOS_RDES3_WB_GP) != 0U;
        printf("RX done: %lu descriptor(s), last RDES3=0x%08lX\r\n",
               (unsigned long)used, (unsigned long)last_rdes3);
        printf("  GP=%lu ES=%lu RWT=%lu OE=%lu CE=%lu PL=%lu\r\n",
               (unsigned long)gp,
               (unsigned long)((last_rdes3 & EQOS_RDES3_WB_ES) != 0U),
               (unsigned long)((last_rdes3 & EQOS_RDES3_WB_RWT) != 0U),
               (unsigned long)((last_rdes3 & EQOS_RDES3_WB_OE) != 0U),
               (unsigned long)((last_rdes3 & EQOS_RDES3_WB_CE) != 0U),
               (unsigned long)rx_len);
        printf("  Rx_Unicast_Packet_Count_Good [0x7C4]: %lu -> %lu (info only)\r\n",
               (unsigned long)uni_before, (unsigned long)uni_after);
        gpsl_test_dump("RX frame", gpsl_rx_frame,
                       rx_len < sizeof(gpsl_rx_frame) ? rx_len : sizeof(gpsl_rx_frame));

        /* ------eth_rx_frame_receive---- 7. Check GP status, then the whole frame ---------- */
        if (gp != expect_giant) {
            printf("FAIL: GP=%lu, expected %lu\r\n", (unsigned long)gp,
                   (unsigned long)expect_giant);
            case_pass = false;
        }
        if ((last_rdes3 & (EQOS_RDES3_WB_RWT | EQOS_RDES3_WB_OE | EQOS_RDES3_WB_CE)) != 0U) {
            printf("FAIL: RWT/OE/CE error status\r\n");
            case_pass = false;
        }
        if (!expect_giant && (last_rdes3 & EQOS_RDES3_WB_ES) != 0U) {
            printf("FAIL: ES=1 on a non-giant frame\r\n");
            case_pass = false;
        }
        if (rx_len != tx_len) {
            printf("FAIL: length %lu, expected %lu\r\n",
                   (unsigned long)rx_len, (unsigned long)tx_len);
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

    /* ---------- 8. Restore MACCR, MTL and GPSL ---------- */
    uint32_t maccr = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION,
                        maccr & ~(EQOS_MAC_CFG_GPSLCE | EQOS_MAC_CFG_WD | EQOS_MAC_CFG_JD));
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
