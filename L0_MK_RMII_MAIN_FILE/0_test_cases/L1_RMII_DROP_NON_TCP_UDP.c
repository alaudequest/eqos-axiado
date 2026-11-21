#include "eth_functions_utils.h"
#include "eth_config_test_case.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* Drop Non TCP/UDP test (test ID 5), MAC loopback (LM=1), 100M full duplex.
 * Called by eth_rmii_mac_lb() after mac_start(); every run starts from a MAC reset.
 *   Setup: MAC_Packet_Filter IPFE=1, DNTU=DNTU_TEST_ENABLE (normal DA filter, no PR/RA)
 *   Frame: 60 bytes, DA = SA = MAC0, IPv4 192.0.2.1 -> 192.0.2.2, protocol
 *          DNTU_TEST_PROTOCOL: 1 = ICMP echo request, 17 = UDP 1234 -> 5678
 *   Expect DNTU=1 and ICMP: no frame (dropped)
 *          otherwise:       frame received, same length
 * Returns the failure count, -1 on setup error. */

static uint8_t dntu_tx_frame[60] __attribute__((aligned(32)));
static uint8_t dntu_rx_frame[EQOS_BUF_SIZE];

int eth_test_drop_non_tcp_udp(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                              struct eqos_desc *rx_desc, uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    printf("\r\n ========== Begin Drop Non TCP/UDP test ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL ||
        (DNTU_TEST_PROTOCOL != 1U && DNTU_TEST_PROTOCOL != 17U))
        return -1;
    const uint8_t protocol = DNTU_TEST_PROTOCOL;
    const bool enable = (DNTU_TEST_ENABLE != 0U);
    const bool expect_rx = !(enable && protocol == 1U);

    /* ---------- 1. Setup: IPv4 frame, MAC_Packet_Filter ---------- */
    const uint8_t zero = 0U;
    uint32_t length = eth_construct_frame(dntu_tx_frame, sizeof(dntu_tx_frame), 60U,
        eth->cfg.mac_addr, eth->cfg.mac_addr, 0x0800U, &zero, 1U);
    if (length == 0U)
        return -1;

    /* IPv4: 20-byte header + 8-byte ICMP/UDP, Ethernet padding follows.
     * No fragmentation. Source 192.0.2.1, destination 192.0.2.2. */
    uint8_t ip[20] = {0x45, 0, 0, 28, 0, 1, 0, 0, 64, protocol,
                      0, 0, 192, 0, 2, 1, 192, 0, 2, 2};
    uint32_t sum = 0U;
    for (unsigned int i = 0; i < sizeof(ip); i += 2U)
        sum += ((uint16_t)ip[i] << 8) | ip[i + 1U];
    while (sum >> 16)
        sum = (sum & 0xFFFFU) + (sum >> 16);
    uint16_t checksum = (uint16_t)~sum;
    ip[10] = (uint8_t)(checksum >> 8);
    ip[11] = (uint8_t)checksum;
    memcpy(dntu_tx_frame + 14U, ip, sizeof(ip));

    if (protocol == 1U) {
        /* ICMP echo request, identifier=1, sequence=1, valid checksum. */
        const uint8_t icmp[8] = {8, 0, 0xF7, 0xFD, 0, 1, 0, 1};
        memcpy(dntu_tx_frame + 34U, icmp, sizeof(icmp));
    } else {
        /* UDP ports 1234/5678, length 8. Zero checksum is valid for IPv4. */
        const uint8_t udp[8] = {4, 0xD2, 0x16, 0x2E, 0, 8, 0, 0};
        memcpy(dntu_tx_frame + 34U, udp, sizeof(udp));
    }

    uint32_t filter = EQOS_MAC_PKT_FILTER_IPFE;
    if (enable)
        filter |= EQOS_MAC_PKT_FILTER_DNTU;
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, filter);
    printf("DNTU=%u protocol=%u: expect %s\r\n", (unsigned int)enable,
           (unsigned int)protocol, expect_rx ? "RX packet" : "no RX descriptor");

    /* ---------- 2. Send one frame, receive ---------- */
    const int tx = eth_egress_packet(0, dntu_tx_frame, (int)length);
    const uint32_t tdes3 = eth_pkt_last_tx(0)->tdes3;
    const int rx = eth_ingress_packet(0, dntu_rx_frame, sizeof(dntu_rx_frame));
    printf("TX ret=%d TDES3=0x%08lX, RX ret=%d RDES3=0x%08lX\r\n", tx, (unsigned long)tdes3,
           rx, (unsigned long)eth_pkt_last_rx(0)->rdes3);

    /* ---------- 3. Check ---------- */
    bool pass = (tx == (int)length) && ((tdes3 & EQOS_TDES3_WB_ES) == 0U);
    if (expect_rx)
        pass = pass && (rx == (int)length);
    else
        pass = pass && (rx == ETH_PKT_ETIMEOUT);

    printf("\r\n========== Drop Non TCP/UDP test: %s ==========\r\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
