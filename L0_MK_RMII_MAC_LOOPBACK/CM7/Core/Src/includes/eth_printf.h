#ifndef ETH_PRINTF_H
#define ETH_PRINTF_H

#include <stdint.h>

/* format(printf): GCC checks every eth_printf() call against its arguments, like printf */
void eth_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* UART output used by eth_printf, provided by the platform (eth_function_stm32.c) */
void uart_putc(char c);
void uart_flush(void);

#endif // ETH_PRINTF_H
