#ifndef ETH_DMA_ORDER_UTILS_H
#define ETH_DMA_ORDER_UTILS_H

#include <stdint.h>

// === DMA start order test: TX tail pointer before DMA_CH0_Tx_Control.ST, RX frame held in the MTL RX FIFO while DMA_CH0_Rx_Control.SR = 0 (eth_run_test_ops = 13) ===
uint16_t eth_mac_dma_order_lb(uint8_t mac_idx);

#endif // ETH_DMA_ORDER_UTILS_H
