#ifndef ETH_COE_UTILS_H
#define ETH_COE_UTILS_H

#include <stdint.h>

// === Checksum offload test: TDES3.CIC, MAC_Configuration.IPC, RDES1 IPHE / IPCE / PT (eth_run_test_ops = 7) ===
uint16_t eth_mac_coe_lb(uint8_t mac_idx);

#endif // ETH_COE_UTILS_H
