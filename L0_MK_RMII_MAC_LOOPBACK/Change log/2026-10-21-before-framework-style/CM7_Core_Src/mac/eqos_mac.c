/* SPDX-License-Identifier: GPL-2.0 */
/*
 * eqos_mac.c - khoi MAC (RM0399 61.9.3): [8] bo loc, dia chi MAC, ETH_MACCR,
 * roi bat bo phat/bo thu (TE/RE) sau khi DMA da chay.
 */
#include "../core/eqos.h"
#include "../core/eqos_hw.h"
#include "../flow/eqos_flow.h"
#include <stdio.h>

#if FLOW_LOG >= 2
// Bang truong bit cac thanh ghi MAC (ten giong RM0399), dung de giai ma trong log
static const flow_field_t F_MACCR[]     = {{"RE",0,0},{"TE",1,1},{"PRELEN",3,2},{"DO",10,10},{"ECRSFD",11,11},{"LM",12,12},
                                           {"DM",13,13},{"FES",14,14},{"JE",16,16},{"JD",17,17},{"WD",19,19},
                                           {"ACS",20,20},{"CST",21,21},{"S2KP",22,22},{"GPSLCE",23,23},{"IPG",26,24},{"IPC",27,27}};
static const flow_field_t F_MACPFR[]    = {{"PR",0,0},{"HUC",1,1},{"HMC",2,2},{"DAIF",3,3},{"PM",4,4},{"DBF",5,5},
                                           {"PCF",7,6},{"SAIF",8,8},{"SAF",9,9},{"HPF",10,10},{"RA",31,31}};
#endif

/* [8] Cau hinh MAC (RM0399 61.9.3 buoc 1, 2, 5 - tr.3032-3033):
 * ETH_MACPFR, ETH_MACA0HR/LR, ETH_MACCR. Chua bat TE/RE: viec do o eqos_start_mac(). */
void eqos_config_mac(const struct mac_cfg *c, const struct link_cfg *l)
{
  // [FLOW2]
  FLOW_STEP("[8]", "Cau hinh MAC (bo loc, dia chi, MACCR), roi khoi dong: DMA truoc, MAC sau");
  /* MAC configuration is assembled below; TE/RE are added by eqos_start_mac().*/

  /* MAC packet filter PR(Promiscuous) accepts frames irrespective of destination-address
   * matching, useful because the test destination differs from this MAC address.
   * Promiscuous mode is not a bypass for every integrity/error check.
   * Current code only || PR when CFG.mac.promiscuous is true; false does not explicitly clear an
   * already-set PR bit (SWR at step [4] clears it on every boot).*/
  // Doc bo loc ETH_MACPFR
  uint32_t mac_filter = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
  // promiscuous = true ->
  if (c->promiscuous)
    // dat PR (bit 0): nhan frame bat ke dia chi dich
    mac_filter |= EQOS_MAC_PKT_FILTER_PR; //Promiscuous Mode
  // Ghi
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, mac_filter);
  // [FLOW]
  FLOW("[8] Cau hinh MAC:\r\n");
  flow_reg_f("ETH_MACPFR (PR = 1)", EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, F_MACPFR, FLOW_N(F_MACPFR));

  /* Program perfect-filter address slot 0, byte by byte into its register words.
   * For 00:80:E1:00:00:00 this gives HIGH=0x00000000 and LOW=0x00E18000.
   * The packing is register byte order, not a reversal of bytes in the frame.
   * Source MAC bytes are also explicitly copied into the test Ethernet header. */
  // ETH_MACA0HR = byte 5 (bit 15:8) | byte 4 (bit 7:0) = 0 (bit 31 AE luon = 1)
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS0_HIGH,
      ((uint32_t)c->addr[5] << 8) | c->addr[4]);
  // ETH_MACA0LR = byte 3..0 vao bit 31:0 = 0x00E18000. Ep (uint32_t) de dich 24 bit khong tran kieu int
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS0_LOW,
      ((uint32_t)c->addr[3] << 24) |
      ((uint32_t)c->addr[2] << 16) |
      ((uint32_t)c->addr[1] << 8) | c->addr[0]);
  // [FLOW] HIGH doc ra 0x80000000 vi bit 31 (AE) luon = 1
  flow_reg("ETH_MACA0HR (MAC byte 5:4)", EQOS_MAC_BASE, EQOS_MAC_ADDRESS0_HIGH);
  flow_reg("ETH_MACA0LR (MAC byte 3:0)", EQOS_MAC_BASE, EQOS_MAC_ADDRESS0_LOW);

  /* MAC_Configuration fields used by this experiment:
   * CST/ACS control receive FCS/padding stripping according to frame Type/Length.
   * They do not describe TX CRC insertion. With EtherType 0x88B5 there is no
   * length field telling the MAC that the custom payload is only eight bytes;
   * RX data can therefore contain trailing minimum-frame padding.
   * FES=1 selects 100 Mbit/s; FES=0 selects 10 Mbit/s on this 10/100 MAC.
   * DM=1 selects full duplex; DM=0 half duplex. CFG.link (100 Mbit/s, full
   * duplex) sets both bits; these are fixed values, not negotiated by a PHY.
   * LM loops TX into MAC RX internally. It does not prove the cable/PHY path.
   * WD=0/JD=0 keep watchdog/jabber protection active; JE (CFG.mac.je) raises the
   * giant limit to 9018 and the watchdog/jabber cut-off to 10240 bytes.
   * No checksum-offload, timestamping or pause-flow-control setup is added here. */
  // Dung ETH_MACCR trong bien, bat dau tu 0 (khong doc thanh ghi cu)
  uint32_t mac_control=0;
  // CST (bit 21): bo FCS o frame Type
  if (c->cst)
    mac_control |= EQOS_MAC_CFG_CST; //RC stripping for Type packets
  // ACS (bit 20): bo pad/FCS o frame Length < 1536
  if (c->acs)
    mac_control |= EQOS_MAC_CFG_ACS; //Automatic Pad or CRC Stripping
  // JE (bit 16): jumbo -> giant 9018, watchdog RX + jabber TX 2048 -> 10240 byte
  if (c->je)
    mac_control |= EQOS_MAC_CFG_JE; //Jumbo Packet Enable
  // WD (bit 19): ten bit nguoc ("Watchdog Disable") -> 1 = TAT watchdog RX, nhan toi 16383 byte
  if (c->wd)
    mac_control |= EQOS_MAC_CFG_WD; //Watchdog Disable
  // speed = 100 (CFG.link) -> FES (bit 14) = 1: 100 Mbit/s
  if (l->speed == EQOS_SPEED_100)
    mac_control |= EQOS_MAC_CFG_FES;
  // full_duplex = true -> DM (bit 13) = 1: full duplex, bat buoc khi loopback (RM0399 61.5.8, tr.2998)
  if (l->full_duplex)
    mac_control |= EQOS_MAC_CFG_DM;
  // loopback = true -> dat LM (bit 12): MAC tu vong TX sang RX
  if (c->loopback)
    mac_control |= EQOS_MAC_CFG_LM;
  // ETH_MACWTR (RM0399 tr.3121): PWE (bit 8) = 1 -> watchdog cat frame RX o nguong WTO (bit 3:0)
  // thay cho 2048 / 10240 (JE). Chi co tac dung khi WD = 0. Luon ghi de xoa gia tri cu khi pwe = false.
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_WATCHDOG_TIMEOUT,
      (c->pwe ? EQOS_MAC_WTR_PWE : 0u) | ((uint32_t)c->wto & EQOS_MAC_WTR_WTO_MASK));
  // [FLOW]
  flow_reg("ETH_MACWTR (PWE bit 8, WTO bit 3:0)", EQOS_MAC_BASE, EQOS_MAC_WATCHDOG_TIMEOUT);
  // In gia tri chua co TE/RE -> 317000 (JE = 1; 307000 neu JE = 0; them 80000 neu WD = 1)
  printf("MAC Control: %lx\n",mac_control);
  // Ghi ETH_MACCR = cac bit tren, CHUA co TE/RE -> mong doi 0x00317000 (doc lai 0x0031F000: bit 15 PS luon = 1)
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, mac_control); //Apply mac config
  // [FLOW]
  flow_reg_f("ETH_MACCR (0x00317000)", EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, F_MACCR, FLOW_N(F_MACCR));
}

/* [8] Bat bo phat va bo thu MAC (RM0399 61.9.3 buoc 6, tr.3033).
 * TE and RE enable the MAC transmitter and receiver. These are independent
 * of DMA ST/SR: both sides of the pipeline must be active for frame movement.
 * Ring registration, MTL configuration and DMA start (eqos_start_dma()) have
 * completed by this point, so a received frame always has somewhere to go.
 * eth.started below is only a software flag; it does not read hardware state. */
void eqos_start_mac(void)
{
  // Doc ETH_MACCR (gia tri eqos_config_mac() vua ghi)
  uint32_t mac_control = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);

  printf("============================= mac_control:%x\n", mac_control );
  // Them TE (bit 1, bat bo phat) | RE (bit 0, bat bo thu) -> mong doi 0x00317003
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION,
                      mac_control
                      | EQOS_MAC_CFG_TE //Start transmit
                      | EQOS_MAC_CFG_RE); //Start receive
  // [FLOW] Tu day MAC chay
  flow_reg_f("ETH_MACCR (0x00317003)", EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, F_MACCR, FLOW_N(F_MACCR));
#if FLOW_LOG
  // [FLOW] Tom tat cac bit chinh, lay tu gia tri DOC LAI (khong in cung) nen luon dung voi phan cung
  uint32_t maccr = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);
  FLOW("[8] MAC: CST=%d ACS=%d LM=%d (loopback) FES=%d (%s) DM=%d (%s) TE=%d RE=%d\r\n",
       (maccr & EQOS_MAC_CFG_CST) != 0U, (maccr & EQOS_MAC_CFG_ACS) != 0U, (maccr & EQOS_MAC_CFG_LM) != 0U,
       (maccr & EQOS_MAC_CFG_FES) != 0U, (maccr & EQOS_MAC_CFG_FES) ? "100M" : "10M",
       (maccr & EQOS_MAC_CFG_DM) != 0U, (maccr & EQOS_MAC_CFG_DM) ? "full duplex" : "half duplex",
       (maccr & EQOS_MAC_CFG_TE) != 0U, (maccr & EQOS_MAC_CFG_RE) != 0U);
#endif
  // [FLOW2] Trang thai bo phat/thu MAC (phai idle) va bo dem MMC ban dau (phai = 0)
  flow_mac_state();
  flow_mmc();
  //Set software flag.
  // Co phan mem "da khoi dong xong" (khong doc lai phan cung)
  eth.started = true;
}
