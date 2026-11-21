/* SPDX-License-Identifier: GPL-2.0 */
/*
 * eqos_port.h - phan STM32H755 cua driver: [2] kiem tra bo nho DMA,
 * [3] DWT, chan RMII, SYSCFG, RCC, NVIC. Than ham: eqos_port.c.
 */
#ifndef EQOS_PORT_H
#define EQOS_PORT_H

#include <stdint.h>
// GPIO_TypeDef, dia chi GPIOA..GPIOK
#include "stm32h7xx.h"

// Mot chan RMII: port GPIO (vi du GPIOA = 0x58020000) + so chan 0..15
struct rmii_pin { GPIO_TypeDef *port; uint32_t pin; };
// RMII luon co 9 tin hieu: REF_CLK, MDIO, CRS_DV, MDC, RXD0, RXD1, TXD1, TX_EN, TXD0
#define RMII_PIN_COUNT 9U

// PART 1 - BOARD (Nucleo-H755ZI-Q, MB1363): UART va day noi tu MCU sang PHY
struct board_cfg {
  uint32_t uart_baud;                          // toc do UART COM1 (qua ST-LINK)
  struct rmii_pin rmii_pins[RMII_PIN_COUNT];   // chan MCU noi voi PHY LAN8742 (schematic MB1363 tr.6)
};

int  stm32_precheck(void);                          // [2] D-cache phai tat (tra ve EQOS_EINVAL neu dang bat)
void stm32_config_eth(const struct board_cfg *c);   // [3] DWT, 9 chan RMII, SYSCFG = RMII, clock + reset ETH, tat IRQ ETH

#endif /* EQOS_PORT_H */
