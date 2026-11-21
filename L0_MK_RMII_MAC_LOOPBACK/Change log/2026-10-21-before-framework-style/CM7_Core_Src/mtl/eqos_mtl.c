/* SPDX-License-Identifier: GPL-2.0 */
/*
 * eqos_mtl.c - khoi MTL (RM0399 61.9.2): [7] cau hinh hang doi TX/RX queue 0,
 * tuc 2 FIFO nam giua DMA va MAC.
 */
#include "../core/eqos.h"
#include "../core/eqos_hw.h"
#include "../flow/eqos_flow.h"

#if FLOW_LOG >= 2
// Bang truong bit cac thanh ghi MTL (ten giong RM0399), dung de giai ma trong log
static const flow_field_t F_MTLTXQOMR[] = {{"FTQ",0,0},{"TSF",1,1},{"TXQEN",3,2},{"TTC",6,4},{"TQS",18,16}};
static const flow_field_t F_MTLRXQOMR[] = {{"RTC",1,0},{"FUP",3,3},{"FEP",4,4},{"RSF",5,5},{"DIS_TCP_EF",6,6},{"RQS",22,20}};
#endif

/* [7] Cau hinh MTL queue 0 (RM0399 61.9.2, tr.3032): ETH_MTLTXQOMR, ETH_MTLRXQOMR. */
void eqos_config_mtl(const struct mtl_cfg *c)
{
  // [FLOW2]
  FLOW_STEP("[7]", "Cau hinh MTL: hang doi TX/RX (FIFO giua DMA va MAC)");
  /* MTL TX queue 0 (MTL queue base + 0x00):
   * TSF selects store-and-forward: accumulate the full frame before passing it
   * to the MAC, reducing underrun risk compared with threshold-based forwarding.
   * FTQ (Flush transmit queue) is NOT set here (da sua review #3): right after SWR
   * the TX FIFO is already empty, and RM0399 61.9.2 does not list FTQ in MTL
   * initialization. The old code set it when !duplex_full: a workaround the
   * pre-inline driver ran in eqos_adjust_link() when the link switches to half
   * duplex at run time. FTQ is a command that hardware clears; until it reads
   * back 0, ETH_MTLTXQOMR must not be written again (tr.3102). No TX is submitted yet. */
  // Doc ETH_MTLTXQOMR (reset 0x00070008: TQS = 7 -> FIFO 2 KB, TXQEN = 10b da bat san)
  uint32_t mtl_tx_control = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE);
  // TSF (bit 1): cho du ca frame trong FIFO roi moi day xuong MAC
  if (c->tsf)
    mtl_tx_control |= EQOS_MTL_TXQ_TSF;
  // Ghi -> 0x0007000A (TSF + TXQEN + TQS)
  AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE, mtl_tx_control);
  // [FLOW] Doc lai phai = 0x0007000A
  FLOW("[7] Cau hinh MTL (FIFO giua DMA va MAC):\r\n");
  // [FLOW2] Gia tri vua ghi (doc lai ben duoi phai giong)
  FLOW2("      ETH_MTLTXQOMR gia tri da ghi = 0x%08lX\r\n", (unsigned long)mtl_tx_control);
  flow_reg_f("ETH_MTLTXQOMR (0x0007000A)", EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE, F_MTLTXQOMR, FLOW_N(F_MTLTXQOMR));


  /* MTL RX queue 0 (queue base + 0x30):
   * RSF selects store-and-forward so a frame is collected before DMA delivery.
   * DISTCPEF disables dropping based on TCP/IP checksum errors; it does not
   * disable Ethernet FCS validation or make descriptor errors acceptable.
   * The test uses a private EtherType/raw payload and no IP checksum offload.
   * Queue-size/scheduling controls of other EQoS integrations are not configured. */
  // Doc ETH_MTLRXQOMR (reset 0x00700000: RQS = 7 -> FIFO 2 KB, chi doc)
  uint32_t mtl_rx_control = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE);
  // RSF (bit 5): gom du frame roi moi giao DMA
  if (c->rsf)
    mtl_rx_control |= EQOS_MTL_RXQ_RSF; //Receive Queue Store and Forward
  // DIS_TCP_EF (bit 6): khong bo frame chi vi loi checksum TCP/IP
  if (c->dis_tcp_ef)
    mtl_rx_control |= EQOS_MTL_RXQ_DISTCPEF; //Disable Dropping of TCP/IP Checksum Error Packets
  // Ghi -> 0x00700060
  AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, mtl_rx_control);
  // [FLOW]
  flow_reg_f("ETH_MTLRXQOMR (0x00700060)", EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, F_MTLRXQOMR, FLOW_N(F_MTLRXQOMR));
  // [FLOW2] Ca 2 FIFO phai rong luc nay
  flow_mtl_state();
}
