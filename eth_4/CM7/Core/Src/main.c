
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
/*
 * EQoS bring-up map (STM32H755 instance of Synopsys DesignWare Ethernet QoS):
 *
 *   TX: application frame -> RAM TX buffer -> DMA -> MTL TX FIFO -> MAC -> RMII
 *   RX: RMII -> MAC -> MTL RX FIFO -> DMA -> RAM RX buffer -> application
 *
 * All AX_BASE_* calls take an ABSOLUTE block base and a RELATIVE byte offset:
 *   MAC: 0x40028000; MTL queue 0 window: 0x40028D00; DMA: 0x40029000.
 * Example: DMA_CH0_TX_CONTROL = 0x104 -> actual address 0x40029104.
 * The MTL base here selects queue 0, not the global MTL window at offset 0xC00.
 * eqos_hw.h includes generic EQoS definitions; not every generic feature is
 * implemented by the H755. This file uses its single 10/100-Mbit MAC and AHB DMA.
 *
 * CURRENT EXPERIMENTAL SEQUENCE -- these comments describe the code as saved:
 * - cfg.loopback is true; the LAN8742 initialization/link wait below is disabled.
 * - DMA ST/SR are set BEFORE descriptor initialization/list registration below.
 *   The usual bring-up order is reset -> configure while stopped -> prepare
 *   buffers/rings -> publish ring addresses -> start DMA/MAC. If early DMA
 *   status errors occur, inspect this order; 
 *
 */
COM_InitTypeDef BspCOMInit;
static struct eqos_dev eth;
static lan8742_Object_t LAN8742;
/* eth records software state/statistics; it is not a hardware register overlay.
 * The EXTI callback sets this flag and main consumes it. volatile makes the ISR
 * update observable, but this is a one-bit request latch, not a counted queue. */
static volatile uint8_t send_requested;
static uint8_t frame[114];
static uint32_t frame_len;

/*
 * Normal descriptors occupy four 32-bit words (16 bytes). The SAME words have
 * different meanings in DMA read format (CPU submits work) and DMA write-back
 * format (hardware reports completion); never interpret RX status as an address.
 *
 * This single-buffer, non-TSO application submits:
 *   TX DES0 = buffer-1 address; DES1 = 0 (no second buffer);
 *      DES2 = B1L, buffer byte count; DES3 = OWN | FD | LD | total frame length.
 *   RX DES0 = buffer-1 address; DES1/DES2 = 0 (unused);
 *      DES3 = OWN | BUF1V, making that buffer available for DMA reception.
 * After completion DES3 carries OWN=0 and write-back flags/length/error status.
 *
 * OWN=1: DMA owns the descriptor and associated buffer; CPU must not rewrite it.
 * OWN=0: CPU can inspect completion and later rebuild/reuse the descriptor.
 * volatile prevents compiler-cached status reads; it is NOT cache coherency.
 */
struct eqos_desc {
  volatile uint32_t des0, des1, des2, des3;
};
/* Rings: 4 TX and 8 RX entries by current eqos.h defaults. The linker places
 * .eqos_nocache in DMA-accessible RAM_D1, with NOLOAD: initialize every word
 * explicitly below. A section NAME does not program the MPU/cache attributes.
 * Buffers are also in RAM_D1 (.bss), not DTCM, which this ETH DMA cannot access.
 * 32-byte alignment is the M7 cache-line alignment; descriptors remain packed
 * at a 16-byte stride inside each ring. DSL must therefore remain zero. */
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
#error "This inline sequence requires STM32H755 with uncached DMA storage"
#endif

void SystemClock_Config(void);
static int32_t ETH_PHY_INTERFACE_Init(void);
static int32_t ETH_PHY_INTERFACE_DeInit(void);
static int32_t ETH_PHY_INTERFACE_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal);
static int32_t ETH_PHY_INTERFACE_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal);
static int32_t ETH_PHY_INTERFACE_GetTick(void);
static int32_t ETH_PHY_INTERFACE_WaitIdle(void);
/* Original LAN8742 component calls these MDIO/tick functions through its bus
 * context. They are presently unused by main because step 4 is commented out;
 * keep them available for a later external-PHY bring-up experiment. */
static lan8742_IOCtx_t LAN8742_IOCtx = {
  ETH_PHY_INTERFACE_Init, ETH_PHY_INTERFACE_DeInit,
  ETH_PHY_INTERFACE_WriteReg, ETH_PHY_INTERFACE_ReadReg, ETH_PHY_INTERFACE_GetTick
};

int main(void)
{
  /* 1. Existing system clock, UART and button. */
  /* Keep SysTick running: LAN8742 reset/link timeouts and the MDIO busy helper
   * use HAL_GetTick(). UART is initialized before EQoS so failures are visible.
   * SystemClock_Config currently selects 224 MHz CPU/HCLK from the 64 MHz HSI. */
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

  /* csr_clk_hz is the MAC register/CSR clock used for MDC division and the
   * microsecond tick register; it is NOT the external 50 MHz RMII reference.
   * PR=true requests promiscuous reception, useful with the test's peer MAC.
   * LM=true requests MAC-internal loopback later when MACCR is written.
   * max_speed/aneg_timeout_ms do not take effect while the PHY sequence is
   * disabled; no code in this initializer directly programs the LAN8742. */
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
  /* DMA and CPU share RAM without clean/invalidate operations in this example.
   * Refuse D-cache-on execution rather than polling stale descriptor OWN bits
   * or sending stale packet bytes. __DSB() alone cannot clean dirty cache lines. */
  if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
    printf("Keep D-cache disabled for this DMA bring-up\r\n");
    Error_Handler();
  }

  /* 2. DWT delay clock, Ethernet GPIO AF11, RMII selection and RCC reset. */
  /* TRCENA enables access to the DWT cycle counter; LAR unlocks DWT on this core.
   * CYCCNT then counts CPU cycles for eqos_udelay(). Its conversion depends on
   * SystemCoreClock being up to date. Debug halts make software timeouts longer
   * in wall-clock time; the timeout loops are bring-up bounds, not hard timing. */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55U;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN |
                  RCC_AHB4ENR_GPIOCEN | RCC_AHB4ENR_GPIOGEN;
  RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
  (void)RCC->AHB4ENR;
  (void)RCC->APB4ENR;
  /* Clock-enable readbacks above allow the RCC writes to take effect before
   * accessing port registers. No PHY reset GPIO is configured in this sequence.
   * PA1 must receive the board's RMII clock even when testing the MAC locally.
   * All pins below use AF11, push-pull, no internal pulls and very-high speed;
   * OSPEEDR controls output slew rate, not the Ethernet line-rate setting. */
  const struct { GPIO_TypeDef *port; uint32_t pin; } rmii_pins[] = {
    {GPIOA, 1}, {GPIOA, 2}, {GPIOA, 7}, /* REF_CLK, MDIO, CRS_DV */
    {GPIOC, 1}, {GPIOC, 4}, {GPIOC, 5}, /* MDC, RXD0, RXD1 */
    {GPIOB, 13}, {GPIOG, 11}, {GPIOG, 13} /* TXD1, TX_EN, TXD0 */
  };
  for (uint32_t i = 0; i < sizeof(rmii_pins) / sizeof(rmii_pins[0]); ++i) {
    GPIO_TypeDef *gpio = rmii_pins[i].port;
    uint32_t pin = rmii_pins[i].pin;
    /* MODER/PUPDR/OSPEEDR allocate two bits per pin; AFR allocates four.
     * pin/8 selects AFRL or AFRH and pin%8 selects the nibble within that word.
     * Set the alternate-function selection before switching MODER to AF mode. */
    uint32_t shift = pin * 2U;
    uint32_t af_shift = (pin % 8U) * 4U;
    gpio->OTYPER &= ~(1U << pin);
    gpio->PUPDR &= ~(3U << shift);
    gpio->OSPEEDR = (gpio->OSPEEDR & ~(3U << shift)) | (3U << shift);
    gpio->AFR[pin / 8U] = (gpio->AFR[pin / 8U] & ~(15U << af_shift)) | (11U << af_shift);
    gpio->MODER = (gpio->MODER & ~(3U << shift)) | (2U << shift);
  }
  /* EPIS_SEL=100b selects RMII on H755. MAC/TX/RX clocks are all required.
   * RCC peripheral reset below is separate from the DMA software reset (SWR)
   * and from a PHY reset over MDIO. Changing one does not replace the others. */
  SYSCFG->PMCR = (SYSCFG->PMCR & ~SYSCFG_PMCR_EPIS_SEL_Msk) | SYSCFG_PMCR_EPIS_SEL_2;
  (void)SYSCFG->PMCR;
  RCC->AHB1ENR |= RCC_AHB1ENR_ETH1MACEN | RCC_AHB1ENR_ETH1TXEN | RCC_AHB1ENR_ETH1RXEN;
  (void)RCC->AHB1ENR;
  RCC->AHB1RSTR |= RCC_AHB1RSTR_ETH1MACRST;
  __DSB();
  RCC->AHB1RSTR &= ~RCC_AHB1RSTR_ETH1MACRST;
  __DSB();
  /* This program polls descriptors in main: leave ETH IRQ delivery disabled.
   * Later DMACIER=0 disables DMA interrupt sources as well. Status bits may
   * still latch even when interrupt delivery is disabled; inspect DMACSR. */
  NVIC_DisableIRQ(ETH_IRQn);
  NVIC_ClearPendingIRQ(ETH_IRQn);
  eqos_udelay(10);

  /* 3. MAC/DMA software reset. External PA1 REF_CLK must be running. */
  printf("MAC version: %08lX\r\n", (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_VERSION));
  /* SWR requests a reset of the EQoS DMA/controller state and self-clears when
   * complete. The loop must reread the volatile register; a saved snapshot
   * cannot tell us when hardware has finished. Missing interface clocks can
   * leave SWR set. Do not program rings/MAC settings until this wait succeeds. */
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
  /* Program clocks-per-microsecond minus one for the MAC's internal tick.
   * With this project's HCLK=224 MHz the value is 223. This is unrelated to
   * the IEEE 1588 timestamp increment and does not enable PTP timestamping. */
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_US_TIC_COUNTER,
      eth.cfg.csr_clk_hz / 1000000U - 1U);
  /* CR selects an MDC divider for Clause-22 PHY management transactions.
   * The MDIO callbacks insert eth.mdio_cr into MACMDIOAR for every operation;
   * the numeric CR code is an encoding, not the divisor itself.
   * With 224 MHz HCLK this branch selects the 150-250 MHz clock range.
   * Speed/duplex negotiation is separate from this management-bus clock. */
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

  /* PHY bring-up outline (currently DISABLED by // on each statement):
   * RegisterBusIO binds the callbacks; Init scans the PHY address, requests
   * software reset and waits for settling; StartAutoNego enables negotiation.
   * GetLinkState returns named status codes, not a speed in Mbit/s. Only the
   * four 10/100 half/full-duplex cases assign a usable eth.speed/duplex_full.
   * LINK_DOWN and AUTONEGO_NOTDONE require waiting, not starting the MAC.
   * The component's own reset/settle delays precede the separate 5-second wait.
   * MAC loopback does not automatically restore this disabled PHY sequence. */
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

  /* 5. Configure the DMA, then MTL, then MAC blocks (current source order).
   * Register snapshots below are local read/modify/write values. Combine bits
   * before writing to avoid redundant bus accesses, but continue fresh reads
   * for hardware status polling. OR-ing a multi-bit field is not equivalent
   * to replacing it unless the old field bits are known to be zero. */



  /***************************************************** DMA Config *****************************************************/
  /* DMA channel-0 TX control (DMA base + 0x104):
   * OSP allows processing the next packet while the previous packet completes.
   * TXPBL is a programmable burst-length field, in bus beats, not bytes.
   * With TXPBL=8 and PBLX8=1 below, the configured limit is 8*8=64 beats
   * (256 bytes on a 32-bit bus); actual transfers can be shorter/split.
   * ST starts TX DMA. Here it is set before the TX ring base/length are written.
   * The field-clear code is commented out: OR-ing 8 relies on the reset value
   * for TXPBL. A repeated configuration without reset could retain other bits. */
  uint32_t tx_dma_control = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL);
  // tx_dma_control &= ~((EQOS_DMA_CH_TX_TXPBL_MASK << EQOS_DMA_CH_TX_TXPBL_SHIFT) |
  //                     EQOS_DMA_CH_TX_ST); //Transmit Programmable Burst Length
  tx_dma_control |= EQOS_DMA_CH_TX_OSP //Operate on Second Packet 
                  | (8U << EQOS_DMA_CH_TX_TXPBL_SHIFT) // Transmit Programmable Burst Length
                  | EQOS_DMA_CH_TX_ST; //Start or Stop Transmission Command
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL, tx_dma_control);

  /* DMA channel-0 RX control (DMA base + 0x108):
   * Clear RBSZ/RXPBL/SR locally, then supply the intended field values.
   * RBSZ=1600 is the capacity of EACH DMA RX buffer; the register field is
   * shifted by one bit, so do not write the unshifted size to the register.
   * RXPBL=8 is scaled by PBLX8 just like TXPBL. SR starts receive DMA.
   * The final value here includes SR=1 before the RX descriptors are registered;
   * inspect DMA status if it attempts to fetch from an uninitialized list. */
  uint32_t rx_dma_control = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RX_CONTROL);
  rx_dma_control &= ~((EQOS_DMA_CH_RX_RBSZ_MASK << EQOS_DMA_CH_RX_RBSZ_SHIFT) | 
                      (EQOS_DMA_CH_RX_RXPBL_MASK << EQOS_DMA_CH_RX_RXPBL_SHIFT) |
                      EQOS_DMA_CH_RX_SR);
  rx_dma_control |= (EQOS_BUF_SIZE << EQOS_DMA_CH_RX_RBSZ_SHIFT)  //Receive Buffer size
                  | (8U << EQOS_DMA_CH_RX_RXPBL_SHIFT) //Receive Programmable Burst Length
                  | EQOS_DMA_CH_RX_SR; //Start or Stop Receive

  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RX_CONTROL,rx_dma_control);
  /* DMA channel control (DMA base + 0x100):
   * PBLX8 multiplies both burst-length fields by eight. It does NOT select DSL.
   * DSL is the number of bus-width units skipped after each 16-byte descriptor.
   * This packed array needs DSL=0. Since the explicit DSL clear is disabled,
   * the current code relies on the successful SWR reset leaving DSL at zero. */
  uint32_t dma_control = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_CONTROL);
   
  // dma_control &= ~(EQOS_DMA_CH_CTRL_DSL_MASK << EQOS_DMA_CH_CTRL_DSL_SHIFT);
  dma_control |= EQOS_DMA_CH_CTRL_PBLX8; /* Scale PBL by 8; DSL is unchanged here. */
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_CONTROL,dma_control);
  /* AHB system-bus mode: AAL requests address-aligned bursts; FB selects fixed
   * burst behavior. H755 uses the AHB variant: generic EQoS AXI BLEN/EAME fields
   * must not be substituted here. This whole-register write intentionally sets
   * the configured bits and leaves the other writable controls clear.
   * DMACIER=0 selects polling; it does not clear DMACSR or stop DMA transfers. */
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_SYSBUS_MODE,
                        EQOS_DMA_SYSBUS_AHB_AAL //Address-Aligned Beats
                        | EQOS_DMA_SYSBUS_AHB_FB); //  Fixed Burst Length
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_INTERRUPT_ENABLE,0U); //Disable Interrupt

  /* 7. Initialize TX/RX descriptors and register both rings with DMA. */
  /* TX initially has no work: OWN=0 in every descriptor. RX starts pre-posted:
   * each descriptor advertises a dedicated empty buffer with BUF1V and OWN.
   * CPU writes address/unused words first, executes DSB, then publishes OWN.
   * A second barrier after the loop completes publication before MMIO ring
   * registration. The CPU must not modify an RX buffer after giving OWN away.
   * Debug sequence note: ST/SR were already set above; these writes have not
   * stopped the engines, so the usual stopped-DMA setup ordering is not met. */
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
  /* List-address registers hold absolute RAM addresses, NOT EQoS block offsets.
   * Ring length is encoded N-1: current TX=4 -> 3, RX=8 -> 7.
   * DMA walks the array using the 16-byte descriptor plus DSL skip; it does not
   * use the software eth.tx_head/rx_head variables to find the next descriptor.
   * Tail registers are published work boundaries/resume notifications, not
   * payload pointers. OWN, ring bounds and the tail must agree. */
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
  /* Preserve the supplied driver's last-descriptor RX tail convention:
   * initial tail=&rx_desc[N-1]; recycling later writes the returned descriptor's
   * address. Do not silently substitute a one-past-end convention from another
   * EQoS driver. On a stall, inspect OWN, current RX descriptor and tail together;
   * exercise more than N packets to verify wraparound on the actual hardware. */
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_TAIL_PTR,
      (uint32_t)(uintptr_t)&rx_desc[EQOS_RX_DESC_COUNT - 1U]);//Rx Tail Pointer
  eth.tx_head = 0;
  eth.rx_head = 0;


  

  /***************************************************** End DMA Config *****************************************************/


  /***************************************************** MTL Config *****************************************************/
  /* MTL TX queue 0 (MTL queue base + 0x00):
   * TSF selects store-and-forward: accumulate the full frame before passing it
   * to the MAC, reducing underrun risk compared with threshold-based forwarding.
   * FTQ requests a FIFO flush and is a command that hardware clears; it is not
   * a persistent operating-mode flag. Do not reuse a stale FTQ=1 snapshot later.
   * The condition currently takes the half-duplex branch because PHY assignment
   * is disabled and eth.duplex_full remains false. No TX is submitted yet. */
  uint32_t mtl_tx_control = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE);
    mtl_tx_control |= EQOS_MTL_TXQ_TSF;
  if (!eth.duplex_full)
    mtl_tx_control |= EQOS_MTL_TXQ_FTQ;
  AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_OPERATION_MODE, mtl_tx_control);


  /* MTL RX queue 0 (queue base + 0x30):
   * RSF selects store-and-forward so a frame is collected before DMA delivery.
   * DISTCPEF disables dropping based on TCP/IP checksum errors; it does not
   * disable Ethernet FCS validation or make descriptor errors acceptable.
   * The test uses a private EtherType/raw payload and no IP checksum offload.
   * Queue-size/scheduling controls of other EQoS integrations are not configured. */
  uint32_t mtl_rx_control = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE);
  mtl_rx_control |= EQOS_MTL_RXQ_RSF | EQOS_MTL_RXQ_DISTCPEF; //Receive Queue Store and Forward || Disable Dropping of TCP/IP Checksum Error Packets
  AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, mtl_rx_control);
  /***************************************************** End MTL Config *****************************************************/




  /***************************************************** MAC Config *****************************************************/
  
  /* MAC configuration is assembled below and committed with TE/RE in one write.
   * The old read/clear sequence is left commented out. The ACTIVE path starts
   * mac_control at zero, so unlisted writable features are intentionally clear;
   * it does not preserve previously programmed MAC control bits. */
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

  /* MAC packet filter PR accepts frames irrespective of destination-address
   * matching, useful because the test destination differs from this MAC address.
   * Promiscuous mode is not a bypass for every integrity/error check.
   * Current code only ORs PR when true; false does not explicitly clear an
   * already-set PR bit, so this branch relies on reset state for false. */
  uint32_t mac_filter = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER);
  if (eth.cfg.promiscuous)
    mac_filter |= EQOS_MAC_PKT_FILTER_PR; //Promiscuous Mode
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, mac_filter);
  /* Program perfect-filter address slot 0, byte by byte into its register words.
   * For 00:80:E1:00:00:00 this gives HIGH=0x00000000 and LOW=0x00E18000.
   * The packing is register byte order, not a reversal of bytes in the frame.
   * Source MAC bytes are also explicitly copied into the test Ethernet header. */
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS0_HIGH,
      ((uint32_t)eth.cfg.mac_addr[5] << 8) | eth.cfg.mac_addr[4]);
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_ADDRESS0_LOW,
      ((uint32_t)eth.cfg.mac_addr[3] << 24) |
      ((uint32_t)eth.cfg.mac_addr[2] << 16) |
      ((uint32_t)eth.cfg.mac_addr[1] << 8) | eth.cfg.mac_addr[0]);

  /* MACCR fields used by this experiment:
   * CST/ACS control receive FCS/padding stripping according to frame Type/Length.
   * They do not describe TX CRC insertion. With EtherType 0x88B5 there is no
   * length field telling the MAC that the custom payload is only eight bytes;
   * RX data can therefore contain trailing minimum-frame padding.
   * FES=1 selects 100 Mbit/s; FES=0 selects 10 Mbit/s on this 10/100 MAC.
   * DM=1 selects full duplex; DM=0 half duplex. The current zero eth.speed and
   * false duplex_full leave both bits clear; these are not negotiated values.
   * LM loops TX into MAC RX internally. It does not prove the cable/PHY path.
   * WD=0/JD=0 keep watchdog/jabber protection active; JE=0 leaves jumbo disabled.
   * No checksum-offload, timestamping or pause-flow-control setup is added here. */
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
  /* TE and RE enable the MAC transmitter and receiver. These are independent
   * of DMA ST/SR: both sides of the pipeline must be active for frame movement.
   * Ring registration and MTL configuration have completed by this point.
   * eth.started below is only a software flag; it does not read hardware state. */
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION,
                      mac_control //Apply mac config
                      | EQOS_MAC_CFG_TE //Start transmit
                      | EQOS_MAC_CFG_RE); //Start receive


  /***************************************************** End MAC Config *****************************************************/
  eth.started = true;
  /* Raw Ethernet-II frame: 6 destination bytes + 6 source bytes + 2 EtherType
   * bytes + payload. 88 B5 is stored in network byte order; no IP header follows.
   * Current payload has eight literal bytes, so frame_len=22 (no trailing NUL).
   * DES3 CRC/padding controls remain zero in the TX path: hardware supplies
   * minimum-frame padding and the FCS; neither is included in frame_len.
   * In MAC loopback the RX length need not equal these 22 submitted bytes. */
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
      /* tx_head names the next software slot to submit. The DMA has its own
       * current-descriptor pointer. After a timeout the old slot may still be
       * owned by DMA; the OWN check below prevents unsafe reuse on ring wrap. */
      struct eqos_desc *tx = &tx_desc[idx];
      int tx_status = EQOS_OK;
      if (frame_len == 0U || frame_len > EQOS_BUF_SIZE) {
        tx_status = EQOS_EINVAL;
      } else if ((tx->des3 & EQOS_TDES3_OWN) != 0U) {
        tx_status = EQOS_EAGAIN; /* Never overwrite a buffer still owned by DMA. */
      } else {
        /* Copy into persistent, DMA-accessible RAM before publishing ownership.
         * DES0 points to the packet, DES1 is unused, and DES2.B1L equals the
         * number of valid bytes in that buffer. No interrupt-on-completion bit
         * is requested; completion is observed by polling DES3. */
        memcpy(tx_buf[idx], frame, frame_len);
        tx->des0 = (uint32_t)(uintptr_t)tx_buf[idx];
        tx->des1 = 0;
        tx->des2 = frame_len & EQOS_TDES2_B1L_MASK;
        __DSB();
        /* One packet fits in one descriptor: FD and LD are both set.
         * FL is the total submitted frame length excluding generated FCS/pad.
         * CTXT=0 means normal descriptor; CIC=0 disables IP checksum insertion;
         * CPC=0 requests CRC and pad insertion in the H755 TX descriptor format.
         * Publish OWN LAST. First DSB orders data/address writes before OWN;
         * second DSB orders OWN before the MMIO tail update that wakes DMA. */
        tx->des3 = EQOS_TDES3_OWN | EQOS_TDES3_FD | EQOS_TDES3_LD |
                   (frame_len & EQOS_TDES3_FL_MASK); // Mark descriptor as FD, LD , frame length and set OWN bit
        __DSB();
        /* Advance modulo the power-of-two ring size. TX tail points at the
         * NEXT descriptor: this submits the preceding owned descriptor.
         * Tail advancement is necessary even if the descriptor is already OWN=1
         * when the engine is suspended at its previous work boundary. */
        eth.tx_head = (idx + 1U) & (EQOS_TX_DESC_COUNT - 1U);
        AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR,
            (uint32_t)(uintptr_t)&tx_desc[eth.tx_head]); // Update Tail pointer
        /* Wait for THIS submitted descriptor, not eth.tx_head (which advanced).
         * Approximately one million 1-us delays plus loop overhead is the bound.
         * Do not print inside this poll: UART latency would distort completion.
         * OWN clears before software interprets the TX write-back error summary. */
        elapsed = 0;
        uint32_t tx_result;
        do {
          tx_result = tx->des3;
          if ((tx_result & EQOS_TDES3_OWN) == 0U)
            break;
          eqos_udelay(1);
        } while (++elapsed < 1000000U);
        /* Timeout does not cancel DMA or give ownership back to the CPU.
         * ES means hardware completed with an error; otherwise count local TX
         * completion. Success is not a peer acknowledgement or cable test,
         * especially with MAC-internal loopback enabled. */
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
    /* Read a fresh volatile status for the current RX head on each main-loop
     * iteration. OWN=1 means no CPU-owned packet at this slot yet; no release
     * or head advancement is allowed in that case. Poll the ring in order. */
    uint32_t rx_status = rx->des3;
    if ((rx_status & EQOS_RDES3_OWN) == 0U) {
      __DSB(); /* Acquire packet bytes after DMA returns ownership. */
      /* OWN=0 switches interpretation to RX write-back format.
       * CTXT describes metadata rather than a normal data packet.
       * ES summarizes receive errors. FD|LD must both be present because this
       * application cannot assemble frames spanning multiple descriptors.
       * PL is the hardware-reported length after applicable MAC stripping;
       * check it before using it as the limit for a memory read/UART dump. */
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
        /* Keep this buffer CPU-owned while printing. Returning OWN early would
         * let DMA overwrite bytes while printf is still reading them.
         * A full frame dump at 115200 baud is slow; other descriptors can fill
         * and RX can suspend/drop under traffic. This path favors inspection,
         * not sustained line-rate throughput. */
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
      /* Rebuild ALL four words: DMA write-back may have overwritten address
       * fields with status. Returning only OWN would resubmit invalid contents.
       * Rearm even rejected/error/context descriptors so one bad frame does not
       * permanently consume a ring entry. Do not access rx_buf[idx] after OWN.
       * DSB then tail write makes the rearmed descriptor visible before DMA is
       * notified; software advances rx_head only after returning this slot. */
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

/* Serialize Clause-22 management commands through the MAC's MDIO engine.
 * GB (busy) is set by software to launch a command and clears on completion.
 * The HAL tick timeout requires SysTick interrupts; it cannot expire if called
 * with interrupts disabled. These callbacks are intended for main context,
 * and main currently does not call them while PHY initialization is disabled. */
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
                  (eth.mdio_cr << EQOS_MDIO_ADDR_CR_SHIFT) |
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
                  (eth.mdio_cr << EQOS_MDIO_ADDR_CR_SHIFT) |
                  (EQOS_MDIO_ADDR_GOC_WRITE << EQOS_MDIO_ADDR_GOC_SHIFT) | EQOS_MDIO_ADDR_GB;
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_MDIO_ADDRESS, mdio_address);
  return ETH_PHY_INTERFACE_WaitIdle();
}

/* Clock arithmetic for the CURRENT constants: HSI 64 MHz / PLLM 4 = 16 MHz;
 * VCO = 16 * PLLN 28 = 448 MHz; PLLP 2 -> SYSCLK 224 MHz.
 * SYSCLKDivider and AHBCLKDivider are both /1, hence CPU/HCLK 224 MHz.
 * MAC CSR/MDC selection follows HCLK; the PHY supplies a separate RMII 50 MHz.
 * Recheck the MDIO CR range and DWT delays if these divisors are changed. */
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

/* EXTI only latches a request; the blocking descriptor poll and UART output
 * execute in main. Multiple edges before the next poll can coalesce, and no
 * debounce/counting is implemented. A button press is not an Ethernet IRQ. */
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
