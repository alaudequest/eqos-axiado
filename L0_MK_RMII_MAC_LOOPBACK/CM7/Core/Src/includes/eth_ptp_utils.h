#ifndef ETH_PTP_UTILS_H
#define ETH_PTP_UTILS_H

#include <stdint.h>

// === PTP timestamp test: MAC_Timestamp_Control system time, TDES2.TTSE, RX context descriptor (eth_run_test_ops = 3) ===
uint8_t  mac_ptp_init(uint8_t mac_idx);		// start the system time (RM0399 §61.9.9 p.3036), before mac_start()
uint16_t eth_mac_ptp_lb(uint8_t mac_idx);

#endif // ETH_PTP_UTILS_H
