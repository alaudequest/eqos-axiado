#include <stdio.h>
#include <stdint.h>
#include "main.h"
#include "../../../baremetal/eqos.h"
#include "../../../baremetal/eqos_hw.h"
#include "eth_config_test_case.h"
#include "eth_functions_utils.h"
#include "eth_function_stm32.h"

/* Test files: outside the build folders, compiled through this file only */
#include "../../../../0_test_cases/L1_MK_RMII_VLAN_FILTER.c"
#include "../../../../0_test_cases/L1_RMII_DROP_NON_TCP_UDP.c"
#include "../../../../0_test_cases/L1_RMII_MAC_RX_CRC_ERROR.c"
#include "../../../../0_test_cases/MAC_CTRL_SARC.c"
#include "../../../../0_test_cases/MAC_CTRL_GPSLCE.c"
#include "../../../../0_test_cases/MAC_PFR_HASH_UC.c"
#include "../../../../0_test_cases/MAC_WTR_WATCHDOG.c"
/* Both TXFC files define txfc_tx_buf / txfc_rx_frame: build only one of them */
#define ETH_TXFC_TIMESTAMP  1
#if ETH_TXFC_TIMESTAMP
#include "../../../../0_test_cases/MAC_Q0_TX_FLOW_CTRL_PAUSE_TIMESTAMP.c"
#else
#include "../../../../0_test_cases/MAC_Q0_TX_FLOW_CTRL_PAUSE.c"
#endif
#include "../../../../0_test_cases/MAC_VTR_DVLAN_OUTER.c"
#include "../../../../0_test_cases/MAC_VTR_DVLAN_INNER.c"
#include "../../../../0_test_cases/MAC_VTR_DVLAN_STRIP.c"
#include "../../../../0_test_cases/MAC_ISR_MMC_TX.c"
#include "../../../../0_test_cases/MTL_RXQ0_OVERFLOW.c"
#include "../../../../0_test_cases/DMA_CH0_PKT_TXRX.c"
#include "../../../../0_test_cases/MAC_VIR_QINQ.c"

/* Tests in this build: one line each (ID of eth_config_test_case.h, name for the banner, entry) */
static const struct {
	int id;
	const char *name;
	int (*run)(struct eqos_dev *eth, struct eqos_desc *tx_desc, struct eqos_desc *rx_desc,
	           uint8_t rx_buf[][EQOS_BUF_SIZE]);
} eth_tests[] = {
	{TEST_RMII_VLAN_FILTER,         "RMII_VLAN_FILTER",         eth_test_vlan_filter},
	{TEST_RMII_DROP_NON_TCP_UDP,    "RMII_DROP_NON_TCP_UDP",    eth_test_drop_non_tcp_udp},
	{TEST_RMII_MAC_RX_CRC_ERROR,    "RMII_MAC_RX_CRC_ERROR",    eth_test_mac_rx_crc_error},
	{TEST_MAC_CTRL_SARC,            "MAC_CTRL_SARC",            eth_test_mac_ctrl_sarc},
	{TEST_MAC_CTRL_GPSLCE,          "MAC_CTRL_GPSLCE",          eth_test_mac_ctrl_gpslce},
	{TEST_MAC_PFR_HASH_UC,          "MAC_PFR_HASH_UC",          eth_test_mac_pfr_hash_uc},
	{TEST_MAC_WTR_WATCHDOG,         "MAC_WTR_WATCHDOG",         eth_test_mac_wtr_watchdog},
#if ETH_TXFC_TIMESTAMP
	{TEST_MAC_TXFC_PAUSE_TIMESTAMP, "MAC_TXFC_PAUSE_TIMESTAMP", eth_test_mac_txfc_pause_timestamp},
#else
	{TEST_MAC_TXFC_PAUSE,           "MAC_TXFC_PAUSE",           eth_test_mac_txfc_pause},
#endif
	{TEST_MAC_VTR_DVLAN_OUTER,      "MAC_VTR_DVLAN_OUTER",      eth_test_mac_vtr_dvlan_outer},
	{TEST_MAC_VTR_DVLAN_INNER,      "MAC_VTR_DVLAN_INNER",      eth_test_mac_vtr_dvlan_inner},
	{TEST_MAC_VTR_DVLAN_STRIP,      "MAC_VTR_DVLAN_STRIP",      eth_test_mac_vtr_dvlan_strip},
	{TEST_MAC_ISR_MMC_TX,           "MAC_ISR_MMC_TX",           eth_test_mac_isr_mmc_tx},
	{TEST_MTL_RXQ0_OVERFLOW,        "MTL_RXQ0_OVERFLOW",        eth_test_mtl_rxq0_overflow},
	{TEST_DMA_CH0_PKT_TXRX,         "DMA_CH0_PKT_TXRX",         eth_test_dma_ch0_pkt_txrx},
	{TEST_MAC_VIR_QINQ,             "MAC_VIR_QINQ",             eth_test_mac_vir_qinq},
};

/**
 * RMII[mac_idx] MAC loopback test, selected by eth_run_test_ops (TEST_* IDs, list in
 * src/L0_MK_RMII_MAIN_FILE.c):
 *   eth_stm32_init()        STM32: D-cache check, DWT, RMII pins, SYSCFG, ETH clocks + reset
 *   eth_reset_mac()         software state eth, DMA_Mode.SWR, MAC_1US_Tic_Counter, MDIO CR
 *   mac_init_100m()         DMA, rings, MTL, MAC_Address0, MAC_Configuration (DMA stopped)
 *   init_mac_internal_lb()  MAC_Configuration.LM = 1
 *   mac_start()             DMA ST / SR, then MAC TE / RE
 *   eth_tests[i].run()      eth_test_xxx(&eth, tx_desc, rx_desc, rx_buf) = failure count
 * Return 1: PASS, 0: FAIL
 */
uint16_t eth_rmii_mac_lb(uint8_t mac_idx)
{
	const uint32_t count = sizeof(eth_tests) / sizeof(eth_tests[0]);
	uint32_t i = 0;
	int fail = -1;

	while (i < count && eth_tests[i].id != eth_run_test_ops)
		i++;
	if (i == count) {
		printf("Test %d is not in this build\r\n", eth_run_test_ops);
		return 0;
	}

	printf("======== RMII[%d] - START TEST %d %s ========\r\n", mac_idx, eth_run_test_ops, eth_tests[i].name);

	if (eth_stm32_init(mac_idx) == 1 && eth_reset_mac(&eth) == 1) {
		mac_init_100m(&eth);
		init_mac_internal_lb(&eth);
		mac_start(&eth);
		fail = eth_tests[i].run(&eth, tx_desc, rx_desc, rx_buf);
	}

	printf("======== RMII[%d] - END TEST %s (%d failure(s)) ========\r\n", mac_idx, eth_tests[i].name, fail);
	return (fail == 0) ? 1 : 0;
}

/* MAC CSR clock = HCLK (224 MHz): MAC_1US_Tic_Counter and the MDIO CR range */
uint32_t eth_csr_clock_hz(void)
{
	return HAL_RCC_GetHCLKFreq();
}

/**
 * STM32H755 Ethernet bring-up, before the EQoS registers are used (moved from main.c step 2):
 *   SCB->CCR.DC [16]              0       (read) D-cache must be off
 *   CoreDebug->DEMCR.TRCENA [24]  1       DWT on; DWT->CYCCNT counts for eqos_udelay()
 *   RCC_AHB4ENR / RCC_APB4ENR     1       clocks of GPIOA/B/C/G and SYSCFG
 *   GPIOx (9 RMII pins)           AF11    pins connected to the Ethernet MAC
 *   SYSCFG_PMCR.EPIS_SEL [23:21]  100b    PHY interface = RMII
 *   RCC_AHB1ENR.ETH1MACEN [15]    1       ETH clocks (+ ETH1TXEN, ETH1RXEN)
 *   RCC_AHB1RSTR.ETH1MACRST [15]  1 -> 0  reset pulse of the ETH block
 *   NVIC ETH_IRQn                 off     no interrupt, the test polls
 * Return 1: OK, 0: mac_idx not 0 or D-cache is on
 */
uint8_t eth_stm32_init(uint8_t mac_idx)
{
	static const struct { GPIO_TypeDef *port; uint32_t pin; } rmii_pins[] = {
		{GPIOA, 1}, {GPIOA, 2}, {GPIOA, 7},		/* REF_CLK, MDIO, CRS_DV */
		{GPIOC, 1}, {GPIOC, 4}, {GPIOC, 5},		/* MDC, RXD0, RXD1 */
		{GPIOB, 13}, {GPIOG, 11}, {GPIOG, 13}	/* TXD1, TX_EN, TXD0 */
	};

	if (mac_idx != 0U) {
		printf("mac_idx %d: STM32H755 has 1 MAC\r\n", mac_idx);
		return 0;
	}
	if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
		printf("Keep D-cache disabled for this DMA bring-up\r\n");
		return 0;
	}
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->LAR = 0xC5ACCE55U;
	DWT->CYCCNT = 0U;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN |
					RCC_AHB4ENR_GPIOCEN | RCC_AHB4ENR_GPIOGEN;
	RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
	(void)RCC->AHB4ENR;
	(void)RCC->APB4ENR;
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
	printf("MAC version: %08lX\r\n", (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_VERSION));
	return 1;
}
