#ifndef ETH_FUNCTION_STM32_H
#define ETH_FUNCTION_STM32_H

#include <stdint.h>

/* Test flow: STM32 init, EQoS bring-up, then the test chosen by eth_run_test_ops.
 * Return 1: PASS, 0: FAIL */
uint16_t eth_rmii_mac_lb(uint8_t mac_idx);
/* D-cache check, DWT, RMII pins, SYSCFG = RMII, ETH clocks + reset, ETH IRQ off. Return 1: OK, 0: FAIL */
uint8_t eth_stm32_init(uint8_t mac_idx);

#endif /* ETH_FUNCTION_STM32_H */
