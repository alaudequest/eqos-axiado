#ifndef ETH_VLAN_RX_UTILS_H
#define ETH_VLAN_RX_UTILS_H

#include <stdint.h>

// === VLAN filter test: MAC_VLAN_Tag VL/ETV/VTIM/VTHM/ERSVLM/ERIVLT/DOVLTC, MAC_Packet_Filter.VTFE (eth_run_test_ops = 8) ===
uint16_t eth_mac_vlan_filter_lb(uint8_t mac_idx);
// === VLAN strip test: MAC_VLAN_Tag EVLS/EIVLS, RDES0 OVT/IVT, RDES2.VF (eth_run_test_ops = 9) ===
uint16_t eth_mac_vlan_strip_lb(uint8_t mac_idx);

#endif // ETH_VLAN_RX_UTILS_H
