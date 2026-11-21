#ifndef ETH_FUNCTION_STM32
#define ETH_FUNCTION_STM32

#include <stdint.h>

// === Test flow ===
uint16_t eth_rmii_mac_lb(uint8_t mac_idx);

// === STM32H755 Ethernet bring-up ===
uint8_t eth_stm32_init(uint8_t mac_idx);
uint8_t eth_stm32_check_dcache(void);
void eth_stm32_enable_dwt(void);
void eth_stm32_config_rmii_pins(void);
void eth_stm32_enable_eth(void);
void eth_stm32_disable_eth_irq(void);
void eth_stm32_print_chip_info(void);

#endif //// ETH_FUNCTION_STM32
