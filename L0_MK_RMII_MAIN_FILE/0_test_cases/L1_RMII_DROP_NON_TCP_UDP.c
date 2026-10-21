#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

uint32_t eth_test_drop_non_tcp_udp(struct eqos_dev *eth, uint8_t *buffer,
                                  size_t capacity, uint8_t protocol, bool enable)
{
    printf("\n ========== Begin Drop Non TCP/UDP test ==========\n");
    if (eth == NULL || (protocol != 1U && protocol != 17U))
        return 0U;
    const uint8_t zero = 0U;
    uint32_t length = eth_construct_frame(buffer, capacity, 60U,
        eth->cfg.mac_addr, eth->cfg.mac_addr, 0x0800U, &zero, 1U);
    if (length == 0U)
        return 0U;

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
    memcpy(buffer + 14U, ip, sizeof(ip));
    if (protocol == 1U) {
        /* ICMP echo request, identifier=1, sequence=1, valid checksum. */
        const uint8_t icmp[8] = {8, 0, 0xF7, 0xFD, 0, 1, 0, 1};
        memcpy(buffer + 34U, icmp, sizeof(icmp));
    } else {
        /* UDP ports 1234/5678, length 8. Zero checksum is valid for IPv4. */
        const uint8_t udp[8] = {4, 0xD2, 0x16, 0x2E, 0, 8, 0, 0};
        memcpy(buffer + 34U, udp, sizeof(udp));
    }
    eth->cfg.loopback = true;
    eth->cfg.promiscuous = false;
    /* Normal DA filtering, no PR/RA/VLAN bypass. Enable IP filtering. */
    uint32_t filter = EQOS_MAC_PKT_FILTER_IPFE;
    if (enable)
        filter |= EQOS_MAC_PKT_FILTER_DNTU;
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, filter);
    printf("DNTU=%u protocol=%u: expect %s. Press USER to send.\r\n",
           (unsigned int)enable, (unsigned int)protocol,
           enable && protocol == 1U ? "no RX descriptor" : "RX packet");
    return length;
}
