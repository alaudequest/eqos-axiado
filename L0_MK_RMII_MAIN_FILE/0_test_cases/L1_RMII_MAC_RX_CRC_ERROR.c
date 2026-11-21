#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>

/* MAC RX CRC error test (test ID 6), MAC loopback (LM=1), 100M full duplex.
 * Called by eth_rmii_mac_lb() after mac_start(); every run starts from a MAC reset.
 *   Setup: MAC_Packet_Filter = 0; MTL_RxQ0_Operation_Mode FEP=0 (error frames dropped), RSF=1
 *          MMC_Control RSTONRD=0, CNTFREEZ=0 (cumulative counter, delta is read)
 *   Frame: 64 bytes, DA = SA = MAC0, bytes 60..63 = FCS with one bit flipped,
 *          sent with TDES3.CPC = 10 (no CRC insertion): the bad FCS goes on the wire
 *   Expect no frame received, Rx_CRC_Error_Count + 1
 * Returns the failure count, -1 on setup error. */

static uint8_t crc_tx_frame[64] __attribute__((aligned(32)));
static uint8_t crc_rx_frame[EQOS_BUF_SIZE];

int eth_test_mac_rx_crc_error(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                              struct eqos_desc *rx_desc, uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    printf("\r\n ========== Begin MAC RX CRC Error test ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};

    /* ---------- 1. Setup: frame with a bad FCS, filter, MTL RX queue, MMC ---------- */
    uint32_t length = eth_construct_frame(crc_tx_frame, sizeof(crc_tx_frame), 64U,
        eth->cfg.mac_addr, eth->cfg.mac_addr, 0x88B5U, pattern, sizeof(pattern));
    if (length == 0U)
        return -1;

    /* Compute the Ethernet FCS over 60 bytes, then flip one bit so failure
     * is guaranteed. Bytes 60..63 contain the deliberately corrupted FCS. */
    uint32_t crc = 0xFFFFFFFFU;
    for (unsigned int i = 0; i < 60U; ++i) {
        crc ^= crc_tx_frame[i];
        for (unsigned int bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
    crc = (~crc) ^ 1U;
    for (unsigned int i = 0; i < 4U; ++i)
        crc_tx_frame[60U + i] = (uint8_t)(crc >> (8U * i));

    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, 0U);
    /* FEP=0: bad frames are counted but not delivered. */
    uint32_t rx_mode = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE);
    rx_mode &= ~ETH_MTLRQOMR_FEP;
    rx_mode |= EQOS_MTL_RXQ_RSF;
    AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, rx_mode);
    ETH->MMCCR &= ~(ETH_MMCCR_RSTONRD | ETH_MMCCR_CNTFREEZ);
    const uint32_t crc_count_before = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_CRC_ERROR);
    printf("64 bytes including bad FCS; FEP=0: expect no RX descriptor, CRC count +1\r\n");

    /* ---------- 2. Send one frame without CRC insertion, receive ---------- */
    struct eth_tx_req req = {0};
    req.buf1 = crc_tx_frame;
    req.len1 = length;
    req.fd_tdes3 = EQOS_TDES3_CPC;      /* disable CRC insertion */
    const int tx = eth_egress_packet_ex(0, &req);
    const uint32_t tdes3 = eth_pkt_last_tx(0)->tdes3;
    const int rx = eth_ingress_packet(0, crc_rx_frame, sizeof(crc_rx_frame));
    const uint32_t crc_count = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_CRC_ERROR);
    printf("TX ret=%d TDES3=0x%08lX, RX ret=%d RDES3=0x%08lX\r\n", tx, (unsigned long)tdes3,
           rx, (unsigned long)eth_pkt_last_rx(0)->rdes3);
    printf("Rx_CRC_Error_Count [0x794]=%lu, delta=%lu\r\n",
           (unsigned long)crc_count, (unsigned long)(crc_count - crc_count_before));

    /* ---------- 3. Check ---------- */
    const bool pass = (tx == (int)length) && ((tdes3 & EQOS_TDES3_WB_ES) == 0U) &&
                      (rx == ETH_PKT_ETIMEOUT) && (crc_count - crc_count_before == 1U);

    printf("\r\n========== MAC RX CRC Error test: %s ==========\r\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
