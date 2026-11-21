/* SPDX-License-Identifier: GPL-2.0 */
/* STM32H755 glue for the EQoS driver: [2] DMA-memory precheck, [3] DWT,
 * RMII pins, SYSCFG, RCC and NVIC, the microsecond delay, and compile-time
 * checks of the register map. Declarations: eqos_port.h. */
#include "stm32h7xx.h"
#include "../core/eqos.h"
#include "../core/eqos_hw.h"
#include "eqos_port.h"
#include "../flow/eqos_flow.h"
#include <stdio.h>



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

// Cac buoc con cua stm32_config_eth(), goi theo thu tu nay
static void stm32_enable_dwt(void);                             // bo dem chu ky DWT (cho eqos_udelay)
static void stm32_enable_gpio_clocks(void);                     // clock GPIOA/B/C/G + SYSCFG
static void stm32_config_rmii_pins(const struct board_cfg *c);  // 9 chan RMII -> AF11
static void stm32_enable_eth(void);                             // SYSCFG chon RMII, clock + reset ETH
static void stm32_disable_eth_irq(void);                        // tat ngat ETH (polling)

/* [2] Kiem tra dieu kien bo nho truoc khi cau hinh Ethernet:
 * descriptor/buffer nam o RAM khong cache -> D-cache phai tat.
 * Tieu de buoc [2] va bang CFG duoc in ngay truoc do, o print_config() trong main.c.
 * Tra ve EQOS_EINVAL neu D-cache dang bat (main() se goi Error_Handler()). */
int stm32_precheck(void)
{
  // [FLOW2] Kich thuoc cac vung nho DMA se dung
  FLOW2("[2] Descriptor %lu byte; ring TX %lu x, RX %lu x; buffer %lu byte -> tx_buf %lu B, rx_buf %lu B\r\n",
        (unsigned long)sizeof(struct eqos_desc), (unsigned long)EQOS_TX_DESC_COUNT, (unsigned long)EQOS_RX_DESC_COUNT,
        (unsigned long)EQOS_BUF_SIZE, (unsigned long)(EQOS_TX_DESC_COUNT * EQOS_BUF_SIZE),
        (unsigned long)(EQOS_RX_DESC_COUNT * EQOS_BUF_SIZE));

  /*Keep D-cache disabled for this test*/
  // SCB->CCR (0xE000ED14, thanh ghi cua loi Cortex-M7) bit 16 DC = 1 nghia la D-cache dang bat -> in canh bao, bao loi
  if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
    printf("Keep D-cache disabled for this DMA bring-up\r\n");
    // main() se goi Error_Handler()
    return EQOS_EINVAL;
  }
  // [FLOW] Toi day nghia la D-cache dang tat: CPU va DMA cung nhin RAM that
  FLOW("[2] D-cache TAT (SCB->CCR = 0x%08lX, bit DC = 0) -> CPU va DMA thay cung du lieu RAM\r\n",
       (unsigned long)SCB->CCR);
  return EQOS_OK;
}

/* [3] DWT delay clock, Ethernet GPIO AF11, RMII selection and RCC reset.
 * Thu tu: DWT (de eqos_udelay chay) -> clock GPIO/SYSCFG -> 9 chan RMII ->
 * SYSCFG chon RMII + clock + reset ETH -> tat ngat ETH. */
void stm32_config_eth(const struct board_cfg *c)
{
  // [FLOW2]
  FLOW_STEP("[3]", "DWT, chan RMII (AF11), SYSCFG chon RMII, reset khoi ETH");
  stm32_enable_dwt();          // bo dem chu ky CPU cho eqos_udelay()
  stm32_enable_gpio_clocks();  // clock GPIOA/B/C/G + SYSCFG
  stm32_config_rmii_pins(c);   // 9 chan RMII -> AF11
  stm32_enable_eth();          // SYSCFG chon RMII, bat 3 clock ETH, reset khoi ETH qua RCC
  stm32_disable_eth_irq();     // tat ngat ETH (chuong trinh chay kieu polling)
}

/* CYCCNT then counts CPU cycles for eqos_udelay(). Its conversion depends on
 * SystemCoreClock being up to date. */
static void stm32_enable_dwt(void)
{
  // Bat bit TRCENA: cap nguon cho khoi debug/trace (DWT nam trong khoi nay)
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  // Ghi "mat khau" 0xC5ACCE55 vao Lock Access Register de mo khoa ghi thanh ghi DWT
  DWT->LAR = 0xC5ACCE55U;
  // Xoa bo dem chu ky
  DWT->CYCCNT = 0U;
  // Cho bo dem chay: +1 moi chu ky CPU (224 trieu lan/giay); tu day eqos_udelay() dung duoc
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* Clock cho 4 port GPIO cua chan RMII va cho SYSCFG (can de ghi SYSCFG_PMCR) */
static void stm32_enable_gpio_clocks(void)
{
  // Bat clock GPIOA/B/C/G qua thanh ghi RCC_AHB4ENR (RM0399 tr.492)
  RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN |
                  RCC_AHB4ENR_GPIOCEN | RCC_AHB4ENR_GPIOGEN;
  // Bat clock SYSCFG de ghi duoc PMCR (RM0399 tr.505)
  RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
  // Doc lai (bo ket qua) de chac lenh bat clock da co hieu luc truoc khi dung ngoai vi
  (void)RCC->AHB4ENR;
  (void)RCC->APB4ENR;
  // [FLOW] DWT dang dem: gia tri CYCCNT phai > 0
  FLOW("[3] DWT CYCCNT dang dem (CYCCNT = %lu), clock GPIOA/B/C/G + SYSCFG ON\r\n",
       (unsigned long)DWT->CYCCNT);
}

/* 9 chan RMII trong CFG.board.rmii_pins -> alternate function AF11 (Ethernet) */
static void stm32_config_rmii_pins(const struct board_cfg *c)
{
  // Lap 9 lan qua bang chan: moi phan tu gom port (con tro toi khoi thanh ghi GPIO) va so chan 0..15
  for (uint32_t i = 0; i < RMII_PIN_COUNT; ++i) {
    // Lay port (vi du GPIOA = 0x58020000)
    GPIO_TypeDef *gpio = c->rmii_pins[i].port;
    // Lay so chan
    uint32_t pin = c->rmii_pins[i].pin;
    // Vi tri field 2 bit cua chan nay (MODER, OSPEEDR, PUPDR)
    uint32_t shift = pin * 2U;
    // Vi tri field 4 bit trong AFRL/AFRH (moi thanh ghi chua 8 chan)
    uint32_t af_shift = (pin % 8U) * 4U;
    // Xoa 1 bit -> push-pull
    gpio->OTYPER &= ~(1U << pin);
    // Xoa 2 bit -> khong pull-up/pull-down
    gpio->PUPDR &= ~(3U << shift);
    // Xoa roi ghi 11b -> very high speed (RMII 50 MHz can canh nhanh)
    gpio->OSPEEDR = (gpio->OSPEEDR & ~(3U << shift)) | (3U << shift);
    // AFR[0] = AFRL (chan 0-7), AFR[1] = AFRH (chan 8-15); ghi 11 = AF11 (Ethernet)
    gpio->AFR[pin / 8U] = (gpio->AFR[pin / 8U] & ~(15U << af_shift)) | (11U << af_shift);
    // Ghi 10b -> alternate function. Lam sau cung de chan chi doi che do khi AF da dung
    gpio->MODER = (gpio->MODER & ~(3U << shift)) | (2U << shift);
    // [FLOW] Ten chan: cac port GPIO cach nhau 0x400 nen (dia chi - GPIOA) / 0x400 = 0 cho A, 1 cho B...
    FLOW("[3] Chan P%c%-2lu -> AF11 (MODER=0x%08lX AFR[%lu]=0x%08lX)\r\n",
         (char)('A' + (((uintptr_t)gpio - GPIOA_BASE) / 0x400U)), (unsigned long)pin,
         (unsigned long)gpio->MODER, (unsigned long)(pin / 8U), (unsigned long)gpio->AFR[pin / 8U]);
  }
}

/* EPIS_SEL=100b selects RMII on H755. MAC/TX/RX clocks are all required.
 * RCC peripheral reset below is separate from the DMA software reset (SWR)
 * and from a PHY reset over MDIO. Changing one does not replace the others. */
static void stm32_enable_eth(void)
{
  // Xoa field EPIS (bit 23:21) roi dat bit 23 -> 100b = RMII (RM0399 tr.587)
  SYSCFG->PMCR = (SYSCFG->PMCR & ~SYSCFG_PMCR_EPIS_SEL_Msk) | SYSCFG_PMCR_EPIS_SEL_2;
  // Doc lai de chac lenh ghi da xong
  (void)SYSCFG->PMCR;
  // Bat 3 clock cua Ethernet: MAC (bus/thanh ghi), TX, RX (RM0399 tr.487)
  RCC->AHB1ENR |= RCC_AHB1ENR_ETH1MACEN | RCC_AHB1ENR_ETH1TXEN | RCC_AHB1ENR_ETH1RXEN;
  // Doc lai
  (void)RCC->AHB1ENR;
  // Keo khoi ETH vao trang thai reset (RM0399 tr.463)
  RCC->AHB1RSTR |= RCC_AHB1RSTR_ETH1MACRST;
  // Cho lenh ghi tren hoan tat
  __DSB();
  // Tha reset -> moi thanh ghi ETH ve gia tri mac dinh
  RCC->AHB1RSTR &= ~RCC_AHB1RSTR_ETH1MACRST;
  // Cho lan nua
  __DSB();
  // [FLOW] EPIS phai = 4 (100b = RMII)
  FLOW("[3] SYSCFG_PMCR = 0x%08lX -> EPIS = %lu (4 = RMII)\r\n",
       (unsigned long)SYSCFG->PMCR,
       (unsigned long)((SYSCFG->PMCR & SYSCFG_PMCR_EPIS_SEL_Msk) >> SYSCFG_PMCR_EPIS_SEL_Pos));
  FLOW("[3] RCC_AHB1ENR = 0x%08lX (ETH1MAC/TX/RX clock ON), da reset khoi ETH qua RCC\r\n",
       (unsigned long)RCC->AHB1ENR);
  // [FLOW2] Bit ETH1MACRST phai da ve 0 (khoi ETH khong con bi giu reset)
  FLOW2("[3] RCC_AHB1RSTR = 0x%08lX (ETH1MACRST = %lu, phai = 0)\r\n",
        (unsigned long)RCC->AHB1RSTR,
        (unsigned long)((RCC->AHB1RSTR & RCC_AHB1RSTR_ETH1MACRST) ? 1U : 0U));
}

/* This program polls descriptors in main: leave ETH IRQ delivery disabled.
 * Later DMACIER=0 disables DMA interrupt sources as well. Status bits may
 * still latch even when interrupt delivery is disabled; inspect DMACSR. */
static void stm32_disable_eth_irq(void)
{
  // Tat duong ngat Ethernet o NVIC (bo dieu khien ngat cua loi)
  NVIC_DisableIRQ(ETH_IRQn);
  // Xoa ngat dang cho (neu co tu truoc)
  NVIC_ClearPendingIRQ(ETH_IRQn);
  // Cho 10 us (dung DWT) cho khoi ETH on dinh sau reset (truoc khi buoc 4 cham vao thanh ghi ETH)
  eqos_udelay(10);
  // [FLOW]
  FLOW("[3] Tat ngat ETH (chuong trinh chay kieu polling)\r\n");
}
