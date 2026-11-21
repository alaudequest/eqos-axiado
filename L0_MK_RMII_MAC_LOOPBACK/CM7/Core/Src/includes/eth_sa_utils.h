#ifndef ETH_SA_UTILS_H
#define ETH_SA_UTILS_H

#include <stdint.h>

// === SA insertion / replacement and CRC-pad control test: TDES3 SAIC / CPC, MAC_Configuration.SARC, MTL_RxQ0_Operation_Mode FUP / FEP (eth_run_test_ops = 10) ===
uint16_t eth_mac_sa_cpc_lb(uint8_t mac_idx);

#endif // ETH_SA_UTILS_H
