#ifndef ETH_VLAN_UTILS_H
#define ETH_VLAN_UTILS_H

#include <stdint.h>

// === VLAN insertion test: TX context descriptor, TDES2.VTIR, MAC_VLAN_Incl (eth_run_test_ops = 4) ===
uint16_t eth_mac_vlan_lb(uint8_t mac_idx);
// === QinQ test: context with outer + inner tag, MAC_VLAN_Incl.CSVL, MAC_Inner_VLAN_Incl (eth_run_test_ops = 5) ===
uint16_t eth_mac_qinq_lb(uint8_t mac_idx);

#endif // ETH_VLAN_UTILS_H
