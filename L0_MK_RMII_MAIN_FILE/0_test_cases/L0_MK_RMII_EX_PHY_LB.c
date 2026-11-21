#include "eth_functions_utils.h"
#include <stdio.h>

int32_t eth_test_enable_phy_loopback(struct eqos_dev *eth, lan8742_Object_t *phy)
{
    printf("\n ========== Begin EQOS External PHY Loopback test ==========\n");

    /* Force the PHY speed/duplex and disable auto-negotiation. */
    int32_t status = LAN8742_SetLinkState(phy, LAN8742_STATUS_100MBITS_FULLDUPLEX);
    if (status == LAN8742_STATUS_OK)
        status = LAN8742_EnableLoopbackMode(phy);
    if (status != LAN8742_STATUS_OK)
        return status;

    uint32_t phy_bcr = 0U;
    const uint32_t loopback_bits = LAN8742_BCR_LOOPBACK |
        LAN8742_BCR_SPEED_SELECT | LAN8742_BCR_DUPLEX_MODE;
    if (phy->IO.ReadReg(phy->DevAddr, LAN8742_BCR, &phy_bcr) < 0)
        return LAN8742_STATUS_READ_ERROR;
    if ((phy_bcr & (loopback_bits | LAN8742_BCR_AUTONEGO_EN)) != loopback_bits) {
        printf("LAN8742 loopback readback failed: BCR=0x%04lX\r\n",
               (unsigned long)phy_bcr);
        return LAN8742_STATUS_ERROR;
    }

    eth->phy_addr = (int)phy->DevAddr;
    eth->speed = EQOS_SPEED_100;
    eth->duplex_full = true;
    eth->link_up = true; /* Local test readiness, not cable link status. */
    eth->cfg.loopback = false; /* Keep MAC LM clear to exercise the PHY. */
    printf("PHY %d: loopback, 100 Mbps full duplex, BCR=0x%04lX\r\n",
           eth->phy_addr, (unsigned long)phy_bcr);
    return LAN8742_STATUS_OK;
}
