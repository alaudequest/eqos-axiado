#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "stm32h7xx_hal.h"	// CMSIS (RCC, GPIO, SYSCFG, SCB, DWT, NVIC), HAL_RCC_GetHCLKFreq
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_function_stm32.h"
#include "eth_filter_utils.h"
#include "eth_burst_utils.h"
#include "eth_ptp_utils.h"
#include "eth_vlan_utils.h"
#include "eth_vlan_rx_utils.h"
#include "eth_sa_utils.h"
#include "eth_addr_utils.h"
#include "eth_istat_utils.h"
#include "eth_dma_order_utils.h"
#include "eth_tso_utils.h"
#include "eth_coe_utils.h"
#include "eth_printf.h"
#include "eth_flow_utils.h"

/*
 * STM32H755 part of the Ethernet test (Nucleo-H755ZI-Q MB1363, PHY LAN8742 in RMII):
 * delays (DWT), UART output (BSP COM1), D-cache check, RMII pins, SYSCFG, ETH clocks and reset.
 * eth_functions.c only uses the EQoS registers: on another SoC only this file changes.
 */

/* Register map check against the CMSIS ETH_TypeDef (compile time only) */
_Static_assert(EQOS_BASE == ETH_BASE, "ETH base address");
_Static_assert(offsetof(ETH_TypeDef, MACCR) == R_MAC_CONFIGURATION, "MAC_Configuration offset");
_Static_assert(offsetof(ETH_TypeDef, MACECR) == R_MAC_EXT_CONFIGURATION, "MAC_Ext_Configuration offset");
_Static_assert(offsetof(ETH_TypeDef, MACPFR) == R_MAC_PACKET_FILTER, "MAC_Packet_Filter offset");
_Static_assert(offsetof(ETH_TypeDef, MACWTR) == R_MAC_WD_JB_TIMEOUT, "MAC_WD_JB_Timeout offset");
_Static_assert(offsetof(ETH_TypeDef, MACHT0R) == R_MAC_HASH_TABLE_REG0, "MAC_Hash_Table_Reg0 offset");
_Static_assert(offsetof(ETH_TypeDef, MACHT1R) == R_MAC_HASH_TABLE_REG1, "MAC_Hash_Table_Reg1 offset");
_Static_assert(offsetof(ETH_TypeDef, MAC1USTCR) == R_MAC_1US_TIC_COUNTER, "MAC_1US_Tic_Counter offset");
_Static_assert(offsetof(ETH_TypeDef, MACVR) == R_MAC_VERSION, "MAC_Version offset");
_Static_assert(offsetof(ETH_TypeDef, MACDR) == R_MAC_DEBUG, "MAC_Debug offset");
_Static_assert(offsetof(ETH_TypeDef, MACHWF0R) == R_MAC_HW_FEATURE0, "MAC_HW_Feature0 offset");
_Static_assert(offsetof(ETH_TypeDef, MACHWF1R) == R_MAC_HW_FEATURE1, "MAC_HW_Feature1 offset");
_Static_assert(offsetof(ETH_TypeDef, MACHWF2R) == R_MAC_HW_FEATURE2, "MAC_HW_Feature2 offset");
_Static_assert(offsetof(ETH_TypeDef, MACMDIOAR) == R_MAC_MDIO_ADDRESS, "MAC_MDIO_Address offset");
_Static_assert(offsetof(ETH_TypeDef, MACMDIODR) == R_MAC_MDIO_DATA, "MAC_MDIO_Data offset");
_Static_assert(offsetof(ETH_TypeDef, MACA0HR) == R_MAC_ADDRESS0_HIGH, "MAC_Address0_High offset");
_Static_assert(offsetof(ETH_TypeDef, MACA0LR) == R_MAC_ADDRESS0_LOW, "MAC_Address0_Low offset");
_Static_assert(offsetof(ETH_TypeDef, MACA1HR) == R_MAC_ADDRESS1_HIGH, "MAC_Address1_High offset");
_Static_assert(offsetof(ETH_TypeDef, MACA1LR) == R_MAC_ADDRESS1_LOW, "MAC_Address1_Low offset");
_Static_assert(offsetof(ETH_TypeDef, MMCCR) == R_MMC_CONTROL, "MMC_CONTROL offset");
_Static_assert(offsetof(ETH_TypeDef, MMCTSCGPR) == MMC_STATS_TX_SINGLE_COL_GOOD, "TX single collision offset");
_Static_assert(offsetof(ETH_TypeDef, MMCTMCGPR) == MMC_STATS_TX_MULTI_COL_GOOD, "TX multiple collision offset");
_Static_assert(offsetof(ETH_TypeDef, MMCTPCGR) == MMC_STATS_TX_PKT_GOOD, "TX packet count good offset");
_Static_assert(offsetof(ETH_TypeDef, MMCRCRCEPR) == MMC_STATS_RX_CRC_ERR, "RX CRC error offset");
_Static_assert(offsetof(ETH_TypeDef, MMCRAEPR) == MMC_STATS_RX_ALIGN_ERR, "RX alignment error offset");
_Static_assert(offsetof(ETH_TypeDef, MMCRUPGR) == MMC_STATS_RX_UNICAST_GOOD, "RX unicast good offset");
_Static_assert(offsetof(ETH_TypeDef, MMCTLPIMSTR) == MMC_STATS_TX_LPI_USEC, "TX LPI usec offset");
_Static_assert(offsetof(ETH_TypeDef, MMCTLPITCR) == MMC_STATS_TX_LPI_TRAN, "TX LPI transition offset");
_Static_assert(offsetof(ETH_TypeDef, MMCRLPIMSTR) == MMC_STATS_RX_LPI_USEC, "RX LPI usec offset");
_Static_assert(offsetof(ETH_TypeDef, MMCRLPITCR) == MMC_STATS_RX_LPI_TRAN, "RX LPI transition offset");
_Static_assert(offsetof(ETH_TypeDef, MTLOMR) == R_MTL_OPERATION_MODE, "MTL_Operation_Mode offset");
_Static_assert(offsetof(ETH_TypeDef, MTLTQOMR) == R_MTL_TXQ0_OPERATION_MODE, "MTL_TxQ0_Operation_Mode offset");
_Static_assert(offsetof(ETH_TypeDef, MTLTQUR) == R_MTL_TXQ0_UNDERFLOW, "MTL_TxQ0_Underflow offset");
_Static_assert(offsetof(ETH_TypeDef, MTLTQDR) == R_MTL_TXQ0_DEBUG, "MTL_TxQ0_Debug offset");
_Static_assert(offsetof(ETH_TypeDef, MTLRQOMR) == R_MTL_RXQ0_OPERATION_MODE, "MTL_RxQ0_Operation_Mode offset");
_Static_assert(offsetof(ETH_TypeDef, MTLRQMPOCR) == R_MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT, "MTL_RxQ0_Missed_Packet_Overflow_Cnt offset");
_Static_assert(offsetof(ETH_TypeDef, MTLRQDR) == R_MTL_RXQ0_DEBUG, "MTL_RxQ0_Debug offset");
_Static_assert(offsetof(ETH_TypeDef, DMAMR) == R_DMA_MODE, "DMA_Mode offset");
_Static_assert(offsetof(ETH_TypeDef, DMASBMR) == R_DMA_SYSBUS_MODE, "DMA_SysBus_Mode offset");
_Static_assert(offsetof(ETH_TypeDef, DMAISR) == R_DMA_INTERRUPT_STATUS, "DMA_Interrupt_Status offset");
_Static_assert(offsetof(ETH_TypeDef, DMADSR) == R_DMA_DEBUG_STATUS0, "DMA_Debug_Status0 offset");
_Static_assert(offsetof(ETH_TypeDef, DMACCR) == R_DMA_CH0_CONTROL, "DMA_CH0_Control offset");
_Static_assert(offsetof(ETH_TypeDef, DMACTCR) == R_DMA_CH0_TX_CONTROL, "DMA_CH0_Tx_Control offset");
_Static_assert(offsetof(ETH_TypeDef, DMACRCR) == R_DMA_CH0_RX_CONTROL, "DMA_CH0_Rx_Control offset");
_Static_assert(offsetof(ETH_TypeDef, DMACTDLAR) == R_DMA_CH0_TXDESC_LIST_ADDRESS, "DMA_CH0_TxDesc_List_Address offset");
_Static_assert(offsetof(ETH_TypeDef, DMACRDLAR) == R_DMA_CH0_RXDESC_LIST_ADDRESS, "DMA_CH0_RxDesc_List_Address offset");
_Static_assert(offsetof(ETH_TypeDef, DMACTDTPR) == R_DMA_CH0_TXDESC_TAIL_POINTER, "DMA_CH0_TxDesc_Tail_Pointer offset");
_Static_assert(offsetof(ETH_TypeDef, DMACRDTPR) == R_DMA_CH0_RXDESC_TAIL_POINTER, "DMA_CH0_RxDesc_Tail_Pointer offset");
_Static_assert(offsetof(ETH_TypeDef, DMACTDRLR) == R_DMA_CH0_TXDESC_RING_LENGTH, "DMA_CH0_TxDesc_Ring_Length offset");
_Static_assert(offsetof(ETH_TypeDef, DMACRDRLR) == R_DMA_CH0_RX_CONTROL2, "DMA_CH0_Rx_Control2 offset");
_Static_assert(offsetof(ETH_TypeDef, DMACIER) == R_DMA_CH0_INTERRUPT_ENABLE, "DMA_CH0_Interrupt_Enable offset");
_Static_assert(offsetof(ETH_TypeDef, DMACCATDR) == R_DMA_CH0_CURRENT_APP_TXDESC, "DMA_CH0_Current_App_TxDesc offset");
_Static_assert(offsetof(ETH_TypeDef, DMACCARDR) == R_DMA_CH0_CURRENT_APP_RXDESC, "DMA_CH0_Current_App_RxDesc offset");
_Static_assert(offsetof(ETH_TypeDef, DMACSR) == R_DMA_CH0_STATUS, "DMA_CH0_Status offset");
_Static_assert(offsetof(ETH_TypeDef, DMACMFCR) == R_DMA_CH0_MISS_FRAME_CNT, "DMA_CH0_Miss_Frame_Cnt offset");
_Static_assert(MAC_CONFIGURATION_RE == ETH_MACCR_RE && MAC_CONFIGURATION_TE == ETH_MACCR_TE && MAC_CONFIGURATION_LM == ETH_MACCR_LM &&
			   MAC_CONFIGURATION_DM == ETH_MACCR_DM && MAC_CONFIGURATION_FES == ETH_MACCR_FES && MAC_CONFIGURATION_JE == ETH_MACCR_JE &&
			   MAC_CONFIGURATION_WD == ETH_MACCR_WD && MAC_CONFIGURATION_ACS == ETH_MACCR_ACS && MAC_CONFIGURATION_CST == ETH_MACCR_CST, "MAC_Configuration bits");
_Static_assert(MAC_PACKET_FILTER_PR == ETH_MACPFR_PR && MAC_PACKET_FILTER_RA == ETH_MACPFR_RA, "MAC_Packet_Filter bits");
_Static_assert(MAC_WD_JB_TIMEOUT_PWE == ETH_MACWTR_PWE, "MAC_WD_JB_Timeout bits");
_Static_assert(MAC_ADDRESS_HIGH_AE == ETH_MACA1HR_AE && MAC_ADDRESS_HIGH_SA == ETH_MACA1HR_SA, "MAC_Address(#i)_High bits");
_Static_assert(MTL_TXQ0_OPERATION_MODE_TSF == ETH_MTLTQOMR_TSF, "MTL_TxQ0_Operation_Mode bits");
_Static_assert(MTL_RXQ0_OPERATION_MODE_RSF == ETH_MTLRQOMR_RSF && MTL_RXQ0_OPERATION_MODE_DIS_TCP_EF == ETH_MTLRQOMR_DISTCPEF, "MTL_RxQ0_Operation_Mode bits");
_Static_assert(DMA_MODE_SWR == ETH_DMAMR_SWR, "DMA_Mode bits");
_Static_assert(DMA_SYSBUS_MODE_FB == ETH_DMASBMR_FB && DMA_SYSBUS_MODE_AAL == ETH_DMASBMR_AAL, "DMA_SysBus_Mode bits");
_Static_assert(DMA_CH0_CONTROL_PBLX8 == ETH_DMACCR_8PBL, "DMA_CH0_Control bits");
_Static_assert(DMA_CH0_TX_CONTROL_ST == ETH_DMACTCR_ST && DMA_CH0_TX_CONTROL_OSF == ETH_DMACTCR_OSP, "DMA_CH0_Tx_Control bits");
_Static_assert(DMA_CH0_RX_CONTROL_SR == ETH_DMACRCR_SR, "DMA_CH0_Rx_Control bits");

/* RMII pins of the Nucleo-H755ZI-Q (schematic MB1363 p.6), all AF11 (Ethernet) */
static const struct {
	GPIO_TypeDef *port;
	uint32_t pin;
	const char *name;
} rmii_pins[] = {
	{GPIOA, 1, "REF_CLK"}, {GPIOA, 2, "MDIO"}, {GPIOA, 7, "CRS_DV"},
	{GPIOC, 1, "MDC"}, {GPIOC, 4, "RXD0"}, {GPIOC, 5, "RXD1"},
	{GPIOB, 13, "TXD1"}, {GPIOG, 11, "TX_EN"}, {GPIOG, 13, "TXD0"},
};

/* Delay in us with the CPU cycle counter DWT->CYCCNT.
 * Short chunks keep the unsigned cycle subtraction safe across wraparound. */
void udelay(uint32_t us)
{
	uint32_t cycles_per_us = (SystemCoreClock + 999999U) / 1000000U;

	if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U) {
		eth_stm32_enable_dwt();
	}
	while (us != 0U) {
		uint32_t chunk = us > 1000U ? 1000U : us;
		uint32_t start = DWT->CYCCNT;
		uint32_t cycles = chunk * cycles_per_us;
		while ((uint32_t)(DWT->CYCCNT - start) < cycles) { }
		us -= chunk;
	}
}
void mdelay(uint32_t ms)
{
	for (uint32_t j = 0; j < ms; ++j) {
		udelay(1000);
	}
}
/* CSR clock of the MAC = HCLK (224 MHz): MAC_1US_Tic_Counter and the MDIO clock range use it */
uint32_t eth_csr_clock_hz(void)
{
	return HAL_RCC_GetHCLKFreq();
}

/* ms since boot (HAL SysTick): FLOW step titles */
uint32_t eth_get_ms(void)
{
	return HAL_GetTick();
}

/* UART output: stdout -> BSP COM1 (ST-LINK virtual COM port, 115200-8N1).
 * Same stdout as printf, so eth_printf and printf lines stay in order. */
void uart_putc(char c)
{
	putchar(c);
}
/* Send what is still in the stdout buffer (a line without '\n', e.g. the ".") */
void uart_flush(void)
{
	fflush(stdout);
}

/**
 * RMII[mac_idx] MAC loopback test, selected by eth_run_test_ops:
 * 0: traffic test (eth_mac_internal_lb), 1: MAC address filter test (eth_mac_filter_lb),
 * 2: DMA burst test, DMA_SysBus_Mode.FB / AAL (eth_mac_burst_lb)
 * 3: PTP timestamp test, MAC_Timestamp_Control / TDES2.TTSE / RX context descriptor + one-step OSTC context (eth_mac_ptp_lb)
 * 4: VLAN insertion test, TX context descriptor / TDES2.VTIR / MAC_VLAN_Incl + bad context CDE (eth_mac_vlan_lb)
 * 5: QinQ test, context with outer + inner tag / MAC_VLAN_Incl.CSVL / MAC_Inner_VLAN_Incl / IVTIR (eth_mac_qinq_lb)
 * 6: TSO test, MSS context descriptor / TDES3.TSE / DMA_CH0_Tx_Control.TSE / DMA_CH0_Control.MSS (eth_mac_tso_lb)
 
 
  
Mode    New case  What is checked                                                                  Expected on board
4 VLAN  F         deliberately wrong context: CTXT + FD + LD (RM0399 p.3060 lists it as an error)  ctx TDES3 write-back has bit 23 CDE = 1, OWN = 0; DMA_CH0_Status CDE [13] = 1, AIS [14] = 1. Result 6/6
5 QinQ  C         MAC_Inner_VLAN_Incl.VLP = 0, context IVTIR = insert                                          68 bytes, 88 A8 07 D0 81 00 00 64 same as A but decided by the descriptor
5 QinQ  D         sent frame already has 2 tags (S 07D0 + C E0C8), IVTIR = replace                 68 bytes, inner tag changed to 81 00 00 64
5 QinQ  E         same frame, IVTIR = remove                                                       64 bytes, 88 A8 07 D0 followed by EtherType 88 B5. Result 5/5
6 TSO   D         MSS = 1000 written to DMA_CH0_Control.MSS instead of context, after B/C stored 1460   3 frames of 1054 bytes, "last write wins". Result 4/4
3 PTP   one-step  context OSTC + TCMSSV before a PTP Sync frame (0x88F7); MAC_Timestamp_Control in Master mode   bytes 48..57 of received frame (originTimestamp, sent as 0) hold system time, seconds match that frame's own TX timestamp



 * 7: checksum offload test, TDES3.CIC / MAC_Configuration.IPC / RDES1 IPHE IPCE PT (eth_mac_coe_lb)
 * 8: VLAN filter test, MAC_VLAN_Tag VL ETV VTIM VTHM ERSVLM ERIVLT DOVLTC / MAC_Packet_Filter.VTFE (eth_mac_vlan_filter_lb)
 * 9: VLAN strip test, MAC_VLAN_Tag EVLS EIVLS / RDES0 OVT IVT / RDES2.VF (eth_mac_vlan_strip_lb)
 * 10: SA insertion / replacement + CRC-pad control, TDES3 SAIC CPC / MAC_Configuration.SARC / MTL_RxQ0_Operation_Mode FUP FEP (eth_mac_sa_cpc_lb)
 * 11: MAC address filter, MAC_Packet_Filter PR = 0 / MAC_Address1..3_High/Low / MAC_Hash_Table_Reg0/Reg1 / RDES2 MADRM HF (eth_mac_addr_filter_lb)
 * 12: interrupt status with nothing enabled, DMA_CH0_Status / DMA_Interrupt_Status / MTL_Q0_Interrupt_Control_Status / MAC_Interrupt_Status, IOC, RBU, overflow, RWT, FBE (eth_mac_int_status_lb)
 * Return 1: PASS, 0: FAIL
 */
uint16_t eth_rmii_mac_lb(uint8_t mac_idx) {
    uint16_t test_result = 0;

	ETH_PRINTF_L1("======== RMII[%d] - START TEST ========\n", mac_idx);

	//Init STM32: D-cache check, RMII pins, ETH clocks and reset
	if (eth_stm32_init(mac_idx) == 1) {
		// Run Eth MAC loopback
		if (eth_run_test_ops == 1) {
			test_result = eth_mac_filter_lb(mac_idx);
		} else if (eth_run_test_ops == 2) {
			test_result = eth_mac_burst_lb(mac_idx);
		} else if (eth_run_test_ops == 3) {
			test_result = eth_mac_ptp_lb(mac_idx);
		} else if (eth_run_test_ops == 4) {
			test_result = eth_mac_vlan_lb(mac_idx);
		} else if (eth_run_test_ops == 5) {
			test_result = eth_mac_qinq_lb(mac_idx);
		} else if (eth_run_test_ops == 6) {
			test_result = eth_mac_tso_lb(mac_idx);
		} else if (eth_run_test_ops == 7) {
			test_result = eth_mac_coe_lb(mac_idx);
		} else if (eth_run_test_ops == 8) {
			test_result = eth_mac_vlan_filter_lb(mac_idx);
		} else if (eth_run_test_ops == 9) {
			test_result = eth_mac_vlan_strip_lb(mac_idx);
		} else if (eth_run_test_ops == 10) {
			test_result = eth_mac_sa_cpc_lb(mac_idx);
		} else if (eth_run_test_ops == 11) {
			test_result = eth_mac_addr_filter_lb(mac_idx);
		} else if (eth_run_test_ops == 12) {
			test_result = eth_mac_int_status_lb(mac_idx);
		} else if (eth_run_test_ops == 13) {
			test_result = eth_mac_dma_order_lb(mac_idx);
		} else {
			test_result = eth_mac_internal_lb(mac_idx);
		}
	}

	ETH_PRINTF_L1("======== RMII[%d] - END TEST ========\n", mac_idx);

	return test_result;
}

/**
 * STM32H755 Ethernet bring-up, before the EQoS registers are used:
 * [2] D-cache check, [3] DWT for udelay(), RMII pins, SYSCFG = RMII, ETH clocks + RCC reset, ETH IRQ off.
 *
 *   Function                      Register.bit [pos]            Value   Function of the bit
 *   ----------------------------- ----------------------------- ------- ------------------------------
 *   eth_stm32_check_dcache()      SCB->CCR.DC [16]              0       (read) D-cache must be off
 *   eth_stm32_enable_dwt()        CoreDebug->DEMCR.TRCENA [24]  1       turn on the DWT block
 *                                 DWT->CTRL.CYCCNTENA [0]       1       start the CPU cycle counter
 *   eth_stm32_config_rmii_pins()  RCC_AHB4ENR.GPIOxEN           1       clocks of GPIOA/B/C/G
 *                                 RCC_APB4ENR.SYSCFGEN [1]      1       clock of SYSCFG
 *                                 GPIOx_MODER, AFR (9 pins)     AF11    pins connected to the Ethernet MAC
 *   eth_stm32_enable_eth()        SYSCFG_PMCR.EPIS_SEL [23:21]  100b    PHY interface = RMII
 *                                 RCC_AHB1ENR.ETH1MACEN [15]    1       ETH clocks (+ ETH1TXEN, ETH1RXEN)
 *                                 RCC_AHB1RSTR.ETH1MACRST [15]  1 -> 0  reset pulse of the ETH block
 *   eth_stm32_disable_eth_irq()   NVIC: ETH_IRQn (61)           off     no interrupt, the test polls
 * Return 1: OK, 0: FAIL
 */
uint8_t eth_stm32_init(uint8_t mac_idx)
{
	if (mac_idx >= MAX_MAC_CNT) {
		eth_printf("mac_idx %d: STM32H755 has %d MAC\n", mac_idx, MAX_MAC_CNT);
		return 0;
	}
	FLOW_STEP("[2]", "DMA memory check: D-cache must be OFF");
	FLOW2("[2] Descriptor %d bytes; ring TX %d, RX %d; buffer %d bytes -> tx_buf %d B, rx_buf %d B\n",
		  (int)sizeof(struct eth_desc), MAC_100M_TX_DESC_CNT, MAC_100M_RX_DESC_CNT, MAC_100M_BUF_SIZE,
		  MAC_100M_TX_DESC_CNT * MAC_100M_BUF_SIZE, MAC_100M_RX_DESC_CNT * MAC_100M_BUF_SIZE);
	if (eth_stm32_check_dcache() == 0) {
		return 0;
	}
	FLOW_STEP("[3]", "DWT, RMII pins (AF11), SYSCFG = RMII, ETH clocks and reset");
	eth_stm32_enable_dwt();
	eth_stm32_config_rmii_pins();
	eth_stm32_enable_eth();
	eth_stm32_disable_eth_irq();
	return 1;
}
/**
 * Descriptors and buffers are in normal RAM (RAM_D1) with no cache maintenance:
 * the D-cache must be off (SCB->CCR.DC, Cortex-M7). Return 1: off (OK), 0: on
 *
 *   Register (PM0253)      Bit [pos]            Value  Function
 *   ---------------------- -------------------- ------ ------------------------------
 *   SCB->CCR               DC [16]              0      (read only here) D-cache off: CPU and DMA
 *                                                      see the same RAM
 */
uint8_t eth_stm32_check_dcache(void)
{
	if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
		eth_printf("D-cache is ON: keep it disabled for this DMA test\n");
		return 0;
	}
	FLOW("[2] D-cache OFF (SCB->CCR = 0x%08lX, bit DC = 0) -> CPU and DMA see the same RAM\n",
		 (unsigned long)SCB->CCR);
	return 1;
}
/**
 * CPU cycle counter for udelay(): DWT->CYCCNT counts SystemCoreClock cycles (Cortex-M7 debug)
 *
 *   Register               Bit [pos]            Value       Function
 *   ---------------------- -------------------- ----------- ------------------------------
 *   CoreDebug->DEMCR       TRCENA [24]          1           power the debug/trace block (DWT)
 *   DWT->LAR               all                  0xC5ACCE55  unlock the DWT registers
 *   DWT->CYCCNT            all                  0           count from 0
 *   DWT->CTRL              CYCCNTENA [0]        1           start counting
 */
void eth_stm32_enable_dwt(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;	// power the debug/trace block (DWT)
	DWT->LAR = 0xC5ACCE55U;							// unlock the DWT registers
	DWT->CYCCNT = 0U;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;			// start counting
	FLOW("[3] DWT CYCCNT counting (CYCCNT = %lu)\n", (unsigned long)DWT->CYCCNT);
}
/**
 * GPIOA/B/C/G clocks (RCC_AHB4ENR p.492) and SYSCFG clock (RCC_APB4ENR p.505), then each
 * RMII pin: push-pull, no pull, very high speed, AF11 (GPIO, RM0399 chapter 12)
 *
 *   Register               Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   RCC_AHB4ENR p.492      GPIOAEN [0]          1       clock of GPIOA (and GPIOB [1], GPIOC [2], GPIOG [6])
 *   RCC_APB4ENR p.505      SYSCFGEN [1]         1       clock of SYSCFG (used by eth_stm32_enable_eth)
 *   for each pin n of rmii_pins[] (PA1 PA2 PA7 PB13 PC1 PC4 PC5 PG11 PG13):
 *   GPIOx_OTYPER           OTn [n]              0       push-pull
 *   GPIOx_PUPDR            PUPDn [2n+1:2n]      00      no pull-up / pull-down
 *   GPIOx_OSPEEDR          OSPEEDn [2n+1:2n]    11      very high speed (50 MHz REF_CLK)
 *   GPIOx_AFRL / AFRH      AFSELn (4 bits)      1011    AF11 = Ethernet (AFRL: pin 0-7, AFRH: 8-15)
 *   GPIOx_MODER            MODEn [2n+1:2n]      10      alternate function (written last)
 */
void eth_stm32_config_rmii_pins(void)
{
	RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIOCEN | RCC_AHB4ENR_GPIOGEN;
	RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
	(void)RCC->AHB4ENR;	// read back: clocks are on before the GPIO writes
	(void)RCC->APB4ENR;
	FLOW("[3] Clock GPIOA/B/C/G + SYSCFG ON\n");

	for (uint32_t i = 0; i < sizeof(rmii_pins) / sizeof(rmii_pins[0]); i++) {
		GPIO_TypeDef *gpio = rmii_pins[i].port;
		uint32_t pin = rmii_pins[i].pin;
		uint32_t shift = pin * 2U;				// 2-bit field: MODER, OSPEEDR, PUPDR
		uint32_t af_shift = (pin % 8U) * 4U;	// 4-bit field in AFRL (pin 0-7) / AFRH (pin 8-15)

		gpio->OTYPER &= ~(1U << pin);										// push-pull
		gpio->PUPDR &= ~(3U << shift);										// no pull-up / pull-down
		gpio->OSPEEDR = (gpio->OSPEEDR & ~(3U << shift)) | (3U << shift);	// very high speed (50 MHz RMII)
		gpio->AFR[pin / 8U] = (gpio->AFR[pin / 8U] & ~(15U << af_shift)) | (11U << af_shift);	// AF11
		gpio->MODER = (gpio->MODER & ~(3U << shift)) | (2U << shift);		// alternate function, set last
		// port letter: GPIO ports are 0x400 apart, (address - GPIOA) / 0x400 = 0 for A, 1 for B ...
		FLOW("[3] Pin P%c%-2lu %-7s -> AF11 (MODER=0x%08lX AFR[%lu]=0x%08lX)\n",
			 (char)('A' + (((uintptr_t)gpio - GPIOA_BASE) / 0x400U)), (unsigned long)pin, rmii_pins[i].name,
			 (unsigned long)gpio->MODER, (unsigned long)(pin / 8U), (unsigned long)gpio->AFR[pin / 8U]);
	}
}
/**
 * Select RMII, turn on the ETH clocks, then a reset pulse of the ETH block with the RCC.
 * This RCC reset is not the EQoS software reset (DMA_Mode.SWR, eth_reset_mac) nor a PHY reset.
 *
 *   Register               Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   SYSCFG_PMCR p.586      EPIS_SEL [23:21]     100b    Ethernet PHY interface = RMII (000b = MII)
 *   RCC_AHB1ENR p.487      ETH1MACEN [15]       1       clock of the MAC (registers and DMA)
 *                          ETH1TXEN [16]        1       TX clock
 *                          ETH1RXEN [17]        1       RX clock
 *   RCC_AHB1RSTR p.463     ETH1MACRST [15]      1 -> 0  reset pulse of the whole ETH block
 */
void eth_stm32_enable_eth(void)
{
	SYSCFG->PMCR = (SYSCFG->PMCR & ~SYSCFG_PMCR_EPIS_SEL_Msk) | SYSCFG_PMCR_EPIS_SEL_2;
	(void)SYSCFG->PMCR;
	RCC->AHB1ENR |= RCC_AHB1ENR_ETH1MACEN | RCC_AHB1ENR_ETH1TXEN | RCC_AHB1ENR_ETH1RXEN;
	(void)RCC->AHB1ENR;
	RCC->AHB1RSTR |= RCC_AHB1RSTR_ETH1MACRST;
	__DSB();
	RCC->AHB1RSTR &= ~RCC_AHB1RSTR_ETH1MACRST;
	__DSB();

	FLOW("[3] SYSCFG_PMCR = 0x%08lX -> EPIS = %lu (4 = RMII)\n", (unsigned long)SYSCFG->PMCR,
		 (unsigned long)((SYSCFG->PMCR & SYSCFG_PMCR_EPIS_SEL_Msk) >> SYSCFG_PMCR_EPIS_SEL_Pos));
	FLOW("[3] RCC_AHB1ENR = 0x%08lX (ETH1MAC/TX/RX clocks ON), ETH block reset by RCC\n",
		 (unsigned long)RCC->AHB1ENR);
	FLOW2("[3] RCC_AHB1RSTR = 0x%08lX (ETH1MACRST = %d, must be 0)\n", (unsigned long)RCC->AHB1RSTR,
		  (RCC->AHB1RSTR & RCC_AHB1RSTR_ETH1MACRST) != 0U);
}
/* The test polls the descriptors: ETH interrupt off in the NVIC, then 10 us for the ETH block after the reset.
 * NVIC_ICER / NVIC_ICPR bit of ETH_IRQn (61) = 1: interrupt disabled, pending flag cleared */
void eth_stm32_disable_eth_irq(void)
{
	NVIC_DisableIRQ(ETH_IRQn);
	NVIC_ClearPendingIRQ(ETH_IRQn);
	udelay(10);
	FLOW("[3] ETH interrupt OFF (the test polls the descriptors)\n");
}
/* [1] Chip ID, core revision, bus clocks, caches (FLOW_LOG 2) */
void eth_stm32_print_chip_info(void)
{
	if (FLOW_LOG < 2) {
		return;
	}
	uint32_t id = DBGMCU->IDCODE, cpu = SCB->CPUID;
	FLOW2("      Chip: DBGMCU_IDCODE=0x%08lX -> DEV_ID=0x%03lX (0x450 = STM32H74x/75x), REV_ID=0x%04lX\n",
		  (unsigned long)id, (unsigned long)(id & 0xFFFU), (unsigned long)(id >> 16));
	FLOW2("      Core: SCB->CPUID=0x%08lX -> Cortex-M7 r%lup%lu\n",
		  (unsigned long)cpu, (unsigned long)((cpu >> 20) & 0xFU), (unsigned long)(cpu & 0xFU));
	FLOW2("      Clock: SYSCLK=%lu HCLK=%lu PCLK1=%lu PCLK2=%lu Hz\n",
		  (unsigned long)HAL_RCC_GetSysClockFreq(), (unsigned long)HAL_RCC_GetHCLKFreq(),
		  (unsigned long)HAL_RCC_GetPCLK1Freq(), (unsigned long)HAL_RCC_GetPCLK2Freq());
	FLOW2("      Cache: I-cache=%lu D-cache=%lu (SCB->CCR bit 17/16)\n",
		  (unsigned long)((SCB->CCR >> 17) & 1U), (unsigned long)((SCB->CCR >> 16) & 1U));
}
