#ifndef ETH_TSO_UTILS_H
#define ETH_TSO_UTILS_H

#include <stdint.h>

// === TSO test: MSS context descriptor, TDES3.TSE, DMA_CH0_Tx_Control.TSE (eth_run_test_ops = 6) ===
uint16_t eth_mac_tso_lb(uint8_t mac_idx);

#endif // ETH_TSO_UTILS_H
