#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* Ixia IxTest8k_IpCsumErr, drop case: same Ixia script as test 44, with
 * MTL_RxQ0_Operation_Mode.DIS_TCP_EF=0 (test ID 45).
 *
 * main.c includes this file AFTER the TX/RX rings are declared, brings the
 * link up with PHY auto-negotiation (MACCR.LM=0) and runs it once after USER;
 * do not add it as a separate build source.
 *
 * Reverse check of SGMII_MTL_RX_DISABLE_DROP_TCP_CHECKSUM_ERR.
 * The Ixia sends 1000 frames of each, in this order, to the DUT MAC (static
 * MAC on the Ixia): good IPv4/UDP, bad IPv4 header checksum, bad UDP checksum,
 * bad TCP checksum. MACCR.IPC=1, RSF=1, DIS_TCP_EF=0 and FEP=0: the MTL drops
 * every packet with a TCP/IP header or payload error (databook 11.2.2), so
 * only the good UDP frames reach software. The MAC still receives all of them:
 *   MMCRUPGR (unicast good) = 4000
 *   delivered with RDES1.IPHE or IPCE (RS1V=1)          -> 0
 *   delivered good UDP (IPV4=1, PT=001, no IPHE/IPCE)   -> 1000
 * The Ixia rate must be low enough that no frame is missed by the DMA.
 * Run: wait up to CSDROP_START_MS for the first frame, then receive for
 * CSDROP_WINDOW_MS, then read the counters. */

#define CSDROP_FRAMES_PER_STREAM  1000U
#define CSDROP_STREAMS            4U       /* good, IPv4 hdr, UDP, TCP */
#define CSDROP_START_MS           60000U   /* time to start the Ixia script */
#define CSDROP_WINDOW_MS          10000U   /* receive window after the first frame */
#define CSDROP_RDES3_RS1V         (1u << 26)   /* RDES1 valid */
#define CSDROP_RDES1_PT_MASK      7u           /* payload type */
#define CSDROP_RDES1_PT_UDP       1u
#define CSDROP_RDES1_IPHE         (1u << 3)    /* IP header error */
#define CSDROP_RDES1_IPV4         (1u << 4)    /* IPv4 header present */
#define CSDROP_RDES1_IPCE         (1u << 7)    /* IP payload error */

static uint8_t csdrop_rx_frame[EQOS_BUF_SIZE];

int eth_test_ixia_rx_ip_csum_drop(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                                  struct eqos_desc *rx_desc,
                                  uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    unsigned int failures = 0U;
    uint32_t delivered = 0U;
    uint32_t err_delivered = 0U;  /* IPHE=1 or IPCE=1 */
    uint32_t udp_good = 0U;       /* IPV4=1, PT=UDP, no IPHE/IPCE */

    printf("\r\n ========== Begin Ixia IxTest8k_IpCsumErr (drop, receive) ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;

    /* ---------- 1. Setup: bind CH0 rings, no promiscuous mode, RX COE on, drop checksum-error packets ---------- */
    if (eth_pkt_ring_init(0, tx_desc, EQOS_TX_DESC_COUNT,
                          rx_desc, EQOS_RX_DESC_COUNT, rx_buf) != EQOS_OK) {
        printf("FAIL: eth_pkt_ring_init\r\n");
        return -1;
    }
    const uint32_t pfr_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
    const uint32_t maccr_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);
    const uint32_t mtl_saved = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE);
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, EQOS_MAC_PKT_FILTER_PR, 0U) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, 0U, EQOS_MAC_CFG_IPC) ||
        !ETH_REG_UPDATE(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE,
                        EQOS_MTL_RXQ_DISTCPEF | EQOS_MTL_RXQ_FEP, EQOS_MTL_RXQ_RSF)) {
        printf("FAIL: MACPFR / MACCR.IPC / MTL RSF/DIS_TCP_EF/FEP setup readback\r\n");
        AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, mtl_saved);
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, maccr_saved);
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
        return -1;
    }

    /* ---------- 2. Counter start values (MMC: snapshot; DMA missed: read clears) ---------- */
    const uint32_t uc0 = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_UNICAST_GOOD);
    const uint32_t crc0 = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_CRC_ERROR_PACKETS);
    (void)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_MISSED_FRAME_COUNT);

    /* ---------- 3. Wait for the first frame ---------- */
    printf("Start IxTest8k_IpCsumErr on the Ixia now (waiting %lu s).\r\n",
           (unsigned long)(CSDROP_START_MS / 1000U));
    eth_pkt_set_timeout(ETH_PKT_DEFAULT_TIMEOUT_US, CSDROP_START_MS * 1000U);
    int len = eth_ingress_packet(0, csdrop_rx_frame, sizeof(csdrop_rx_frame));
    if (len < 0) {
        printf("FAIL: no frame received\r\n");
        ++failures;
    } else {
        /* ---------- 4. Receive for CSDROP_WINDOW_MS, count LD status (no printf) ---------- */
        eth_pkt_set_timeout(ETH_PKT_DEFAULT_TIMEOUT_US, 0U);    /* RX: check once */
        const uint32_t t0 = HAL_GetTick();
        for (;;) {
            if (len >= 0) {
                const uint32_t rdes1 = eth_pkt_last_rx(0)->rdes1;
                const uint32_t pt = rdes1 & CSDROP_RDES1_PT_MASK;
                ++delivered;
                if ((eth_pkt_last_rx(0)->rdes3 & CSDROP_RDES3_RS1V) != 0U) {
                    if ((rdes1 & (CSDROP_RDES1_IPHE | CSDROP_RDES1_IPCE)) != 0U)
                        ++err_delivered;
                    else if ((rdes1 & CSDROP_RDES1_IPV4) != 0U && pt == CSDROP_RDES1_PT_UDP)
                        ++udp_good;
                }
            }
            if (HAL_GetTick() - t0 >= CSDROP_WINDOW_MS)
                break;
            len = eth_ingress_packet(0, csdrop_rx_frame, sizeof(csdrop_rx_frame));
        }

        /* ---------- 5. Check the MMC counters and the LD status counts ---------- */
        const uint32_t uc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_UNICAST_GOOD) - uc0;
        const uint32_t crc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_CRC_ERROR_PACKETS) - crc0;
        const uint32_t missed = AX_BASE_READ_REG32(EQOS_DMA_BASE,
                                                   EQOS_DMA_CH0_MISSED_FRAME_COUNT);
        printf("MMC RX unicast good:      %lu (expect %lu)\r\n",
               (unsigned long)uc, (unsigned long)(CSDROP_FRAMES_PER_STREAM * CSDROP_STREAMS));
        printf("MMC RX CRC errors:        %lu (info)\r\n", (unsigned long)crc);
        printf("Info: delivered to software %lu, DMA missed %lu%s\r\n",
               (unsigned long)delivered, (unsigned long)(missed & 0x7FFU),
               (missed & (1U << 15)) ? " (overflow)" : "");
        printf("Delivered with IPHE/IPCE: %lu (expect 0)\r\n", (unsigned long)err_delivered);
        printf("Delivered good UDP:       %lu (expect %lu)\r\n",
               (unsigned long)udp_good, (unsigned long)CSDROP_FRAMES_PER_STREAM);
        if (uc != CSDROP_FRAMES_PER_STREAM * CSDROP_STREAMS) {
            printf("FAIL: unicast good count\r\n");
            ++failures;
        }
        if (err_delivered != 0U) {
            printf("FAIL: checksum-error packet not dropped\r\n");
            ++failures;
        }
        if (udp_good != CSDROP_FRAMES_PER_STREAM) {
            printf("FAIL: good UDP count\r\n");
            ++failures;
        }
    }
    printf("RESULT: %s\r\n", failures == 0U ? "PASS" : "FAIL");

    /* ---------- 6. Restore MTL RXQ0, MACCR and MACPFR ---------- */
    AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, mtl_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, maccr_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
    printf("\r\n========== IxTest8k_IpCsumErr (drop): %s ==========\r\n",
           failures == 0U ? "PASS" : "FAIL");
    return (int)failures;
}
