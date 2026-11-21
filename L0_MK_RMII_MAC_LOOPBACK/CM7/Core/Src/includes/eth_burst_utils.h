#ifndef ETH_BURST_UTILS_H
#define ETH_BURST_UTILS_H

#include <stdint.h>

// === DMA burst test: DMA_SysBus_Mode.FB / AAL (eth_run_test_ops = 2) ===
uint16_t eth_mac_burst_lb(uint8_t mac_idx);

#endif // ETH_BURST_UTILS_H
