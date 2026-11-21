/* SPDX-License-Identifier: GPL-2.0 */
/*
 * eqos_dma.c - khoi DMA (RM0399 61.9.1): [5] cau hinh kenh 0 khi DMA con dung,
 * [8] bat DMA TX/RX sau khi ring da san sang. Ring (DLAR/RLR/tail) o desc/eqos_desc.c.
 */
#include "../core/eqos.h"
#include "../core/eqos_hw.h"
#include "../flow/eqos_flow.h"

#if FLOW_LOG >= 2
// Bang truong bit cac thanh ghi DMA (ten giong RM0399), dung de giai ma trong log
static const flow_field_t F_DMASBMR[]   = {{"FB",0,0},{"AAL",12,12},{"MB",14,14},{"RB",15,15}};
static const flow_field_t F_DMACCR[]    = {{"MSS",13,0},{"PBLX8",16,16},{"DSL",20,18}};
static const flow_field_t F_DMACTXCR[]  = {{"ST",0,0},{"OSF",4,4},{"TSE",12,12},{"TXPBL",21,16}};
static const flow_field_t F_DMACRXCR[]  = {{"SR",0,0},{"RBSZ",14,1},{"RXPBL",21,16},{"RPF",31,31}};
#endif

/* [5] Cau hinh DMA kenh 0 (RM0399 61.9.1 buoc 3, 7, 8 - tr.3031-3032):
 * ETH_DMACTXCR, ETH_DMACRXCR, ETH_DMACCR, ETH_DMASBMR, ETH_DMACIER.
 * CHUA bat ST/SR: viec do nam o eqos_start_dma(). */
void eqos_config_dma(const struct dma_cfg *c)
{
  // [FLOW2]
  FLOW_STEP("[5]", "Cau hinh DMA kenh 0 (TX control, RX control, channel, bus)");
  // [FLOW]
  FLOW("[5] Cau hinh DMA kenh 0 (doc lai sau moi lan ghi):\r\n");
  /* DMA channel-0 TX control (DMA base + 0x104):
   * OSP (Operate on second packets) allows processing the next packet while the previous packet completes.
   * TXPBL (Tx programmable burst-length) is a programmable burst-length field, in bus beats, not bytes.
   * With TXPBL=8 and PBLX8=1 below, the configured limit is 8*8=64 beats
   * (256 bytes on a 32-bit bus); actual transfers can be shorter/split.
   * ST bit (Start transmit) is set later, in eqos_start_dma().
   */
  // Doc ETH_DMACTXCR (sau SWR = 0)
  uint32_t tx_dma_control = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL);
  // [FLOW2] Gia tri truoc khi sua (sau SWR thuong = 0)
  FLOW2("      ETH_DMACTXCR truoc khi ghi = 0x%08lX\r\n", (unsigned long)tx_dma_control);
  // (2 dong xoa field da bi comment -> chi OR; dung vi gia tri reset = 0)
  // tx_dma_control &= ~((EQOS_DMA_CH_TX_TXPBL_MASK << EQOS_DMA_CH_TX_TXPBL_SHIFT) |
  //                     EQOS_DMA_CH_TX_ST); //Transmit Programmable Burst Length
  // OSP bit 4 (RM goi la OSF)
  if (c->osf)
    tx_dma_control |= EQOS_DMA_CH_TX_OSP; //Operate on Second Packet
  // TXPBL = 8 o bit 21:16
  tx_dma_control |= ((uint32_t)c->txpbl & EQOS_DMA_CH_TX_TXPBL_MASK) << EQOS_DMA_CH_TX_TXPBL_SHIFT; // Transmit Programmable Burst Length
  // Ghi mot lan -> mong doi 0x00080010 (ST = 1 duoc them sau, trong eqos_start_dma())
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL, tx_dma_control);
  // [FLOW]
  flow_reg_f("ETH_DMACTXCR (0x00080010)", EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL, F_DMACTXCR, FLOW_N(F_DMACTXCR));

  /* DMA channel-0 RX control (DMA base + 0x108):
   * Clear RBSZ/RXPBL/SR locally, then supply the intended field values.
   * RBSZ=1600 is the capacity of EACH DMA RX buffer; the register field is
   * shifted by one bit, so do not write the unshifted size to the register.
   * RXPBL=8 is scaled by PBLX8 just like TXPBL. SR (start receive) is set
   * later, in eqos_start_dma(). */
  // Doc ETH_DMACRXCR
  uint32_t rx_dma_control = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RX_CONTROL);
  // [FLOW2]
  FLOW2("      ETH_DMACRXCR truoc khi ghi = 0x%08lX\r\n", (unsigned long)rx_dma_control);
  // Xoa trong bien 3 field: RBSZ (bit 14:1), RXPBL (bit 21:16), SR (bit 0)
  rx_dma_control &= ~((EQOS_DMA_CH_RX_RBSZ_MASK << EQOS_DMA_CH_RX_RBSZ_SHIFT) |
                      (EQOS_DMA_CH_RX_RXPBL_MASK << EQOS_DMA_CH_RX_RXPBL_SHIFT) |
                      EQOS_DMA_CH_RX_SR);
  // RBSZ = 1600 dich trai 1 (= 0xC80) | RXPBL = 8
  rx_dma_control |= (EQOS_BUF_SIZE << EQOS_DMA_CH_RX_RBSZ_SHIFT)  //Receive Buffer size
                  | (((uint32_t)c->rxpbl & EQOS_DMA_CH_RX_RXPBL_MASK) << EQOS_DMA_CH_RX_RXPBL_SHIFT); //Receive Programmable Burst Length
  // Ghi mot lan -> mong doi 0x00080C80 (SR = 1 duoc them sau, trong eqos_start_dma())
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RX_CONTROL,rx_dma_control);
  // [FLOW]
  // [FLOW2] Giai ma: field RBSZ nam o bit 14:1 nen in ra RBSZ=1600
  flow_reg_f("ETH_DMACRXCR (0x00080C80)", EQOS_DMA_BASE, EQOS_DMA_CH0_RX_CONTROL, F_DMACRXCR, FLOW_N(F_DMACRXCR));

  /* DMA channel control (DMA base + 0x100):
   * PBLX8 multiplies both burst-length fields by eight. It does NOT select DSL.
   * DSL is the number of bus-width units skipped after each 16-byte descriptor.
   * This packed array needs DSL=0.*/
  // Doc ETH_DMACCR
  uint32_t dma_control = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_CONTROL);
  // [FLOW2]
  FLOW2("      ETH_DMACCR truoc khi ghi = 0x%08lX\r\n", (unsigned long)dma_control);

  // (Dong xoa DSL bi comment; DSL van = 0 nho reset)
  // dma_control &= ~(EQOS_DMA_CH_CTRL_DSL_MASK << EQOS_DMA_CH_CTRL_DSL_SHIFT);
  // PBLX8 (bit 16) -> burst thuc te = 8 x 8 = 64 beat = 256 byte
  if (c->pblx8)
    dma_control |= EQOS_DMA_CH_CTRL_PBLX8; /* Scale PBL by 8; DSL is unchanged here. */
  // Ghi -> mong doi 0x00010000
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_CONTROL,dma_control);
  // [FLOW]
  flow_reg_f("ETH_DMACCR (0x00010000)", EQOS_DMA_BASE, EQOS_DMA_CH0_CONTROL, F_DMACCR, FLOW_N(F_DMACCR));

  /* AHB system-bus mode: AAL requests address-aligned bursts; FB selects fixed
   * burst behavior. H755 uses the AHB variant: generic EQoS AXI BLEN/EAME fields
   * must not be substituted here.
   * DMACIER (DMA channel interrupt enable)=0 selects polling; it does not clear DMACSR or stop DMA transfers. */
  // Ghi thang ETH_DMASBMR = AAL (bit 12) | FB (bit 0) = 0x00001001 (khong doc truoc)
  uint32_t sysbus_mode = 0U;
  if (c->aal)
    sysbus_mode |= EQOS_DMA_SYSBUS_AHB_AAL; //Address-Aligned Beats
  if (c->fb)
    sysbus_mode |= EQOS_DMA_SYSBUS_AHB_FB; //  Fixed Burst Length
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_SYSBUS_MODE, sysbus_mode);
  // Ghi ETH_DMACIER = 0: tat moi nguon ngat DMA (polling)
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_INTERRUPT_ENABLE,0U); //Disable Interrupt
  // [FLOW]
  flow_reg_f("ETH_DMASBMR (0x00001001)", EQOS_DMA_BASE, EQOS_DMA_SYSBUS_MODE, F_DMASBMR, FLOW_N(F_DMASBMR));
  flow_reg("ETH_DMACIER (0)", EQOS_DMA_BASE, EQOS_DMA_CH0_INTERRUPT_ENABLE);
}

/* [8] Bat DMA TX va RX (RM0399 61.9.1 buoc 9, tr.3032).
 * ST (Start transmit) starts TX DMA; SR (Start receive) starts RX DMA.
 * Goi SAU eqos_setup_rings(): DMA lay dia chi ring trong DLAR dung luc ST/SR
 * chuyen 0 -> 1 (tr.3083/3084), nen DLAR/RLR/tail phai duoc ghi truoc (da sua review #1).
 * Goi TRUOC eqos_start_mac(): bo thu MAC chi nen bat khi DMA da chay, neu
 * khong frame ve se lam day FIFO RX (RM0399 61.9.3, tr.3032). */
void eqos_start_dma(void)
{
  // Doc ETH_DMACTXCR, them ST (bit 0) -> mong doi 0x00080011
  uint32_t tx_dma_control = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL);
  tx_dma_control |= EQOS_DMA_CH_TX_ST; //Start or Stop Transmission Command
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL, tx_dma_control);
  // Doc ETH_DMACRXCR, them SR (bit 0) -> mong doi 0x00080C81
  uint32_t rx_dma_control = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RX_CONTROL);
  rx_dma_control |= EQOS_DMA_CH_RX_SR; //Start or Stop Receive
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RX_CONTROL, rx_dma_control);
  // [FLOW]
  FLOW("[8] Bat DMA: ST = 1 (TX), SR = 1 (RX)\r\n");
  flow_reg_f("ETH_DMACTXCR (0x00080011)", EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL, F_DMACTXCR, FLOW_N(F_DMACTXCR));
  flow_reg_f("ETH_DMACRXCR (0x00080C81)", EQOS_DMA_BASE, EQOS_DMA_CH0_RX_CONTROL, F_DMACRXCR, FLOW_N(F_DMACRXCR));
  // [FLOW] ETH_DMADSR (offset 0x00C, chua co ten trong eqos_hw.h): TPS0 bit 15:12, RPS0 bit 11:8
  // = trang thai may DMA ngay sau khi bat: TX tam dung (tail = vi tri hien tai, chua co gi de gui),
  // RX dang cho frame (tail = &rx_desc[7], descriptor deu OWN = 1)
  flow_reg("ETH_DMADSR (TPS/RPS)", EQOS_DMA_BASE, 0x00CU);
  // [FLOW2] Dich trang thai ra chu
  flow_dma_state();
}
