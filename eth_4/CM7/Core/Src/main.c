
/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
#include "main.h"
#include "lan8742.h"
#include "../../baremetal/eqos.h"
#include "../../baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* Ethernet sequence derived from the supplied GPL-2.0 EQoS driver.
 * Original U-Boot code: Copyright (c) 2016, NVIDIA CORPORATION.
 * GPIO/reset, MAC/MTL/DMA, rings, TX and RX are executed directly in main(). */
COM_InitTypeDef BspCOMInit;
static struct eqos_dev eth;
static lan8742_Object_t LAN8742;
static volatile uint8_t send_requested;
static uint8_t frame[114];
static uint32_t frame_len;

struct eqos_desc {
  volatile uint32_t des0, des1, des2, des3;
};
static struct eqos_desc tx_desc[EQOS_TX_DESC_COUNT]
  __attribute__((aligned(32), section(".eqos_nocache")));
static struct eqos_desc rx_desc[EQOS_RX_DESC_COUNT]
  __attribute__((aligned(32), section(".eqos_nocache")));
static uint8_t tx_buf[EQOS_TX_DESC_COUNT][EQOS_BUF_SIZE] __attribute__((aligned(32)));
static uint8_t rx_buf[EQOS_RX_DESC_COUNT][EQOS_BUF_SIZE] __attribute__((aligned(32)));
_Static_assert(sizeof(struct eqos_desc) == 16, "DMA descriptor stride must be 16 bytes");
_Static_assert(EQOS_TX_DESC_COUNT > 0 &&
  (EQOS_TX_DESC_COUNT & (EQOS_TX_DESC_COUNT - 1)) == 0, "TX ring must be power of two");
_Static_assert(EQOS_RX_DESC_COUNT > 0 &&
  (EQOS_RX_DESC_COUNT & (EQOS_RX_DESC_COUNT - 1)) == 0, "RX ring must be power of two");
_Static_assert(EQOS_BUF_SIZE % 32 == 0 && EQOS_BUF_SIZE <= 0x3FFF,
  "RX buffer must be aligned and fit RBSZ");
#if !EQOS_STM32H7 || EQOS_DESC_CACHED || EQOS_BUF_CACHED
#error "This inline sequence requires STM32H755 with uncached DMA storage"
#endif

void SystemClock_Config(void);
static int32_t ETH_PHY_INTERFACE_Init(void);
static int32_t ETH_PHY_INTERFACE_DeInit(void);
static int32_t ETH_PHY_INTERFACE_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal);
static int32_t ETH_PHY_INTERFACE_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal);
static int32_t ETH_PHY_INTERFACE_GetTick(void);
static int32_t ETH_PHY_INTERFACE_WaitIdle(void);
static lan8742_IOCtx_t LAN8742_IOCtx = {
  ETH_PHY_INTERFACE_Init, ETH_PHY_INTERFACE_DeInit,
  ETH_PHY_INTERFACE_WriteReg, ETH_PHY_INTERFACE_ReadReg, ETH_PHY_INTERFACE_GetTick
};

int main(void)
{
  /* 1. Existing system clock, UART and button. */
  HAL_Init();
  SystemClock_Config();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);
  BspCOMInit.BaudRate = 115200;
  BspCOMInit.WordLength = COM_WORDLENGTH_8B;
  BspCOMInit.StopBits = COM_STOPBITS_1;
  BspCOMInit.Parity = COM_PARITY_NONE;
  BspCOMInit.HwFlowCtl = COM_HWCONTROL_NONE;
  if (BSP_COM_Init(COM1, &BspCOMInit) != BSP_ERROR_NONE)
    Error_Handler();
  printf("Inline Ethernet sequence starting\r\n");

  eth.cfg = (struct eqos_cfg) {
    .base = EQOS_BASE_ADDR,
    .csr_clk_hz = HAL_RCC_GetHCLKFreq(),
    .phy_iface = EQOS_IFACE_RMII,
    .mac_addr = {0x00, 0x80, 0xE1, 0x00, 0x00, 0x00},
    .max_speed = 100,
    .promiscuous = true,
    .loopback = true,
    .aneg_timeout_ms = 5000,
  };
  eth.base = eth.cfg.base;
  eth.desc_stride = sizeof(struct eqos_desc);
  if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
    printf("Keep D-cache disabled for this DMA bring-up\r\n");
    Error_Handler();
  }

  /* 2. DWT delay clock, Ethernet GPIO AF11, RMII selection and RCC reset. */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55U;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN |
                  RCC_AHB4ENR_GPIOCEN | RCC_AHB4ENR_GPIOGEN;
  RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
  (void)RCC->AHB4ENR;
  (void)RCC->APB4ENR;
  const struct { GPIO_TypeDef *port; uint32_t pin; } rmii_pins[] = {
    {GPIOA, 1}, {GPIOA, 2}, {GPIOA, 7}, /* REF_CLK, MDIO, CRS_DV */
    {GPIOC, 1}, {GPIOC, 4}, {GPIOC, 5}, /* MDC, RXD0, RXD1 */
    {GPIOB, 13}, {GPIOG, 11}, {GPIOG, 13} /* TXD1, TX_EN, TXD0 */
  };
  for (uint32_t i = 0; i < sizeof(rmii_pins) / sizeof(rmii_pins[0]); ++i) {
    GPIO_TypeDef *gpio = rmii_pins[i].port;
    uint32_t pin = rmii_pins[i].pin;
    uint32_t shift = pin * 2U;
    uint32_t af_shift = (pin % 8U) * 4U;
    gpio->OTYPER &= ~(1U << pin);
    gpio->PUPDR &= ~(3U << shift);
    gpio->OSPEEDR = (gpio->OSPEEDR & ~(3U << shift)) | (3U << shift);
    gpio->AFR[pin / 8U] = (gpio->AFR[pin / 8U] & ~(15U << af_shift)) | (11U << af_shift);
    gpio->MODER = (gpio->MODER & ~(3U << shift)) | (2U << shift);
  }
  SYSCFG->PMCR = (SYSCFG->PMCR & ~SYSCFG_PMCR_EPIS_SEL_Msk) | SYSCFG_PMCR_EPIS_SEL_2;
  (void)SYSCFG->PMCR;
  RCC->AHB1ENR |= RCC_AHB1ENR_ETH1MACEN | RCC_AHB1ENR_ETH1TXEN | RCC_AHB1ENR_ETH1RXEN;
  (void)RCC->AHB1ENR;
  RCC->AHB1RSTR |= RCC_AHB1RSTR_ETH1MACRST;
  __DSB();
  RCC->AHB1RSTR &= ~RCC_AHB1RSTR_ETH1MACRST;
  __DSB();
  NVIC_DisableIRQ(ETH_IRQn);
  NVIC_ClearPendingIRQ(ETH_IRQn);
  eqos_udelay(10);

  /* 3. MAC/DMA software reset. External PA1 REF_CLK must be running. */
  printf("MAC version: %08lX\r\n", (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_VERSION));
  uint32_t dma_mode = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_MODE);
  dma_mode |= EQOS_DMA_MODE_SWR;
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_MODE, dma_mode);
  uint32_t elapsed = 0;
  while ((AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_MODE) & EQOS_DMA_MODE_SWR) != 0U) {
    if (elapsed++ >= 100000U) {
      printf("DMA reset timeout: check PA1 50 MHz reference clock\r\n");
      Error_Handler();
    }
    eqos_udelay(1);
  }
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_US_TIC_COUNTER,
      eth.cfg.csr_clk_hz / 1000000U - 1U);
  uint32_t hz = eth.cfg.csr_clk_hz;
  if      (hz >= 20000000U && hz < 35000000U)  eth.mdio_cr = EQOS_MDIO_CR_20_35;
  else if (hz >= 35000000U && hz < 60000000U)  eth.mdio_cr = EQOS_MDIO_CR_35_60;
  else if (hz >= 60000000U && hz < 100000000U) eth.mdio_cr = EQOS_MDIO_CR_60_100;
  else if (hz >= 100000000U && hz < 150000000U) eth.mdio_cr = EQOS_MDIO_CR_100_150;
  else if (hz >= 150000000U && hz < 250000000U) eth.mdio_cr = EQOS_MDIO_CR_150_250;
  else if (hz >= 250000000U && hz < 300000000U) eth.mdio_cr = EQOS_MDIO_CR_250_300;
  else {
    printf("Unsupported MDIO clock: %lu Hz\r\n", (unsigned long)hz);
    Error_Handler();
  }

  /* 4. Original LAN8742 driver via direct-register MDIO callbacks. */
//  int32_t status = LAN8742_RegisterBusIO(&LAN8742, &LAN8742_IOCtx);
//  if (status == LAN8742_STATUS_OK)
//    status = LAN8742_Init(&LAN8742);
//  if (status == LAN8742_STATUS_OK)
//    status = LAN8742_StartAutoNego(&LAN8742);
//  if (status != LAN8742_STATUS_OK) {
//    printf("LAN8742 init failed: %ld\r\n", (long)status);
//    Error_Handler();
//  }
//  eth.phy_addr = (int)LAN8742.DevAddr;
//  uint32_t start = HAL_GetTick();
//  while (!eth.link_up) {
//    status = LAN8742_GetLinkState(&LAN8742);
//    switch (status) {
//    case LAN8742_STATUS_100MBITS_FULLDUPLEX:
//    case LAN8742_STATUS_100MBITS_HALFDUPLEX:
//      eth.speed = EQOS_SPEED_100;
//      eth.duplex_full = (status == LAN8742_STATUS_100MBITS_FULLDUPLEX);
//      eth.link_up = true;
//      break;
//    case LAN8742_STATUS_10MBITS_FULLDUPLEX:
//    case LAN8742_STATUS_10MBITS_HALFDUPLEX:
//      eth.speed = EQOS_SPEED_10;
//      eth.duplex_full = (status == LAN8742_STATUS_10MBITS_FULLDUPLEX);
//      eth.link_up = true;
//      break;
//    case LAN8742_STATUS_LINK_DOWN:
//    case LAN8742_STATUS_AUTONEGO_NOTDONE:
//      break;
//    default:
//      printf("LAN8742 link read failed: %ld\r\n", (long)status);
//      Error_Handler();
//    }
//    if (!eth.link_up) {
//      if ((uint32_t)(HAL_GetTick() - start) >= eth.cfg.aneg_timeout_ms) {
//        printf("LAN8742 link timeout: %ld\r\n", (long)status);
//        Error_Handler();
//      }
//      HAL_Delay(10);
//    }
//  }
  printf("PHY %d: %d Mbps %s duplex\r\n", eth.phy_addr, (int)eth.speed,
         eth.duplex_full ? "full" : "half");

  /* 5. Read each MAC/MTL control once, combine locally, then write once. */



  /***************************************************** DMA Config *****************************************************/
  /* Config dma tx control */
  uint32_t tx_dma_control = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL);
  // tx_dma_control &= ~((EQOS_DMA_CH_TX_TXPBL_MASK << EQOS_DMA_CH_TX_TXPBL_SHIFT) |
  //                     EQOS_DMA_CH_TX_ST); //Transmit Programmable Burst Length
  tx_dma_control |= EQOS_DMA_CH_TX_OSP //Operate on Second Packet 
                  | (8U << EQOS_DMA_CH_TX_TXPBL_SHIFT) // Transmit Programmable Burst Length
                  | EQOS_DMA_CH_TX_ST; //Start or Stop Transmission Command
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL, tx_dma_control);

  /* Config dma rx control */
  uint32_t rx_dma_control = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RX_CONTROL);
  rx_dma_control &= ~((EQOS_DMA_CH_RX_RBSZ_MASK << EQOS_DMA_CH_RX_RBSZ_SHIFT) | 
                      (EQOS_DMA_CH_RX_RXPBL_MASK << EQOS_DMA_CH_RX_RXPBL_SHIFT) |
                      EQOS_DMA_CH_RX_SR);
  rx_dma_control |= (EQOS_BUF_SIZE << EQOS_DMA_CH_RX_RBSZ_SHIFT)  //Receive Buffer size
                  | (8U << EQOS_DMA_CH_RX_RXPBL_SHIFT) //Receive Programmable Burst Length
                  | EQOS_DMA_CH_RX_SR; //Start or Stop Receive

  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RX_CONTROL,rx_dma_control);
  /* Config dma control */
  uint32_t dma_control = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_CONTROL);
   
  // dma_control &= ~(EQOS_DMA_CH_CTRL_DSL_MASK << EQOS_DMA_CH_CTRL_DSL_SHIFT);
  dma_control |= EQOS_DMA_CH_CTRL_PBLX8; /* DSL=0, contiguous descriptors. */ //Descriptor Skip Length 
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_CONTROL,dma_control);
  /* Bus mode*/
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_SYSBUS_MODE,
                        EQOS_DMA_SYSBUS_AHB_AAL //Address-Aligned Beats
                        | EQOS_DMA_SYSBUS_AHB_FB); //  Fixed Burst Length
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_INTERRUPT_ENABLE,0U); //Disable Interrupt

  /* 7. Initialize TX/RX descriptors and register both rings with DMA. */
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
  /* Preserve the supplied driver's last-descriptor RX tail convention. */
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_TAIL_PTR,
      (uint32_t)(uintptr_t)&rx_desc[EQOS_RX_DESC_COUNT - 1U]);//Rx Tail Pointer
  eth.tx_head = 0;
  eth.rx_head = 0;


  

  /***************************************************** End DMA Config *****************************************************/


  /***************************************************** MTL Config *****************************************************/
  /* Config MTL Tx Control*/  
  uint32_t mtl_tx_control = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE);
    mtl_tx_control |= EQOS_MTL_TXQ_TSF;
  if (!eth.duplex_full)
    mtl_tx_control |= EQOS_MTL_TXQ_FTQ;
  AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE, mtl_tx_control);


  /* Config MTL Rx Control*/
  uint32_t mtl_rx_control = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE);
  mtl_rx_control |= EQOS_MTL_RXQ_RSF | EQOS_MTL_RXQ_DISTCPEF; //Receive Queue Store and Forward || Disable Dropping of TCP/IP Checksum Error Packets
  AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, mtl_rx_control);
  /***************************************************** End MTL Config *****************************************************/




  /***************************************************** MAC Config *****************************************************/
  
  /* MAC Config*/
  // uint32_t mac_control = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);
  // printf("MAC Control: %x\n",mac_control);

  // mac_control &= ~(EQOS_MAC_CFG_FES // Config speed
  //                 | EQOS_MAC_CFG_DM  //duplex mode
  //                 | EQOS_MAC_CFG_GPSLCE //Giant Packet Size Limit Control Enable
  //                 | EQOS_MAC_CFG_WD // Watchdog Disable
  //                 | EQOS_MAC_CFG_JD //Jabber Disable
  //                 | EQOS_MAC_CFG_JE // Jumbo Packet Enable
  //                 |EQOS_MAC_CFG_LM //Loopback Mode
  //                 | EQOS_MAC_CFG_TE // Transmitter Enable
  //                 | EQOS_MAC_CFG_RE); //Receiver Enable

  /* MAC Packet Filter*/
  uint32_t mac_filter = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
  if (eth.cfg.promiscuous)
    mac_filter |= EQOS_MAC_PKT_FILTER_PR; //Promiscuous Mode
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, mac_filter);
  /* MAC Address*/
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS0_HIGH,
      ((uint32_t)eth.cfg.mac_addr[5] << 8) | eth.cfg.mac_addr[4]);
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS0_LOW,
      ((uint32_t)eth.cfg.mac_addr[3] << 24) |
      ((uint32_t)eth.cfg.mac_addr[2] << 16) |
      ((uint32_t)eth.cfg.mac_addr[1] << 8) | eth.cfg.mac_addr[0]);

  uint32_t mac_control=0;
  mac_control |= EQOS_MAC_CFG_CST | EQOS_MAC_CFG_ACS; //RC stripping for Type packets || Automatic Pad or CRC Stripping
  if (eth.speed == EQOS_SPEED_100)
    mac_control |= EQOS_MAC_CFG_FES;
  if (eth.duplex_full)
    mac_control |= EQOS_MAC_CFG_DM;
  if (eth.cfg.loopback)
    mac_control |= EQOS_MAC_CFG_LM;
  // AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, mac_control);
  //   mac_control = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);
  printf("MAC Control: %x\n",mac_control);
  /* Start Tx And Rx*/
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION,
                      mac_control //Apply mac config
                      | EQOS_MAC_CFG_TE //Start transmit
                      | EQOS_MAC_CFG_RE); //Start receive


  /***************************************************** End MAC Config *****************************************************/
  eth.started = true;
  const uint8_t dest_mac[6] = {0x6C, 0x1F, 0xF7, 0xCA, 0x2B, 0xFD};
//  const uint8_t payload[] = "Hello world hehe lmao";
  const uint8_t payload[] = {0x17, 0x17, 0x17, 0x69, 0x29, 0x36,0x98,0x99};
  memcpy(frame, dest_mac, 6);
  memcpy(frame + 6, eth.cfg.mac_addr, 6);
  frame[12] = 0x88;
  frame[13] = 0xB5;
  memcpy(frame + 14, payload, sizeof(payload));
  frame_len = 14U + sizeof(payload);
  send_requested = 1U; /* Startup send uses the same inline TX path as USER. */
  printf("Ethernet ready\r\n");

  while (1) {
    /* 9. TX: copy, publish descriptor, advance tail, wait for completion. */
    if (send_requested != 0U) {
      send_requested = 0U;
      uint32_t idx = eth.tx_head;
      struct eqos_desc *tx = &tx_desc[idx];
      int tx_status = EQOS_OK;
      if (frame_len == 0U || frame_len > EQOS_BUF_SIZE) {
        tx_status = EQOS_EINVAL;
      } else if ((tx->des3 & EQOS_TDES3_OWN) != 0U) {
        tx_status = EQOS_EAGAIN; /* Never overwrite a buffer still owned by DMA. */
      } else {
        memcpy(tx_buf[idx], frame, frame_len);
        tx->des0 = (uint32_t)(uintptr_t)tx_buf[idx];
        tx->des1 = 0;
        tx->des2 = frame_len & EQOS_TDES2_B1L_MASK;
        __DSB();
        tx->des3 = EQOS_TDES3_OWN | EQOS_TDES3_FD | EQOS_TDES3_LD |
                   (frame_len & EQOS_TDES3_FL_MASK); // Mark descriptor as FD, LD , frame length and set OWN bit
        __DSB();
        eth.tx_head = (idx + 1U) & (EQOS_TX_DESC_COUNT - 1U);
        AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR,
            (uint32_t)(uintptr_t)&tx_desc[eth.tx_head]); // Update Tail pointer
        elapsed = 0;
        uint32_t tx_result;
        do {
          tx_result = tx->des3;
          if ((tx_result & EQOS_TDES3_OWN) == 0U)
            break;
          eqos_udelay(1);
        } while (++elapsed < 1000000U);
        if ((tx_result & EQOS_TDES3_OWN) != 0U) {
          ++eth.stats.tx_timeouts;
          tx_status = EQOS_ETIMEDOUT;
        } else if ((tx_result & EQOS_TDES3_WB_ES) != 0U) {
          ++eth.stats.tx_errors;
          tx_status = EQOS_EIO;
        } else {
          ++eth.stats.tx_packets;
        }
      }
      printf("TX: %s (%d), %lu bytes before padding/FCS\r\n",
             tx_status == EQOS_OK ? "complete" : "failed", tx_status,
             (unsigned long)frame_len);
      if (tx_status != EQOS_OK)
        printf("DMACSR=%08lX TXDESC3=%08lX TXTAIL=%08lX\r\n",
               (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS), (unsigned long)tx->des3,
               (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR));
    }

    /* 10. RX: poll ownership, validate, print, then rearm the same descriptor. */
    uint32_t idx = eth.rx_head;
    struct eqos_desc *rx = &rx_desc[idx];
    uint32_t rx_status = rx->des3;
    if ((rx_status & EQOS_RDES3_OWN) == 0U) {
      __DSB(); /* Acquire packet bytes after DMA returns ownership. */
      uint32_t len = rx_status & EQOS_RDES3_WB_PL_MASK;
      if ((rx_status & EQOS_RDES3_WB_CTXT) != 0U) {
        /* Timestamp context is not a packet; recycle without printing. */
      } else if ((rx_status & EQOS_RDES3_WB_ES) != 0U) {
        ++eth.stats.rx_errors;
        printf("RX descriptor error: %08lX\r\n", (unsigned long)rx_status);
      } else if ((rx_status & (EQOS_RDES3_WB_FD | EQOS_RDES3_WB_LD)) !=
                 (EQOS_RDES3_WB_FD | EQOS_RDES3_WB_LD) || len == 0U || len > EQOS_BUF_SIZE) {
        ++eth.stats.rx_dropped;
        printf("RX dropped: descriptor=%08lX length=%lu\r\n",
               (unsigned long)rx_status, (unsigned long)len);
      } else {
        ++eth.stats.rx_packets;
        printf("\r\n===== RX packet: %lu bytes @ %p =====\r\n",
               (unsigned long)len, (void *)rx_buf[idx]);
        for (uint32_t i = 0; i < len; ++i) {
          printf("%02X ", (unsigned int)rx_buf[idx][i]);
          if ((i + 1U) % 16U == 0U)
            printf("\r\n");
        }
        printf("\r\n");
      }
      /* Both successful and discarded descriptors return to DMA exactly once. */
      rx->des0 = (uint32_t)(uintptr_t)rx_buf[idx];
      rx->des1 = 0;
      rx->des2 = 0;
      __DSB();
      rx->des3 = EQOS_RDES3_OWN | EQOS_RDES3_BUF1V;
      __DSB();
      AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_TAIL_PTR,
          (uint32_t)(uintptr_t)rx);
      eth.rx_head = (idx + 1U) & (EQOS_RX_DESC_COUNT - 1U);
    }
  }
}

/* LAN8742 requires callable IO callbacks; these cannot be local main statements. */
static int32_t ETH_PHY_INTERFACE_Init(void) { return 0; }
static int32_t ETH_PHY_INTERFACE_DeInit(void) { return 0; }
static int32_t ETH_PHY_INTERFACE_GetTick(void) { return (int32_t)HAL_GetTick(); }

static int32_t ETH_PHY_INTERFACE_WaitIdle(void)
{
  uint32_t start = HAL_GetTick();
  while ((AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_MDIO_ADDRESS) & EQOS_MDIO_ADDR_GB) != 0U) {
    if ((uint32_t)(HAL_GetTick() - start) >= 1000U)
      return -1;
  }
  return 0;
}

static int32_t ETH_PHY_INTERFACE_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal)
{
  if (pRegVal == NULL || DevAddr > 31U || RegAddr > 31U || ETH_PHY_INTERFACE_WaitIdle() < 0)
    return -1;
  uint32_t mdio_address = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_MDIO_ADDRESS);
  mdio_address &= EQOS_MDIO_ADDR_SKAP;
  mdio_address |= (DevAddr << EQOS_MDIO_ADDR_PA_SHIFT) |
                  (RegAddr << EQOS_MDIO_ADDR_RDA_SHIFT) |
                  (eth.mdio_cr << EQOS_MDIO_ADDR_CR_SHIFT) |
                  (EQOS_MDIO_ADDR_GOC_READ << EQOS_MDIO_ADDR_GOC_SHIFT) | EQOS_MDIO_ADDR_GB;
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_MDIO_ADDRESS, mdio_address);
  if (ETH_PHY_INTERFACE_WaitIdle() < 0)
    return -1;
  *pRegVal = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_MDIO_DATA) & EQOS_MDIO_DATA_GD_MASK;
  return 0;
}

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
                  (eth.mdio_cr << EQOS_MDIO_ADDR_CR_SHIFT) |
                  (EQOS_MDIO_ADDR_GOC_WRITE << EQOS_MDIO_ADDR_GOC_SHIFT) | EQOS_MDIO_ADDR_GB;
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_MDIO_ADDRESS, mdio_address);
  return ETH_PHY_INTERFACE_WaitIdle();
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 28;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 5;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

void BSP_PB_Callback(Button_TypeDef Button)
{
  if (Button == BUTTON_USER)
    send_requested = 1U; /* No blocking TX or UART output in the interrupt. */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
