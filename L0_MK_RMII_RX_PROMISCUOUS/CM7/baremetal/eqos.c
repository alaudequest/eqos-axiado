/* SPDX-License-Identifier: GPL-2.0 */
/* STM32H755 DWC EQoS polling driver.
 * Derived from the supplied driver: Copyright (c) 2016, NVIDIA CORPORATION.
 */
#include "stm32h7xx_hal.h"
#include "lan8742.h"
#include "eqos.h"
#include "eqos_hw.h"
#include <stdio.h>
#include <string.h>

static struct eqos_dev *active_dev;
static lan8742_Object_t LAN8742;
static bool tx_faulted;

struct eqos_desc {
  volatile uint32_t des0, des1, des2, des3;
};
/* Rings: 4 TX and 8 RX entries by current eqos.h defaults. The linker places
 * .eqos_nocache in DMA-accessible RAM_D1, with NOLOAD: initialize every word
 * explicitly below. D-cache must be disabled globally.
 * Buffers are also in RAM_D1 (.bss).
 * 32-byte alignment is the M7 cache-line alignment; 
 * Descriptors remain packed at a 16-byte stride inside each ring. DSL (Descriptor skip length) must therefore remain zero.
 * DSl: extra space the DMA skips after a descriptor before reading the next one. Zero means descriptors are continuos.
 */
static struct eqos_desc tx_desc[EQOS_TX_DESC_COUNT]
  __attribute__((aligned(32), section(".eqos_nocache")));
static struct eqos_desc rx_desc[EQOS_RX_DESC_COUNT]
  __attribute__((aligned(32), section(".eqos_nocache")));
static uint8_t tx_buf[EQOS_TX_DESC_COUNT][EQOS_BUF_SIZE] __attribute__((aligned(32)));
static uint8_t rx_buf[EQOS_RX_DESC_COUNT][EQOS_BUF_SIZE] __attribute__((aligned(32)));

/* These checks protect assumptions in the DMA configuration and wrap logic:
 * index = (index + 1) & (count - 1) requires a nonzero power-of-two ring count.
 * RBSZ describes each RX buffer's capacity, not the Ethernet MTU or packet size.
 * A 1600-byte buffer covers an ordinary 1518-byte frame / 1522-byte VLAN frame.
 * Jumbo/multi-descriptor packet reassembly is not implemented here. */
_Static_assert(sizeof(struct eqos_desc) == 16, "DMA descriptor stride must be 16 bytes");
_Static_assert(EQOS_TX_DESC_COUNT > 0 &&
  (EQOS_TX_DESC_COUNT & (EQOS_TX_DESC_COUNT - 1)) == 0, "TX ring must be power of two");
_Static_assert(EQOS_RX_DESC_COUNT > 0 &&
  (EQOS_RX_DESC_COUNT & (EQOS_RX_DESC_COUNT - 1)) == 0, "RX ring must be power of two");
_Static_assert(EQOS_BUF_SIZE % 32 == 0 && EQOS_BUF_SIZE <= 0x3FFF,
  "RX buffer must be aligned and fit RBSZ");
#if !EQOS_STM32H7 || EQOS_DESC_CACHED || EQOS_BUF_CACHED
#error "This driver requires STM32H755 with uncached DMA storage"
#endif

static int32_t ETH_PHY_INTERFACE_Init(void);
static int32_t ETH_PHY_INTERFACE_DeInit(void);
static int32_t ETH_PHY_INTERFACE_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal);
static int32_t ETH_PHY_INTERFACE_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal);
static int32_t ETH_PHY_INTERFACE_GetTick(void);
static int32_t ETH_PHY_INTERFACE_WaitIdle(void);
/* Original LAN8742 component calls these MDIO/tick functions through its bus context.  */
static lan8742_IOCtx_t LAN8742_IOCtx = {
  ETH_PHY_INTERFACE_Init, ETH_PHY_INTERFACE_DeInit,
  ETH_PHY_INTERFACE_WriteReg, ETH_PHY_INTERFACE_ReadReg, ETH_PHY_INTERFACE_GetTick
};


static int eqos_reset(struct eqos_dev *dev)
{
  eqos_port_init();
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_MODE, EQOS_DMA_MODE_SWR);
  uint32_t elapsed = 0;
  while (AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_MODE) & EQOS_DMA_MODE_SWR) {
    if (elapsed++ >= dev->cfg.swr_timeout_us)
      return EQOS_ETIMEDOUT;
    eqos_udelay(1);
  }
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_US_TIC_COUNTER,
                      dev->cfg.csr_clk_hz / 1000000U - 1U);
  uint32_t hz = dev->cfg.csr_clk_hz;
  if      (hz < 35000000U)  dev->mdio_cr = EQOS_MDIO_CR_20_35;
  else if (hz < 60000000U)  dev->mdio_cr = EQOS_MDIO_CR_35_60;
  else if (hz < 100000000U) dev->mdio_cr = EQOS_MDIO_CR_60_100;
  else if (hz < 150000000U) dev->mdio_cr = EQOS_MDIO_CR_100_150;
  else if (hz < 250000000U) dev->mdio_cr = EQOS_MDIO_CR_150_250;
  else                    dev->mdio_cr = EQOS_MDIO_CR_250_300;
  return EQOS_OK;
}

static int eqos_phy_start(struct eqos_dev *dev)
{
 int32_t status = LAN8742_RegisterBusIO(&LAN8742, &LAN8742_IOCtx);
 if (status == LAN8742_STATUS_OK)
   status = LAN8742_Init(&LAN8742);
 if (status == LAN8742_STATUS_OK)
   status = LAN8742_StartAutoNego(&LAN8742);
 if (status != LAN8742_STATUS_OK) {
   printf("LAN8742 init failed: %ld\r\n", (long)status);
   return EQOS_EIO;
 }
 dev->phy_addr = (int)LAN8742.DevAddr;
 uint32_t start = HAL_GetTick();
 while (!dev->link_up) {
   status = LAN8742_GetLinkState(&LAN8742);
   switch (status) {
   case LAN8742_STATUS_100MBITS_FULLDUPLEX:
   case LAN8742_STATUS_100MBITS_HALFDUPLEX:
     dev->speed = EQOS_SPEED_100;
     dev->duplex_full = (status == LAN8742_STATUS_100MBITS_FULLDUPLEX);
     dev->link_up = true;
     break;
   case LAN8742_STATUS_10MBITS_FULLDUPLEX:
   case LAN8742_STATUS_10MBITS_HALFDUPLEX:
     dev->speed = EQOS_SPEED_10;
     dev->duplex_full = (status == LAN8742_STATUS_10MBITS_FULLDUPLEX);
     dev->link_up = true;
     break;
   case LAN8742_STATUS_LINK_DOWN:
   case LAN8742_STATUS_AUTONEGO_NOTDONE:
     break;
   default:
     printf("LAN8742 link read failed: %ld\r\n", (long)status);
     return EQOS_EIO;
   }
   if (!dev->link_up) {
     if ((uint32_t)(HAL_GetTick() - start) >= dev->cfg.aneg_timeout_ms) {
       printf("LAN8742 link timeout: %ld\r\n", (long)status);
       return EQOS_ENOLINK;
     }
     HAL_Delay(10);
   }
 }
  printf("PHY %d: %d Mbps %s duplex\r\n", dev->phy_addr, (int)dev->speed,
         dev->duplex_full ? "full" : "half");

  return EQOS_OK;
}

static void eqos_dma_configure(void)
{
  /* Configure while stopped. Packed descriptors require DSL=0. */
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL,
      EQOS_DMA_CH_TX_OSP | (8U << EQOS_DMA_CH_TX_TXPBL_SHIFT));
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RX_CONTROL,
      (EQOS_BUF_SIZE << EQOS_DMA_CH_RX_RBSZ_SHIFT) |
      (8U << EQOS_DMA_CH_RX_RXPBL_SHIFT));
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_CONTROL,
      EQOS_DMA_CH_CTRL_PBLX8);
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_SYSBUS_MODE,
      EQOS_DMA_SYSBUS_AHB_AAL | EQOS_DMA_SYSBUS_AHB_FB);
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_INTERRUPT_ENABLE, 0U);
}

static void eqos_rings_init(struct eqos_dev *dev)
{
  for (uint32_t i = 0; i < EQOS_TX_DESC_COUNT; ++i) {
    tx_desc[i].des0 = 0;
    tx_desc[i].des1 = 0;
    tx_desc[i].des2 = 0;
    tx_desc[i].des3 = 0;
  }
  for (uint32_t i = 0; i < EQOS_RX_DESC_COUNT; ++i) {
    rx_desc[i].des0 = (uint32_t)(uintptr_t)rx_buf[i];
    rx_desc[i].des1 = 0;
    rx_desc[i].des2 = 0;
    __DSB();
    rx_desc[i].des3 = EQOS_RDES3_OWN | EQOS_RDES3_BUF1V;
  }
  __DSB();

  /* Tell DMA where each descriptor array starts in RAM: &tx_desc[0] and
   * &rx_desc[0]. Write the full RAM address; do not subtract the EQoS base.
   * Write descriptor count minus 1 into each ring-length register:
   * 4 TX descriptors -> write 3; 8 RX descriptors -> write 7.
   * DMA moves to the next descriptor by adding 16 bytes plus any DSL gap.
   * Here DSL=0, so descriptors are 16 bytes apart. DMA wraps to the start
   * after the last descriptor. tx_head/rx_head are only for our C code;
   * DMA tracks its own position.
   * The tail tells DMA where the available work ends. Updating it can
   * restart DMA after it has stopped there. It refers to descriptors,
   * not packet data. Set OWN=1 to give a descriptor to DMA, and keep the
   * tail address and ring size consistent with the descriptors in use. */
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_LIST_ADDR,
      (uint32_t)(uintptr_t)&tx_desc[0]); //Tx List Address
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_RING_LEN,
      EQOS_TX_DESC_COUNT - 1U); //Tx Ring length
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_LIST_ADDR,
      (uint32_t)(uintptr_t)&rx_desc[0]); //Rx List Address
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_RING_LEN,
      EQOS_RX_DESC_COUNT - 1U);  //Rx Ring length
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR,
      (uint32_t)(uintptr_t)&tx_desc[0]); //Tx Tail Pointer
  
  /* Setting tail Rx Desc tail pointer: initial tail=&rx_desc[N-1];
   * recycling later writes the returned descriptor's address.
   * On a stall, inspect OWN, current RX descriptor and tail together;
   * exercise more than N packets to verify wraparound on the actual hardware. */
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_TAIL_PTR,
      (uint32_t)(uintptr_t)&rx_desc[EQOS_RX_DESC_COUNT - 1U]);//Rx Tail Pointer
  dev->tx_head = 0;
  dev->rx_head = 0;

  
}
static void eqos_mac_configure(const struct eqos_dev *dev)
{
  /* H755 single-queue defaults, with store-and-forward enabled. */
  AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE,
      AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE) |
      EQOS_MTL_TXQ_TSF);
  AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE,
      AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE) |
      EQOS_MTL_RXQ_RSF | EQOS_MTL_RXQ_DISTCPEF);

  /* Own unicast + broadcast; no SA/hash/inverse/receive-all filtering.
   * Set PR explicitly in both directions, including when false. */
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER,
      dev->cfg.promiscuous ? EQOS_MAC_PKT_FILTER_PR : 0U);
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS0_HIGH,
      ((uint32_t)dev->cfg.mac_addr[5] << 8) | dev->cfg.mac_addr[4]);
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS0_LOW,
      ((uint32_t)dev->cfg.mac_addr[3] << 24) |
      ((uint32_t)dev->cfg.mac_addr[2] << 16) |
      ((uint32_t)dev->cfg.mac_addr[1] << 8) | dev->cfg.mac_addr[0]);

  uint32_t mac = EQOS_MAC_CFG_CST | EQOS_MAC_CFG_ACS;
  if (dev->speed == EQOS_SPEED_100) mac |= EQOS_MAC_CFG_FES;
  if (dev->duplex_full) mac |= EQOS_MAC_CFG_DM;
  if (dev->cfg.loopback) mac |= EQOS_MAC_CFG_LM;
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, mac);
}

static bool eqos_ready(const struct eqos_dev *dev)
{
  return dev != NULL && dev == active_dev && dev->started;
}

int eqos_init(struct eqos_dev *dev, const struct eqos_cfg *cfg)
{
  if (dev == NULL || cfg == NULL || cfg->base != EQOS_BASE_ADDR ||
      cfg->phy_iface != EQOS_IFACE_RMII ||
      (cfg->phy_addr != 0 && cfg->phy_addr != EQOS_PHY_ADDR_AUTO) ||
      (cfg->max_speed != 0 && cfg->max_speed != 100) ||
      cfg->tx_fifo_sz != 0 || cfg->rx_fifo_sz != 0 ||
      (cfg->rxq_mode != 0 && cfg->rxq_mode != EQOS_RXQ_MODE_DCB) ||
      cfg->csr_clk_hz < 20000000U || cfg->csr_clk_hz >= 300000000U ||
      (cfg->mac_addr[0] & 1U) != 0U ||
      (SCB->CCR & SCB_CCR_DC_Msk) != 0U)
    return EQOS_EINVAL;
  if (active_dev != NULL)
    return EQOS_EBUSY;
  /* cfg may point to dev->cfg: copy before clearing the device. */
  struct eqos_cfg saved_cfg = *cfg;
  memset(dev, 0, sizeof(*dev));
  dev->cfg = saved_cfg;
  dev->base = saved_cfg.base;
  dev->desc_stride = sizeof(struct eqos_desc);
  if (dev->cfg.swr_timeout_us == 0) dev->cfg.swr_timeout_us = 100000U;
  if (dev->cfg.aneg_timeout_ms == 0) dev->cfg.aneg_timeout_ms = 5000U;
  active_dev = dev;
  tx_faulted = false;
  memset(&LAN8742, 0, sizeof(LAN8742));

  int ret = eqos_reset(dev);
  if (ret == EQOS_OK) ret = eqos_phy_start(dev);
  if (ret != EQOS_OK) {
    active_dev = NULL;
    return ret;
  }
  eqos_dma_configure();
  eqos_rings_init(dev);
  eqos_mac_configure(dev);
  /* Publish initialized rings before enabling any DMA fetch. */
  __DSB();
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION,
      AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION) |
      EQOS_MAC_CFG_TE | EQOS_MAC_CFG_RE);
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RX_CONTROL,
      AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RX_CONTROL) | EQOS_DMA_CH_RX_SR);
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL,
      AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL) | EQOS_DMA_CH_TX_ST);
  dev->started = true;
  return EQOS_OK;
}

int eqos_set_promiscuous(struct eqos_dev *dev, bool enabled)
{
  if (!eqos_ready(dev)) return EQOS_EINVAL;
  uint32_t filter = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
  filter &= ~EQOS_MAC_PKT_FILTER_PR;
  if (enabled) filter |= EQOS_MAC_PKT_FILTER_PR;
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, filter);
  dev->cfg.promiscuous = enabled;
  return EQOS_OK;
}

int eqos_send(struct eqos_dev *dev, const void *frame, uint32_t length)
{
  if (!eqos_ready(dev) || frame == NULL || length < 14U || length > EQOS_BUF_SIZE)
    return EQOS_EINVAL;
  if (tx_faulted) return EQOS_EIO;
  uint32_t idx = dev->tx_head;
  struct eqos_desc *tx = &tx_desc[idx];
  if (tx->des3 & EQOS_TDES3_OWN) return EQOS_EAGAIN;
  __DSB();
  memcpy(tx_buf[idx], frame, length);
  tx->des0 = (uint32_t)(uintptr_t)tx_buf[idx];
  tx->des1 = 0;
  tx->des2 = length & EQOS_TDES2_B1L_MASK;
  __DSB();
  /* CPC=0: hardware inserts padding/FCS; single-buffer FD|LD packet. */
  tx->des3 = EQOS_TDES3_OWN | EQOS_TDES3_FD | EQOS_TDES3_LD |
             (length & EQOS_TDES3_FL_MASK);
  __DSB();
  dev->tx_head = (idx + 1U) & (EQOS_TX_DESC_COUNT - 1U);
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR,
      (uint32_t)(uintptr_t)&tx_desc[dev->tx_head]);
  uint32_t elapsed = 0;
  uint32_t result;
  do {
    result = tx->des3;
    if (!(result & EQOS_TDES3_OWN)) break;
    eqos_udelay(1);
  } while (++elapsed < 1000000U);
  if (result & EQOS_TDES3_OWN) {
    ++dev->stats.tx_timeouts;
    tx_faulted = true; /* DMA still owns the buffer. Require board reset. */
    return EQOS_ETIMEDOUT;
  }
  __DSB();
  if (result & EQOS_TDES3_WB_ES) {
    ++dev->stats.tx_errors;
    return EQOS_EIO;
  }
  ++dev->stats.tx_packets;
  return EQOS_OK;
}

int eqos_receive(struct eqos_dev *dev, void *buffer, uint32_t capacity,
                 uint32_t *length)
{
  if (length != NULL) *length = 0;
  if (!eqos_ready(dev) || buffer == NULL || length == NULL || capacity == 0)
    return EQOS_EINVAL;
  uint32_t idx = dev->rx_head;
  struct eqos_desc *rx = &rx_desc[idx];
  uint32_t status = rx->des3;
  if (status & EQOS_RDES3_OWN) return EQOS_EAGAIN;
  __DSB();
  uint32_t len = status & EQOS_RDES3_WB_PL_MASK;
  int ret = EQOS_OK;
  if (status & EQOS_RDES3_WB_CTXT) {
    ret = EQOS_EAGAIN;
  } else if (status & EQOS_RDES3_WB_ES) {
    ++dev->stats.rx_errors;
    ret = EQOS_EIO;
  } else if ((status & (EQOS_RDES3_WB_FD | EQOS_RDES3_WB_LD)) !=
              (EQOS_RDES3_WB_FD | EQOS_RDES3_WB_LD) ||
              len == 0 || len > EQOS_BUF_SIZE || len > capacity) {
    ++dev->stats.rx_dropped;
    ret = EQOS_EIO;
  } else {
    memcpy(buffer, rx_buf[idx], len);
    *length = len;
    ++dev->stats.rx_packets;
  }
  /* Write-back destroys the address words. Rebuild all words, even after
   * errors, and give OWN back only after the copy is done. */
  rx->des0 = (uint32_t)(uintptr_t)rx_buf[idx];
  rx->des1 = 0;
  rx->des2 = 0;
  __DSB();
  rx->des3 = EQOS_RDES3_OWN | EQOS_RDES3_BUF1V;
  __DSB();
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_TAIL_PTR,
      (uint32_t)(uintptr_t)rx);
  dev->rx_head = (idx + 1U) & (EQOS_RX_DESC_COUNT - 1U);
  return ret;
}

void eqos_dump_regs(const struct eqos_dev *dev)
{
  if (dev == NULL || dev != active_dev) return;
  printf("MACCR=%08lX MACPFR=%08lX MACA0HR=%08lX MACA0LR=%08lX\r\n",
      (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION),
      (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER),
      (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS0_HIGH),
      (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS0_LOW));
  printf("DMACSR=%08lX TXDESC3=%08lX RXDESC3=%08lX\r\n",
      (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS),
      (unsigned long)tx_desc[(dev->tx_head - 1U) & (EQOS_TX_DESC_COUNT - 1U)].des3,
      (unsigned long)rx_desc[dev->rx_head].des3);
  /* Read once per dump: MMC counters may be configured reset-on-read. */
  printf("MMC TX good=%lu RX CRC errors=%lu RX unicast good=%lu\r\n",
      (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, 0x768U),
      (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, 0x794U),
      (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, 0x7C4U));
  printf("SW TX=%lu TX errors=%lu TX timeouts=%lu RX=%lu RX errors=%lu RX dropped=%lu\r\n",
      (unsigned long)dev->stats.tx_packets, (unsigned long)dev->stats.tx_errors,
      (unsigned long)dev->stats.tx_timeouts, (unsigned long)dev->stats.rx_packets,
      (unsigned long)dev->stats.rx_errors, (unsigned long)dev->stats.rx_dropped);
}

static int32_t ETH_PHY_INTERFACE_Init(void) { return 0; }
static int32_t ETH_PHY_INTERFACE_DeInit(void) { return 0; }
static int32_t ETH_PHY_INTERFACE_GetTick(void) { return (int32_t)HAL_GetTick(); }

/* Serialize Clause-22 management commands through the MAC's MDIO engine.
 * GB (busy) is set by software to launch a command and clears on completion.
 * The HAL tick timeout requires SysTick interrupts; it cannot expire if called
 * with interrupts disabled. These callbacks are intended for main context,
 * with the driver device already registered. */
static int32_t ETH_PHY_INTERFACE_WaitIdle(void)
{
  uint32_t start = HAL_GetTick();
  while ((AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_MDIO_ADDRESS) & EQOS_MDIO_ADDR_GB) != 0U) {
    if ((uint32_t)(HAL_GetTick() - start) >= 1000U)
      return -1;
  }
  return 0;
}

/* Clause 22 uses a 5-bit PHY address PA and 5-bit register address RDA.
 * Wait for any previous command, preserve SKAP, and construct PA/RDA/CR/GOC.
 * GOC=read, C45E=0 (Clause 22), GB=1 launches the transaction.
 * Only sample the low 16 MDIO data bits AFTER busy clears. Return 0/-1 as the
 * LAN8742 component expects; a register value is returned through pRegVal. */
static int32_t ETH_PHY_INTERFACE_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal)
{
  if (pRegVal == NULL || DevAddr > 31U || RegAddr > 31U || ETH_PHY_INTERFACE_WaitIdle() < 0)
    return -1;
  uint32_t mdio_address = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_MDIO_ADDRESS);
  mdio_address &= EQOS_MDIO_ADDR_SKAP;
  mdio_address |= (DevAddr << EQOS_MDIO_ADDR_PA_SHIFT) |
                  (RegAddr << EQOS_MDIO_ADDR_RDA_SHIFT) |
                  (active_dev->mdio_cr << EQOS_MDIO_ADDR_CR_SHIFT) |
                  (EQOS_MDIO_ADDR_GOC_READ << EQOS_MDIO_ADDR_GOC_SHIFT) | EQOS_MDIO_ADDR_GB;
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_MDIO_ADDRESS, mdio_address);
  if (ETH_PHY_INTERFACE_WaitIdle() < 0)
    return -1;
  *pRegVal = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_MDIO_DATA) & EQOS_MDIO_DATA_GD_MASK;
  return 0;
}

/* Write ordering matters: wait idle -> load low 16 data bits -> issue the
 * address/control command with GOC=write and GB=1 -> wait for completion.
 * Constructing one local control value avoids intermediate MDIO commands.
 * The register busy/status reads must stay live even though static config
 * sequences elsewhere use a single snapshot for read/modify/write. */
static int32_t ETH_PHY_INTERFACE_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal)
{
  if (DevAddr > 31U || RegAddr > 31U || ETH_PHY_INTERFACE_WaitIdle() < 0)
    return -1;
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_MDIO_DATA,
      RegVal & EQOS_MDIO_DATA_GD_MASK);
  uint32_t mdio_address = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_MDIO_ADDRESS);
  mdio_address &= EQOS_MDIO_ADDR_SKAP;
  mdio_address |= (DevAddr << EQOS_MDIO_ADDR_PA_SHIFT) |
                  (RegAddr << EQOS_MDIO_ADDR_RDA_SHIFT) |
                  (active_dev->mdio_cr << EQOS_MDIO_ADDR_CR_SHIFT) |
                  (EQOS_MDIO_ADDR_GOC_WRITE << EQOS_MDIO_ADDR_GOC_SHIFT) | EQOS_MDIO_ADDR_GB;
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_MDIO_ADDRESS, mdio_address);
  return ETH_PHY_INTERFACE_WaitIdle();
}

