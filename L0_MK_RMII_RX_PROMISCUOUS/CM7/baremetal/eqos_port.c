/* SPDX-License-Identifier: GPL-2.0 */
/* STM32H755 delay and register-map checks. Board clocks/GPIO/reset and cycle delay. */
#include "stm32h7xx.h"
#include "eqos.h"
#include "eqos_hw.h"



/* Compile-time validation only: runtime Ethernet access uses eqos_hw.h. */
_Static_assert(EQOS_MAC_BASE == ETH_MAC_BASE, "MAC base address");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, MACVR) == EQOS_MAC_BASE + EQOS_MAC_VERSION, "MACVR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, DMAMR) == EQOS_DMA_BASE + EQOS_DMA_MODE, "DMAMR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, MAC1USTCR) == EQOS_MAC_BASE + EQOS_MAC_US_TIC_COUNTER, "MAC1USTCR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, MACCR) == EQOS_MAC_BASE + EQOS_MAC_CONFIGURATION, "MACCR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, MTLTQOMR) == EQOS_MTL_BASE + EQOS_MTL_TXQ0_OPERATION_MODE, "MTLTQOMR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, MTLRQOMR) == EQOS_MTL_BASE + EQOS_MTL_RXQ0_OPERATION_MODE, "MTLRQOMR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, MACPFR) == EQOS_MAC_BASE + EQOS_MAC_PACKET_FILTER, "MACPFR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, MACTFCR) == EQOS_MAC_BASE + EQOS_MAC_Q0_TX_FLOW_CTRL, "MACTFCR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, MACRFCR) == EQOS_MAC_BASE + EQOS_MAC_RX_FLOW_CTRL, "MACRFCR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, MACA0HR) == EQOS_MAC_BASE + EQOS_MAC_ADDRESS0_HIGH, "MACA0HR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, MACA0LR) == EQOS_MAC_BASE + EQOS_MAC_ADDRESS0_LOW, "MACA0LR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, DMACTCR) == EQOS_DMA_BASE + EQOS_DMA_CH0_TX_CONTROL, "DMACTCR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, DMACRCR) == EQOS_DMA_BASE + EQOS_DMA_CH0_RX_CONTROL, "DMACRCR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, DMACCR) == EQOS_DMA_BASE + EQOS_DMA_CH0_CONTROL, "DMACCR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, DMASBMR) == EQOS_DMA_BASE + EQOS_DMA_SYSBUS_MODE, "DMASBMR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, DMACIER) == EQOS_DMA_BASE + EQOS_DMA_CH0_INTERRUPT_ENABLE, "DMACIER offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, DMACTDLAR) == EQOS_DMA_BASE + EQOS_DMA_CH0_TXDESC_LIST_ADDR, "DMACTDLAR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, DMACTDRLR) == EQOS_DMA_BASE + EQOS_DMA_CH0_TXDESC_RING_LEN, "DMACTDRLR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, DMACRDLAR) == EQOS_DMA_BASE + EQOS_DMA_CH0_RXDESC_LIST_ADDR, "DMACRDLAR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, DMACRDRLR) == EQOS_DMA_BASE + EQOS_DMA_CH0_RXDESC_RING_LEN, "DMACRDRLR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, DMACTDTPR) == EQOS_DMA_BASE + EQOS_DMA_CH0_TXDESC_TAIL_PTR, "DMACTDTPR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, DMACRDTPR) == EQOS_DMA_BASE + EQOS_DMA_CH0_RXDESC_TAIL_PTR, "DMACRDTPR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, DMACSR) == EQOS_DMA_BASE + EQOS_DMA_CH0_STATUS, "DMACSR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, MACMDIOAR) == EQOS_MAC_BASE + EQOS_MAC_MDIO_ADDRESS, "MACMDIOAR offset");
_Static_assert(ETH_BASE + offsetof(ETH_TypeDef, MACMDIODR) == EQOS_MAC_BASE + EQOS_MAC_MDIO_DATA, "MACMDIODR offset");
_Static_assert(EQOS_MTL_RXQ_DISTCPEF == ETH_MTLRQOMR_DISTCPEF, "RX checksum filter bit");
_Static_assert(EQOS_DMA_SYSBUS_AHB_FB == ETH_DMASBMR_FB, "AHB fixed burst bit");
_Static_assert(EQOS_DMA_SYSBUS_AHB_AAL == ETH_DMASBMR_AAL, "AHB alignment bit");

void eqos_udelay(uint32_t us)
{
    /* Short chunks keep unsigned cycle subtraction safe across wraparound. */
    uint32_t cycles_per_us = (SystemCoreClock + 999999U) / 1000000U;
    while (us != 0U) {
        uint32_t chunk = us > 1000U ? 1000U : us;
        uint32_t start = DWT->CYCCNT;
        uint32_t cycles = chunk * cycles_per_us;
        while ((uint32_t)(DWT->CYCCNT - start) < cycles) { }
        us -= chunk;
    }
}

void eqos_port_init(void)
{
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

}
