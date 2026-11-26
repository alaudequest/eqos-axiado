#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* MAC_Q0_Tx_Flow_Ctrl pause frame test, measured with the MAC hardware timestamps
 * (RX/TX descriptor write-back). Flow: 1_test_flow_markdown/MAC_Q0_TX_FLOW_CTRL_PAUSE_TIMESTAMP_flow.md
 *
 * eth_function_stm32.c includes this file; do not add it as a separate build source. Internal MAC loopback, 100 Mbps full duplex.
 * 1. TFE=1, PT=pause time; write FCB_BPA=1 -> the MAC sends a pause frame
 *    (DA 01-80-C2-00-00-01, type 0x8808, opcode 0x0001, PT). FCB_BPA self-clears.
 * 2. RFE=1: MAC RX decodes the looped-back pause frame and holds MAC TX for
 *    PT x 512 bit times (5.12 us per quantum at 100 Mbps).
 *    PCF=10 forwards the pause frame to software; TSENALL=1 gives it an RX
 *    timestamp (context descriptor): T0 = time at the pause frame SFD.
 * 3. A data frame is queued at once with eth_egress_packet_ex()
 *    (TDES2.TTSE=1). When TDES3.TTSS=1 its TX timestamp (TDES0 ns / TDES1 s)
 *    is T1 = time at the data frame SFD after the pause.
 * 4. delta = T1 - T0 = PT x 5.12 us + 5.12 us (64-byte pause frame).
 *   Case 1: PT=0xFFFF -> 335544.32 us. The pause timer runs on the RMII
 *           reference clock (PHY crystal), the timestamp on HCLK (HSI here):
 *           case 1 measures the timestamp clock error in ppm (limit +-2%).
 *   Case 2: PT=0x0100 -> 1315.84 us x (1 + case 1 error), window +-2 us:
 *           proves the pause time follows the PT value.
 * Nothing is printed between T0 and queuing the data frame (UART is slow).
 * Timestamp unit (EQOS databook 18.12.1): fine update, 10 ns per tick (SSINC=10,
 * addend from HCLK), TSCTRLSSR=1 (low word = ns, rolls over at 1 s), start 0 s 0 ns.
 * Registers restored at the end (addend not restored). */

#define TXFC_TX_TIMEOUT_US     1000000U /* longer than the 335 ms pause */
#define TXFC_RX_TIMEOUT_US     10000U
#define TXFC_FCB_TIMEOUT_US    10000U
#define TXFC_TS_POLL_US        1000U    /* TSADDREG / TSINIT self-clear timeout */
#define TXFC_FRAME_LEN         64U
#define TXFC_WIN_NS            2000     /* +-2 us (case 2) */
#define TXFC_CLK_TOL_PPM       20000    /* +-2 %: HSI tolerance (case 1) */
#define TXFC_TS_SSINC_NS       10U      /* 100 MHz virtual timestamp clock */
/* PT x 512 bit times + 512-bit pause frame, 10 ns per bit at 100 Mbps */
#define TXFC_EXPECTED_NS(pt)   (((int64_t)(pt) * 512 + 512) * 10)

/* Registers not defined in eqos_hw.h (EQoS databook names) */
#define TXFC_MAC_SYS_TIME_SEC_UPDATE   0xb10u   /* MAC_System_Time_Seconds_Update */
#define TXFC_MAC_SYS_TIME_NSEC_UPDATE  0xb14u   /* MAC_System_Time_Nanoseconds_Update */

static uint8_t txfc_tx_buf[TXFC_FRAME_LEN] __attribute__((aligned(32)));
static uint8_t txfc_rx_frame[EQOS_BUF_SIZE];

/* Set a self-clearing bit of a MAC register, poll until it clears.
 * Returns false (and prints what) on timeout. */
static bool txfc_set_and_poll(uint32_t offset, uint32_t bit, uint32_t timeout_us, const char *what)
{
    uint32_t elapsed = 0U;

    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, offset, AX_BASE_READ_REG32(EQOS_MAC_BASE, offset) | bit);
    while ((AX_BASE_READ_REG32(EQOS_MAC_BASE, offset) & bit) != 0U) {
        if (elapsed++ >= timeout_us) {
            printf("RESULT: FAIL (%s did not clear)\r\n", what);
            return false;
        }
        eqos_udelay(1U);
    }
    return true;
}

/* Timestamp unit, EQOS databook 18.12.1: fine update, 10 ns per tick, start at 0 s 0 ns,
 * then timestamp every RX frame. Returns false on any setup error. */
static bool txfc_ts_init(void)
{
    const uint32_t hclk = HAL_RCC_GetHCLKFreq();

    if (hclk <= 1000000000U / TXFC_TS_SSINC_NS) {
        printf("RESULT: FAIL (HCLK <= 100 MHz, fine update not possible)\r\n");
        return false;
    }
    const uint32_t addend = (uint32_t)(((uint64_t)(1000000000U / TXFC_TS_SSINC_NS) << 32) / hclk);
    printf("Timestamp: HCLK %lu Hz, addend 0x%08lX\r\n", (unsigned long)hclk, (unsigned long)addend);

    return ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_TS_CONTROL, 0U,
                          EQOS_MAC_TSC_TSENA | EQOS_MAC_TSC_TSCTRLSSR) &&
           ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_SUB_SEC_INCR, 0xFFFFFFFFU,
                          TXFC_TS_SSINC_NS << EQOS_MAC_SSIR_SSINC_SHIFT) &&
           ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_TS_ADDEND, 0xFFFFFFFFU, addend) &&
           txfc_set_and_poll(EQOS_MAC_TS_CONTROL, EQOS_MAC_TSC_TSADDREG, TXFC_TS_POLL_US, "TSADDREG") &&
           ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_TS_CONTROL, 0U, EQOS_MAC_TSC_TSCFUPDT) &&
           ETH_REG_UPDATE(EQOS_MAC_BASE, TXFC_MAC_SYS_TIME_SEC_UPDATE, 0xFFFFFFFFU, 0U) &&
           ETH_REG_UPDATE(EQOS_MAC_BASE, TXFC_MAC_SYS_TIME_NSEC_UPDATE, 0xFFFFFFFFU, 0U) &&
           txfc_set_and_poll(EQOS_MAC_TS_CONTROL, EQOS_MAC_TSC_TSINIT, TXFC_TS_POLL_US, "TSINIT") &&
           ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_TS_CONTROL, 0U, EQOS_MAC_TSC_TSENALL);
}

/* One pause measurement with PT = pt on DMA CH0: pause frame (T0 = its RX timestamp),
 * data frame queued during the pause (T1 = its TX timestamp), then both frames checked.
 * *delta_ns = T1 - T0. Returns 1: frames and timestamps OK, 0: a check failed,
 * -1: stop the test (data frame TX timeout, DMA may still own a descriptor). */
static int txfc_measure(const uint8_t *mac0, uint32_t pt, int64_t *delta_ns)
{
    static const uint8_t pause_da[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x01};
    static const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    const struct eth_rx_frame *rxf;
    const struct eth_tx_frame *txf;
    struct eth_tx_req req = {0};
    int len;
    bool ok = true;

    /* 1. MAC_Q0_Tx_Flow_Ctrl: PT, TFE; FCB_BPA=1 -> the MAC sends the pause frame */
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_Q0_TX_FLOW_CTRL,
                        (EQOS_MAC_TXFC_PT_MASK << EQOS_MAC_TXFC_PT_SHIFT) |
                        EQOS_MAC_TXFC_TFE | EQOS_MAC_TXFC_FCB_BPA,
                        (pt << EQOS_MAC_TXFC_PT_SHIFT) | EQOS_MAC_TXFC_TFE) ||
        !txfc_set_and_poll(EQOS_MAC_Q0_TX_FLOW_CTRL, EQOS_MAC_TXFC_FCB_BPA,
                           TXFC_FCB_TIMEOUT_US, "FCB_BPA"))
        return 0;

    /* 2. Receive the pause frame: T0 = its RX timestamp */
    len = eth_ingress_packet(0, txfc_rx_frame, sizeof(txfc_rx_frame));
    rxf = eth_pkt_last_rx(0);
    if (len < 0 || !rxf->ts_valid) {
        printf("FAIL: pause frame or its RX timestamp not received (ret %d)\r\n", len);
        return 0;
    }
    /* Check now, print after T1: no printf before the data frame is queued. */
    const uint64_t t0 = (uint64_t)rxf->ts_s * 1000000000ULL + rxf->ts_ns;
    const uint32_t t0_s = rxf->ts_s, t0_ns = rxf->ts_ns, pause_pl = rxf->len;
    uint8_t hdr[18];
    memcpy(hdr, txfc_rx_frame, sizeof(hdr));
    const uint32_t rx_pt = ((uint32_t)hdr[16] << 8) | hdr[17];
    const bool fields_ok = memcmp(hdr, pause_da, 6U) == 0 && memcmp(hdr + 6U, mac0, 6U) == 0 &&
                           hdr[12] == 0x88U && hdr[13] == 0x08U &&
                           hdr[14] == 0x00U && hdr[15] == 0x01U && rx_pt == pt;

    /* 3. Data frame queued during the pause: T1 = its TX timestamp */
    eth_construct_frame(txfc_tx_buf, sizeof(txfc_tx_buf), TXFC_FRAME_LEN, mac0, mac0,
                        0x88B5U, pattern, sizeof(pattern));
    req.buf1 = txfc_tx_buf;
    req.len1 = TXFC_FRAME_LEN;
    req.fd_tdes2 = EQOS_TDES2_TTSE;
    len = eth_egress_packet_ex(0, &req);
    txf = eth_pkt_last_tx(0);
    if (len != (int)TXFC_FRAME_LEN) {
        printf("FAIL: data frame TX (ret %d), DMA_CH0_Status=0x%08lX\r\n", len,
               (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
        return -1;
    }
    const uint64_t t1 = (uint64_t)txf->ts_s * 1000000000ULL + txf->ts_ns;
    *delta_ns = (int64_t)(t1 - t0);

    /* 4. T1 taken: print the pause frame check and the timestamps */
    printf("Pause frame RX: DA %02X:%02X:%02X:%02X:%02X:%02X type %02X%02X "
           "opcode %02X%02X PT=0x%04lX PL=%lu: %s\r\n",
           hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[12], hdr[13],
           hdr[14], hdr[15], (unsigned long)rx_pt, (unsigned long)pause_pl,
           fields_ok ? "OK" : "FAIL");
    ok = fields_ok;
    printf("  T0 = %lu s %lu ns\r\n", (unsigned long)t0_s, (unsigned long)t0_ns);
    if (txf->ts_valid) {
        printf("  T1 = %lu s %lu ns\r\n", (unsigned long)txf->ts_s, (unsigned long)txf->ts_ns);
    } else {
        printf("FAIL: no TX timestamp (TDES3.TTSS=0), TDES3=0x%08lX\r\n", (unsigned long)txf->tdes3);
        ok = false;
    }

    /* 5. Receive the data frame, check it is intact */
    len = eth_ingress_packet(0, txfc_rx_frame, sizeof(txfc_rx_frame));
    if (len != (int)TXFC_FRAME_LEN || memcmp(txfc_rx_frame, txfc_tx_buf, TXFC_FRAME_LEN) != 0) {
        printf("FAIL: data frame not received intact (ret %d)\r\n", len);
        ok = false;
    }
    return ok ? 1 : 0;
}

int eth_test_mac_txfc_pause_timestamp(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                                      struct eqos_desc *rx_desc,
                                      uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    const unsigned int case_count = 2U;
    unsigned int passes = 0U;
    int64_t delta_ns = 0;
    int64_t clk_err_ppm = 0;
    bool pass;
    int ret;

    printf("\r\n ========== Begin MAC_Q0_Tx_Flow_Ctrl pause test (timestamp) ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    printf("100 Mbps: 1 pause quantum = 5.12 us\r\n");

    /* ---------- 1. Setup: timestamp unit, RFE, PCF=10 (forward pause frame to software) ---------- */
    const uint32_t txfc_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_Q0_TX_FLOW_CTRL);
    const uint32_t rxfc_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_FLOW_CTRL);
    const uint32_t pfr_saved  = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
    const uint32_t tsc_saved  = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_TS_CONTROL);
    const uint32_t ssir_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_SUB_SEC_INCR);
    if (!txfc_ts_init() ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_RX_FLOW_CTRL, 0U, EQOS_MAC_RXFC_RFE) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER,
                        EQOS_MAC_PKT_FILTER_PCF_MASK, EQOS_MAC_PKT_FILTER_PCF_ALL)) {
        printf("RESULT: FAIL (setup)\r\n");
        return -1;
    }
    eth_pkt_set_timeout(TXFC_TX_TIMEOUT_US, TXFC_RX_TIMEOUT_US);   /* TX waits out the 335 ms pause */

    /* ---------- 2. Case 1: PT=0xFFFF, timestamp clock error vs the pause timer ---------- */
    const int64_t exp1 = TXFC_EXPECTED_NS(0xFFFFU);
    printf("\r\n--- Case 1/2: PT=0xFFFF, expected T1-T0 %ld ns ---\r\n", (long)exp1);
    ret = txfc_measure(eth->cfg.mac_addr, 0xFFFFU, &delta_ns);
    if (ret >= 0) {
        clk_err_ppm = (delta_ns - exp1) * 1000000LL / exp1;
        printf("T1 - T0 = %ld ns, timestamp clock error %ld ppm (limit +-%d)\r\n",
               (long)delta_ns, (long)clk_err_ppm, TXFC_CLK_TOL_PPM);
    }
    const bool clk_ok = ret == 1 && clk_err_ppm >= -TXFC_CLK_TOL_PPM &&
                        clk_err_ppm <= TXFC_CLK_TOL_PPM;
    printf("RESULT: %s\r\n", clk_ok ? "PASS" : "FAIL");
    passes += clk_ok;

    /* ---------- 3. Case 2: PT=0x0100, expected scaled by the case 1 error, +-2 us ---------- */
    if (ret >= 0) {
        const int64_t exp2 = TXFC_EXPECTED_NS(0x0100U);
        const int64_t exp_cal = exp2 + exp2 * clk_err_ppm / 1000000LL;
        printf("\r\n--- Case 2/2: PT=0x0100, expected T1-T0 %ld ns x (1 %+ld ppm) = %ld ns ---\r\n",
               (long)exp2, (long)clk_err_ppm, (long)exp_cal);
        ret = txfc_measure(eth->cfg.mac_addr, 0x0100U, &delta_ns);
        if (ret >= 0)
            printf("T1 - T0 = %ld ns, diff %ld ns (window +-%d)\r\n",
                   (long)delta_ns, (long)(delta_ns - exp_cal), TXFC_WIN_NS);
        if (!clk_ok)
            printf("FAIL: no valid clock error from case 1\r\n");
        pass = ret == 1 && clk_ok && delta_ns - exp_cal >= -TXFC_WIN_NS &&
               delta_ns - exp_cal <= TXFC_WIN_NS;
        printf("RESULT: %s\r\n", pass ? "PASS" : "FAIL");
        passes += pass;
    } else {
        printf("Case 2 not run: reset before rerunning.\r\n");
    }

    /* ---------- 4. Restore ---------- */
    eth_pkt_set_timeout(ETH_PKT_DEFAULT_TIMEOUT_US, ETH_PKT_DEFAULT_TIMEOUT_US);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_Q0_TX_FLOW_CTRL, txfc_saved & ~EQOS_MAC_TXFC_FCB_BPA);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_FLOW_CTRL, rxfc_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_SUB_SEC_INCR, ssir_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_TS_CONTROL, tsc_saved);

    printf("\r\n========== MAC_Q0_Tx_Flow_Ctrl pause test (timestamp): %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, case_count - passes);
    return (int)(case_count - passes);
}
