#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* Ixia IxTest8m_IpFrag: RX IPv4 fragments, Receive Checksum Offload engine on
 * (test ID 47).
 *
 * main.c includes this file AFTER the TX/RX rings are declared, brings the
 * link up with PHY auto-negotiation (MACCR.LM=0) and runs it once after USER;
 * do not add it as a separate build source.
 *
 * The Ixia sends 1000 frames of each, in this order, to the DUT MAC (static
 * MAC on the Ixia): good IPv4/UDP; first fragment (MF=1, offset 0); last
 * fragment (MF=0, offset > 0). All with a valid IPv4 header checksum.
 * MACCR.IPC=1, RSF=1, DIS_TCP_EF=1. For a fragment the COE checks the IP
 * header but not the payload (databook Table 11-2) and sets PT=000.
 * LD status with RS1V=1:
 *   good UDP:  IPV4=1, PT=001, IPHE=0, IPCE=0     = 1000
 *   fragment:  IPV4=1, PT=000, IPHE=0, IPCE=0     = 2000
 *   MMCRUPGR (unicast good) = 3000
 * RDES1.IPCB (COE bypassed) is counted for information.
 * The Ixia rate must be low enough that no frame is missed by the DMA.
 * Run: wait up to IPFRAG_START_MS for the first frame, then receive for
 * IPFRAG_WINDOW_MS, then read the counters. */

#define IPFRAG_FRAMES_PER_STREAM  1000U
#define IPFRAG_STREAMS            3U       /* good, first fragment, last fragment */
#define IPFRAG_START_MS           60000U   /* time to start the Ixia script */
#define IPFRAG_WINDOW_MS          10000U   /* receive window after the first frame */
#define IPFRAG_RDES3_RS1V         (1u << 26)   /* RDES1 valid */
#define IPFRAG_RDES1_PT_MASK      7u           /* payload type */
#define IPFRAG_RDES1_PT_UDP       1u
#define IPFRAG_RDES1_IPHE         (1u << 3)    /* IP header error */
#define IPFRAG_RDES1_IPV4         (1u << 4)    /* IPv4 header present */
#define IPFRAG_RDES1_IPCB         (1u << 6)    /* checksum offload bypassed */
#define IPFRAG_RDES1_IPCE         (1u << 7)    /* IP payload error */

static uint8_t ipfrag_rx_frame[EQOS_BUF_SIZE];

int eth_test_ixia_rx_ip_frag(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                             struct eqos_desc *rx_desc,
                             uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    unsigned int failures = 0U;
    uint32_t delivered = 0U;
    uint32_t udp_good = 0U;     /* IPV4=1, PT=UDP, no IPHE/IPCE */
    uint32_t frag_ok = 0U;      /* IPV4=1, PT=000, no IPHE/IPCE */
    uint32_t bypassed = 0U;     /* IPCB=1 (info) */

    printf("\r\n ========== Begin Ixia IxTest8m_IpFrag (receive) ==========\r\n");
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
    printf("Start IxTest8m_IpFrag on the Ixia now (waiting %lu s).\r\n",
           (unsigned long)(IPFRAG_START_MS / 1000U));
    eth_pkt_set_timeout(ETH_PKT_DEFAULT_TIMEOUT_US, IPFRAG_START_MS * 1000U);
    int len = eth_ingress_packet(0, ipfrag_rx_frame, sizeof(ipfrag_rx_frame));
    if (len < 0) {
        printf("FAIL: no frame received\r\n");
        ++failures;
    } else {
        /* ---------- 4. Receive for IPFRAG_WINDOW_MS, count LD status (no printf) ---------- */
        eth_pkt_set_timeout(ETH_PKT_DEFAULT_TIMEOUT_US, 0U);    /* RX: check once */
        const uint32_t t0 = HAL_GetTick();
        for (;;) {
            if (len >= 0) {
                const uint32_t rdes1 = eth_pkt_last_rx(0)->rdes1;
                const uint32_t pt = rdes1 & IPFRAG_RDES1_PT_MASK;
                ++delivered;
                if ((eth_pkt_last_rx(0)->rdes3 & IPFRAG_RDES3_RS1V) != 0U) {
                    if ((rdes1 & IPFRAG_RDES1_IPCB) != 0U)
                        ++bypassed;
                    if ((rdes1 & IPFRAG_RDES1_IPV4) != 0U &&
                        (rdes1 & (IPFRAG_RDES1_IPHE | IPFRAG_RDES1_IPCE)) == 0U) {
                        if (pt == IPFRAG_RDES1_PT_UDP)
                            ++udp_good;
                        else if (pt == 0U)
                            ++frag_ok;
                    }
                }
            }
            if (HAL_GetTick() - t0 >= IPFRAG_WINDOW_MS)
                break;
            len = eth_ingress_packet(0, ipfrag_rx_frame, sizeof(ipfrag_rx_frame));
        }

        /* ---------- 5. Check the MMC counters and the LD status counts ---------- */
        const uint32_t uc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_UNICAST_GOOD) - uc0;
        const uint32_t crc = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_RX_CRC_ERROR_PACKETS) - crc0;
        const uint32_t missed = AX_BASE_READ_REG32(EQOS_DMA_BASE,
                                                   EQOS_DMA_CH0_MISSED_FRAME_COUNT);
        printf("MMC RX unicast good:      %lu (expect %lu)\r\n",
               (unsigned long)uc, (unsigned long)(IPFRAG_FRAMES_PER_STREAM * IPFRAG_STREAMS));
        printf("MMC RX CRC errors:        %lu (info)\r\n", (unsigned long)crc);
        printf("Info: delivered to software %lu, DMA missed %lu%s\r\n",
               (unsigned long)delivered, (unsigned long)(missed & 0x7FFU),
               (missed & (1U << 15)) ? " (overflow)" : "");
        printf("Good UDP (PT=UDP):        %lu (expect %lu)\r\n",
               (unsigned long)udp_good, (unsigned long)IPFRAG_FRAMES_PER_STREAM);
        printf("Fragment (PT=000, no err):%lu (expect %lu)\r\n",
               (unsigned long)frag_ok, (unsigned long)(2U * IPFRAG_FRAMES_PER_STREAM));
        printf("COE bypassed (IPCB):      %lu (info)\r\n", (unsigned long)bypassed);
        if (uc != IPFRAG_FRAMES_PER_STREAM * IPFRAG_STREAMS) {
            printf("FAIL: unicast good count\r\n");
            ++failures;
        }
        if (udp_good != IPFRAG_FRAMES_PER_STREAM) {
            printf("FAIL: good UDP count\r\n");
            ++failures;
        }
        if (frag_ok != 2U * IPFRAG_FRAMES_PER_STREAM) {
            printf("FAIL: fragment count\r\n");
            ++failures;
        }
    }
    printf("RESULT: %s\r\n", failures == 0U ? "PASS" : "FAIL");

    /* ---------- 6. Restore MTL RXQ0, MACCR and MACPFR ---------- */
    AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, mtl_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, maccr_saved);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, pfr_saved);
    printf("\r\n========== IxTest8m_IpFrag: %s ==========\r\n",
           failures == 0U ? "PASS" : "FAIL");
    return (int)failures;
}
