/* SPDX-License-Identifier: GPL-2.0 */
/* STM32H755 delay and register-map checks. Board setup is inline in main(). */
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
