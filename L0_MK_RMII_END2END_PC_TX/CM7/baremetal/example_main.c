/* SPDX-License-Identifier: GPL-2.0 */
/*
 * example_main.c - vi du dung driver: gui mot ARP request roi doi tra loi.
 */

#include <string.h>
#include <stdio.h>
#include "eqos.h"

static struct eqos_dev eth;

static const uint8_t my_mac[6] = { 0x02, 0x00, 0x00, 0x12, 0x34, 0x56 };
static const uint8_t my_ip[4]  = { 192, 168, 1, 100 };
static const uint8_t peer_ip[4] = { 192, 168, 1, 1 };

/* Dung mot ARP request (42 byte) */
static uint32_t build_arp_request(uint8_t *buf)
{
    memset(buf, 0, 42);

    /* Ethernet header */
    memset(&buf[0], 0xff, 6);          /* dest = broadcast */
    memcpy(&buf[6], my_mac, 6);        /* src  */
    buf[12] = 0x08; buf[13] = 0x06;    /* ethertype = ARP */

    /* ARP payload */
    buf[14] = 0x00; buf[15] = 0x01;    /* HTYPE = ethernet */
    buf[16] = 0x08; buf[17] = 0x00;    /* PTYPE = IPv4 */
    buf[18] = 6;                       /* HLEN */
    buf[19] = 4;                       /* PLEN */
    buf[20] = 0x00; buf[21] = 0x01;    /* OPER = request */
    memcpy(&buf[22], my_mac, 6);       /* sender MAC */
    memcpy(&buf[28], my_ip, 4);        /* sender IP  */
    /* target MAC = 0 */
    memcpy(&buf[38], peer_ip, 4);      /* target IP  */

    return 42;
}

int main(void)
{
    struct eqos_cfg cfg;
    uint8_t txbuf[64];
    uint8_t *rx;
    uint32_t len;
    int ret;

    memset(&cfg, 0, sizeof(cfg));
    cfg.base       = 0x30bf0000;              /* <-- base EQoS cua SoC ban */
    cfg.csr_clk_hz = 250000000;               /* <-- clock CSR that su */
    cfg.phy_addr   = EQOS_PHY_ADDR_AUTO;
    cfg.phy_iface  = EQOS_IFACE_RGMII_ID;
    memcpy(cfg.mac_addr, my_mac, 6);

    ret = eqos_init(&eth, &cfg);
    if (ret != EQOS_OK) {
        printf("eqos_init loi: %d\n", ret);
        eqos_dump_regs(&eth);
        return 1;
    }

    len = build_arp_request(txbuf);
    ret = eqos_send(&eth, txbuf, len);
    if (ret != EQOS_OK) {
        printf("eqos_send loi: %d\n", ret);
        return 1;
    }
    printf("da gui ARP request %u byte\n", len);

    /* Doi toi da 2 giay */
    for (int i = 0; i < 2000; i++) {
        ret = eqos_recv(&eth, &rx);

        if (ret > 0) {
            printf("nhan %d byte: %02x:%02x:%02x:%02x:%02x:%02x -> "
                   "%02x:%02x:%02x:%02x:%02x:%02x  type %02x%02x\n",
                   ret,
                   rx[6], rx[7], rx[8], rx[9], rx[10], rx[11],
                   rx[0], rx[1], rx[2], rx[3], rx[4], rx[5],
                   rx[12], rx[13]);

            eqos_recv_done(&eth);   /* BAT BUOC sau khi xu ly xong */

            if (rx[12] == 0x08 && rx[13] == 0x06)
                break;              /* la ARP reply */
        } else if (ret != EQOS_EAGAIN) {
            printf("eqos_recv loi: %d\n", ret);
        }

        eqos_udelay(1000);
    }

    eqos_dump_regs(&eth);
    eqos_stop(&eth);
    return 0;
}
