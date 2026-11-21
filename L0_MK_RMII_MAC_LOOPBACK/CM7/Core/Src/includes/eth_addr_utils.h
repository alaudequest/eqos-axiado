#ifndef ETH_ADDR_UTILS_H
#define ETH_ADDR_UTILS_H

#include <stdint.h>

// === MAC address filter test: MAC_Packet_Filter (PR = 0), MAC_Address1..3_High/Low, MAC_Hash_Table_Reg0/Reg1, RDES2 MADRM / HF (eth_run_test_ops = 11) ===
uint16_t eth_mac_addr_filter_lb(uint8_t mac_idx);

#endif // ETH_ADDR_UTILS_H
