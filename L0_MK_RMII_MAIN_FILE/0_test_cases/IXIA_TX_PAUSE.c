#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* Ixia IxTest8g_Pause: STM32 TX rate under received PAUSE frames (test ID 40).
 *
 * main.c includes this file AFTER the TX/RX rings are declared, brings the
 * link up with PHY auto-negotiation (MACCR.LM=0) and runs it once after USER;
 * do not add it as a separate build source.
 *
 * The STM32 sends Test 2 frames (UDP, "IXTS" magic, sequence number,
 * PAUSE_FRAME_SIZE bytes incl. CRC) to the Ixia for PAUSE_RUN_MS, as fast as
 * possible. During the run the Ixia sends PAUSE frames (65535 quanta = 335 ms)
 * every 50 ms for 5 s and measures the STM32 fps before, during and after.
 * Pass (measured by the Ixia): during < 1 % of before, after > 90 % of before.
 *
 * STM32 side: MAC_Rx_Flow_Ctrl.RFE=1 (obey received PAUSE), full duplex link
 * required. While paused the TX slots stay with DMA, so eth_egress_packet()
 * returns ETH_PKT_ETIMEOUT after 5 ms: the same frame is retried and the
 * retry is counted as a stall. The run stops early if TX makes no progress
 * for PAUSE_STALL_LIMIT_MS.
 * TX is pipelined (TX timeout 0). Each frame uses one descriptor, so buffer
 * i % PAUSE_NBUF (PAUSE_NBUF = 2 x EQOS_TX_DESC_COUNT) was last used by frame
 * i - PAUSE_NBUF, which DMA has released: the buffer is free to rewrite. */

#define PAUSE_FRAME_SIZE      1518U        /* incl. CRC; the MAC adds the CRC */
#define PAUSE_FRAME_LEN       (PAUSE_FRAME_SIZE - 4U)
#define PAUSE_RUN_MS          300000U     /* before + 5 s PAUSE + after */
#define PAUSE_STALL_LIMIT_MS  10000U     /* no TX progress: stop the run */
#define PAUSE_FLUSH_US        1000000U   /* > one 335 ms PAUSE */
#define PAUSE_NBUF            (2U * EQOS_TX_DESC_COUNT)

static const uint8_t pause_peer_mac[6] = {0x00, 0x00, 0xAA, 0xBB, 0xCC, 0x02};
static const uint8_t pause_dut_ip[4]   = {10, 7, 10, 32};
static const uint8_t pause_peer_ip[4]  = {10, 7, 10, 30};
static uint8_t pause_tx_buf[PAUSE_NBUF][PAUSE_FRAME_LEN] __attribute__((aligned(32)));

int eth_test_ixia_tx_pause(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                           struct eqos_desc *rx_desc,
                           uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    static const uint8_t fill[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    const uint32_t len = PAUSE_FRAME_LEN;
    unsigned int failures = 0U;
    uint32_t sent = 0U;
    uint32_t stalls = 0U;
    bool stopped = false;

    printf("\r\n ========== Begin Ixia IxTest8g_Pause (transmit) ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;

    /* ---------- 1. Setup: bind CH0 rings, pipelined TX, full duplex, RFE=1 ---------- */
    if (eth_pkt_ring_init(0, tx_desc, EQOS_TX_DESC_COUNT,
                          rx_desc, EQOS_RX_DESC_COUNT, rx_buf) != EQOS_OK) {
        printf("FAIL: eth_pkt_ring_init\r\n");
        return -1;
    }
    eth_pkt_set_timeout(0U, 0U);                              /* TX pipelined */
    if ((AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION) & EQOS_MAC_CFG_DM) == 0U) {
        printf("FAIL: half duplex link, PAUSE needs full duplex\r\n");
        return -1;
    }
    const uint32_t rxfc_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_FLOW_CTRL);
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_RX_FLOW_CTRL, 0U, EQOS_MAC_RXFC_RFE)) {
        printf("FAIL: MAC_Rx_Flow_Ctrl setup readback\r\n");
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_FLOW_CTRL, rxfc_saved);
        return -1;
    }

    /* ---------- 2. Build the frames (Test 2 format, sequence set per frame) ---------- */
    for (uint32_t b = 0U; b < PAUSE_NBUF; ++b) {
        uint8_t *f = pause_tx_buf[b];
        uint8_t *ip = f + 14;
        uint32_t sum = 0U;

        memcpy(f, pause_peer_mac, 6U);                        /* Ethernet */
        memcpy(f + 6, eth->cfg.mac_addr, 6U);
        f[12] = 0x08U;
        f[13] = 0x00U;
        memset(ip, 0, 20U);                                   /* IPv4, IHL = 5 */
        ip[0] = 0x45U;
        ip[2] = (uint8_t)((len - 14U) >> 8);
        ip[3] = (uint8_t)(len - 14U);
        ip[8] = 64U;                                          /* TTL */
        ip[9] = 17U;                                          /* UDP */
        memcpy(ip + 12, pause_dut_ip, 4U);
        memcpy(ip + 16, pause_peer_ip, 4U);
        for (uint32_t i = 0U; i < 20U; i += 2U)
            sum += ((uint32_t)ip[i] << 8) | ip[i + 1U];
        while ((sum >> 16) != 0U)
            sum = (sum & 0xFFFFU) + (sum >> 16);
        ip[10] = (uint8_t)(~sum >> 8);
        ip[11] = (uint8_t)~sum;
        f[34] = (uint8_t)(5000U >> 8);                        /* UDP 5000 -> 6000 */
        f[35] = (uint8_t)5000U;
        f[36] = (uint8_t)(6000U >> 8);
        f[37] = (uint8_t)6000U;
        f[38] = (uint8_t)((len - 34U) >> 8);                  /* UDP length */
        f[39] = (uint8_t)(len - 34U);
        f[40] = 0U;                                           /* checksum 0 */
        f[41] = 0U;
        memcpy(f + 42, "IXTS", 4U);                           /* magic */
        memset(f + 46, 0, 4U);                                /* sequence */
        for (uint32_t i = 50U; i < len; ++i)
            f[i] = fill[(i - 50U) & 3U];
    }

    /* ---------- 3. Send for PAUSE_RUN_MS (no printf) ---------- */
    printf("Sending %lu B frames for %lu s: start IxTest8g_Pause on the Ixia now.\r\n",
           (unsigned long)PAUSE_FRAME_SIZE, (unsigned long)(PAUSE_RUN_MS / 1000U));
    const uint32_t mmc0 = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_TX_PACKET_COUNT_GOOD);
    const uint32_t t0 = HAL_GetTick();
    uint32_t t_progress = t0;
    while (HAL_GetTick() - t0 < PAUSE_RUN_MS) {
        uint8_t *f = pause_tx_buf[sent % PAUSE_NBUF];
        f[46] = (uint8_t)(sent >> 24);
        f[47] = (uint8_t)(sent >> 16);
        f[48] = (uint8_t)(sent >> 8);
        f[49] = (uint8_t)sent;
        if (eth_egress_packet(0, f, (int)len) == (int)len) {
            ++sent;
            t_progress = HAL_GetTick();
        } else {
            ++stalls;                     /* paused: no free slot in 5 ms, retry */
            if (HAL_GetTick() - t_progress > PAUSE_STALL_LIMIT_MS) {
                stopped = true;
                break;
            }
        }
    }
    const int flush = eth_egress_flush(0, PAUSE_FLUSH_US);
    const uint32_t ms = HAL_GetTick() - t0;
    const uint32_t mmc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_TX_PACKET_COUNT_GOOD) - mmc0;

    /* ---------- 4. Report (the rate result is measured by the Ixia) ---------- */
    printf("Sent %lu frames in %lu ms, TX stalls %lu, MMC TX good %lu, flush %s\r\n",
           (unsigned long)sent, (unsigned long)ms, (unsigned long)stalls,
           (unsigned long)mmc, flush == EQOS_OK ? "OK" : "TIMEOUT");
    if (stopped) {
        printf("FAIL: no TX progress for %lu ms\r\n", (unsigned long)PAUSE_STALL_LIMIT_MS);
        ++failures;
    }
    if (flush != EQOS_OK) {
        printf("FAIL: TX not drained, DMA_CH0_Status=0x%08lX: reset before rerun\r\n",
               (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
        ++failures;
    }
    if (stalls == 0U)
        printf("CHECK: no TX stall seen, PAUSE frames not received or not obeyed\r\n");
    printf("RESULT: %s (STM32 side; pass/fail of the rate is on the Ixia)\r\n",
           failures == 0U ? "PASS" : "FAIL");

    /* ---------- 5. Restore MAC_Rx_Flow_Ctrl ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_FLOW_CTRL, rxfc_saved);
    printf("\r\n========== IxTest8g_Pause: %s ==========\r\n",
           failures == 0U ? "PASS" : "FAIL");
    return (int)failures;
}
