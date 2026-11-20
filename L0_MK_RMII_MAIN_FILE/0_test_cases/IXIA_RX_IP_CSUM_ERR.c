#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* Ixia IxTest8k_IpCsumErr: RX IPv4 header, UDP and TCP checksum errors with
 * the Receive Checksum Offload engine on, error packets kept (test ID 44).
 *
 * main.c includes this file AFTER the TX/RX rings are declared, brings the
 * link up with PHY auto-negotiation (MACCR.LM=0) and runs it once after USER;
 * do not add it as a separate build source.
 *
 * Covers SGMII_MTL_RX_DISABLE_DROP_TCP_CHECKSUM_ERR, SGMII_MAC_RX_IPV4_HEADER_ERROR,
 * SGMII_MAC_RX_UDP_ERROR and SGMII_MAC_RX_TCP_ERROR.
 * The Ixia sends 1000 frames of each, in this order, to the DUT MAC (static
 * MAC on the Ixia): good IPv4/UDP, bad IPv4 header checksum, bad UDP checksum,
 * bad TCP checksum. MACCR.IPC=1 turns the RX COE on. MTL RXQ0 RSF=1 and
 * DIS_TCP_EF=1 (main.c sets both; read back here): packets whose only error is
 * a checksum error are not dropped and reach the descriptor.
 * The H755 MMC has no IPC counters (full EQOS: RxIPv4_Header_Error_Packets
 * 0x814, RxUDP_Error_Packets 0x834, RxTCP_Error_Packets 0x83C), so the STM32
 * counts the LD write-back status (databook Table 17-26), valid when RS1V=1:
 *   RDES1.IPV4=1 and IPHE=1                -> 1000 (header error, PT=000)
 *   RDES1.IPCE=1 and PT=001 (UDP)          -> 1000
 *   RDES1.IPCE=1 and PT=010 (TCP)          -> 1000
 *   MMCRUPGR (unicast good) = 4000 (a checksum error is not a MAC error)
 * The Ixia rate must be low enough that no frame is missed by the DMA.
 * Run: wait up to CSUM_START_MS for the first frame, then receive for
 * CSUM_WINDOW_MS, then read the counters. */

#define CSUM_FRAMES_PER_STREAM  1000U
#define CSUM_STREAMS            4U       /* good, IPv4 hdr, UDP, TCP */
#define CSUM_START_MS           60000U   /* time to start the Ixia script */
#define CSUM_WINDOW_MS          10000U   /* receive window after the first frame */
#define CSUM_RDES3_RS1V         (1u << 26)   /* RDES1 valid */
#define CSUM_RDES1_PT_MASK      7u           /* payload type */
#define CSUM_RDES1_PT_UDP       1u
#define CSUM_RDES1_PT_TCP       2u
#define CSUM_RDES1_IPHE         (1u << 3)    /* IP header error */
#define CSUM_RDES1_IPV4         (1u << 4)    /* IPv4 header present */
#define CSUM_RDES1_IPCE         (1u << 7)    /* IP payload error */

static uint8_t csum_rx_frame[EQOS_BUF_SIZE];

int eth_test_ixia_rx_ip_csum_err(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                                 struct eqos_desc *rx_desc,
                                 uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    unsigned int failures = 0U;
    uint32_t delivered = 0U;
    uint32_t hdr_err = 0U;      /* IPV4=1, IPHE=1 */
    uint32_t udp_err = 0U;      /* IPCE=1, PT=UDP */
    uint32_t tcp_err = 0U;      /* IPCE=1, PT=TCP */

    printf("\r\n ========== Begin Ixia IxTest8k_IpCsumErr (receive) ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;

    /* ---------- 1. Setup: bind CH0 rings, no promiscuous mode, RX COE on, keep checksum-error packets ---------- */
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
                        0U, EQOS_MTL_RXQ_RSF | EQOS_MTL_RXQ_DISTCPEF)) {
        printf("FAIL: MACPFR / MACCR.IPC / MTL DIS_TCP_EF setup readback\r\n");
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
           (unsigned long)(CSUM_START_MS / 1000U));
    eth_pkt_set_timeout(ETH_PKT_DEFAULT_TIMEOUT_US, CSUM_START_MS * 1000U);
    int len = eth_ingress_packet(0, csum_rx_frame, sizeof(csum_rx_frame));
    if (len < 0) {
        printf("FAIL: no frame received\r\n");
        ++failures;
    } else {
        /* ---------- 4. Receive for CSUM_WINDOW_MS, count LD status (no printf) ---------- */
        eth_pkt_set_timeout(ETH_PKT_DEFAULT_TIMEOUT_US, 0U);    /* RX: check once */
        const uint32_t t0 = HAL_GetTick();
        for (;;) {
            if (len >= 0) {
                const uint32_t rdes1 = eth_pkt_last_rx(0)->rdes1;
                const uint32_t pt = rdes1 & CSUM_RDES1_PT_MASK;
                ++delivered;
                if ((eth_pkt_last_rx(0)->rdes3 & CSUM_RDES3_RS1V) != 0U) {
                    if ((rdes1 & (CSUM_RDES1_IPV4 | CSUM_RDES1_IPHE)) ==
                        (CSUM_RDES1_IPV4 | CSUM_RDES1_IPHE))
                        ++hdr_err;
                    else if ((rdes1 & CSUM_RDES1_IPCE) != 0U && pt == CSUM_RDES1_PT_UDP)
                        ++udp_err;
                    else if ((rdes1 & CSUM_RDES1_IPCE) != 0U && pt == CSUM_RDES1_PT_TCP)
                        ++tcp_err;
                }
            }
            if (HAL_GetTick() - t0 >= CSUM_WINDOW_MS)
                break;
            len = eth_ingress_packet(0, csum_rx_frame, sizeof(csum_rx_frame));
        }

        /* ---------- 5. Check the MMC counters and the LD status counts ---------- */
        const uint32_t uc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_UNICAST_GOOD) - uc0;
        const uint32_t crc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_CRC_ERROR_PACKETS) - crc0;
        const uint32_t missed = AX_BASE_READ_REG32(EQOS_DMA_BASE,
                                                   EQOS_DMA_CH0_MISSED_FRAME_COUNT);
        printf("MMC RX unicast good:      %lu (expect %lu)\r\n",
               (unsigned long)uc, (unsigned long)(CSUM_FRAMES_PER_STREAM * CSUM_STREAMS));
        printf("MMC RX CRC errors:        %lu (info)\r\n", (unsigned long)crc);
        printf("Info: delivered to software %lu, DMA missed %lu%s\r\n",
               (unsigned long)delivered, (unsigned long)(missed & 0x7FFU),
               (missed & (1U << 15)) ? " (overflow)" : "");
        printf("IPv4 header error (IPHE): %lu (expect %lu)\r\n",
               (unsigned long)hdr_err, (unsigned long)CSUM_FRAMES_PER_STREAM);
        printf("UDP error (IPCE, PT=UDP): %lu (expect %lu)\r\n",
               (unsigned long)udp_err, (unsigned long)CSUM_FRAMES_PER_STREAM);
        printf("TCP error (IPCE, PT=TCP): %lu (expect %lu)\r\n",
               (unsigned long)tcp_err, (unsigned long)CSUM_FRAMES_PER_STREAM);
        if (uc != CSUM_FRAMES_PER_STREAM * CSUM_STREAMS) {
            printf("FAIL: unicast good count\r\n");
            ++failures;
        }
        if (hdr_err != CSUM_FRAMES_PER_STREAM) {
            printf("FAIL: IPv4 header error count\r\n");
            ++failures;
        }
        if (udp_err != CSUM_FRAMES_PER_STREAM) {
            printf("FAIL: UDP error count\r\n");
            ++failures;
        }
        if (tcp_err != CSUM_FRAMES_PER_STREAM) {
            printf("FAIL: TCP error count\r\n");
            ++failures;
        }
    }
    printf("RESULT: %s\r\n", failures == 0U ? "PASS" : "FAIL");

    /* ---------- 6. Restore MTL RXQ0, MACCR and MACPFR ---------- */
    AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, mtl_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, maccr_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
    printf("\r\n========== IxTest8k_IpCsumErr: %s ==========\r\n",
           failures == 0U ? "PASS" : "FAIL");
    return (int)failures;
}
