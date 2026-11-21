#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "eth_printf.h"
#define ETH_VAL_PRINT_INFO
/*
 * Print eth debug info, use in validation code
 * Each character goes to uart_putc(); '\n' is sent as "\r\n".
 * Supported (subset of printf):
 *   %d %i %u %x %X %c %s %p %%
 *   flags '-' (left align) and '0' (pad with 0), width (number or '*'),
 *   length 'l' (long), 'll' (long long), 'z' (size_t), 'h' / 'hh' (accepted, ignored)
 *   %x prints like printf: "%08lX" -> 0000ABCD; write "0x%08lX" to get 0x0000ABCD.
 * Each argument is read with its real type: on Cortex-M7 (32-bit) an int is 4 bytes,
 * so reading it as uint64_t takes 8 bytes and shifts every next argument.
 **/

#define PAD_ZERO	(1 << 0)	// '0' flag: pad with 0 instead of spaces
#define PAD_LEFT	(1 << 1)	// '-' flag: left align, spaces after
#define NUM_UPPER	(1 << 2)	// %X: A-F instead of a-f

/* Character out, '\n' -> "\r\n" */
static void eth_putc(char c)
{
    if (c == '\n') {
        uart_putc('\r');   // carriage return
    }
    uart_putc(c);
}
/* Digits of n (base 10 or 16) with the sign, padded to width */
static void eth_putnum(uint64_t n, int neg, unsigned int base, int width, int flags)
{
    const char *digits = (flags & NUM_UPPER) ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[24];
    int i = 0;
    int pad;

    do {
        buf[i++] = digits[n % base];
        n /= base;
    } while (n != 0);

    pad = width - i - (neg ? 1 : 0);
    if (!(flags & (PAD_LEFT | PAD_ZERO))) {
        for (; pad > 0; pad--)
            uart_putc(' ');
    }
    if (neg) {
        uart_putc('-');
    }
    if (flags & PAD_ZERO) {
        for (; pad > 0; pad--)
            uart_putc('0');
    }
    while (i > 0) {
        uart_putc(buf[--i]);
    }
    for (; pad > 0; pad--) {   // left align: spaces after the number
        uart_putc(' ');
    }
}
/* Decimal number: %d %i %u */
static void eth_putdec(uint64_t n, int neg, int width, int flags)
{
    eth_putnum(n, neg, 10, width, flags);
}
/* Hexadecimal number: %x %X (no "0x", like printf) */
static void eth_puthex(uint64_t n, int width, int flags)
{
    eth_putnum(n, 0, 16, width, flags);
}
/* String: %s %c, padded to width with spaces */
static void eth_putstr(const char *s, int width, int flags)
{
    const char *p;
    int pad;

    if (s == NULL) {
        s = "(null)";
    }
    for (p = s; *p; p++) {
    }
    pad = width - (int)(p - s);
    if (!(flags & PAD_LEFT)) {
        for (; pad > 0; pad--)
            uart_putc(' ');
    }
    for (; *s; s++) {
        eth_putc(*s);
    }
    for (; pad > 0; pad--) {
        uart_putc(' ');
    }
}
void eth_printf(const char *fmt, ...)
{
	#ifdef ETH_VAL_PRINT_INFO
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            eth_putc(*fmt++);
            continue;
        }
        fmt++;

        // flags, width and length, e.g. "%-3s", "%08lX", "%*s", "%lu"
        int flags = 0, width = 0, lng = 0, size = 0;
        for (;; fmt++) {
            if (*fmt == '-') {
                flags |= PAD_LEFT;
            } else if (*fmt == '0') {
                flags |= PAD_ZERO;
            } else {
                break;
            }
        }
        if (*fmt == '*') {
            width = va_arg(args, int);
            if (width < 0) {
                flags |= PAD_LEFT;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt++ - '0');
            }
        }
        while (*fmt == 'l') {
            lng++;
            fmt++;
        }
        while (*fmt == 'h') {      // short / char are promoted to int
            fmt++;
        }
        if (*fmt == 'z') {
            size = 1;
            fmt++;
        }
        if (flags & PAD_LEFT) {
            flags &= ~PAD_ZERO;    // '-' wins over '0'
        }

        switch (*fmt) {
            case 'd':
            case 'i': {
                int64_t val = size ? (int64_t)va_arg(args, size_t)
                            : (lng >= 2) ? (int64_t)va_arg(args, long long)
                            : (lng == 1) ? (int64_t)va_arg(args, long)
                            : (int64_t)va_arg(args, int);
                uint64_t mag = (val < 0) ? (uint64_t)0 - (uint64_t)val : (uint64_t)val;
                eth_putdec(mag, val < 0, width, flags);
                break;
            }
            case 'u':
            case 'x':
            case 'X': {
                uint64_t val = size ? (uint64_t)va_arg(args, size_t)
                             : (lng >= 2) ? (uint64_t)va_arg(args, unsigned long long)
                             : (lng == 1) ? (uint64_t)va_arg(args, unsigned long)
                             : (uint64_t)va_arg(args, unsigned int);
                if (*fmt == 'u') {
                    eth_putdec(val, 0, width, flags);
                } else {
                    eth_puthex(val, width, flags | ((*fmt == 'X') ? NUM_UPPER : 0));
                }
                break;
            }
            case 'p': {
                uintptr_t val = (uintptr_t)va_arg(args, void *);
                uart_putc('0');
                uart_putc('x');
                eth_puthex(val, (width > 2) ? width - 2 : 0, flags);
                break;
            }
            case 'c': {
                char str[2] = { (char)va_arg(args, int), '\0' };
                eth_putstr(str, width, flags);
                break;
            }
            case 's':
                eth_putstr(va_arg(args, const char *), width, flags);
                break;
            case '%':
                uart_putc('%');
                break;
            case '\0':             // '%' at the end of the text
                fmt--;
                break;
            default:               // not supported: print it as is
                uart_putc('%');
                uart_putc(*fmt);
                break;
        }
        fmt++;
    }

    va_end(args);
    uart_flush();
	#endif
}
