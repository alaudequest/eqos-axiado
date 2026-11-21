#ifndef ETH_ISTAT_UTILS_H
#define ETH_ISTAT_UTILS_H

#include <stdint.h>

// === Interrupt status test: DMA_CH0_Status / DMA_Interrupt_Status / DMA_Debug_Status0 / MTL_Interrupt_Status / MTL_Q0_Interrupt_Control_Status / MAC_Interrupt_Status with nothing enabled (eth_run_test_ops = 12) ===
uint16_t eth_mac_int_status_lb(uint8_t mac_idx);

#endif // ETH_ISTAT_UTILS_H
