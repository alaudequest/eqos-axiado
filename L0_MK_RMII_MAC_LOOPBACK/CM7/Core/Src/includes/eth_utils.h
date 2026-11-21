#ifndef ETH_UTILS_H
#define ETH_UTILS_H

#include <stdio.h>

/* 1: ETH_DEBUG_PRINTF lines (start/end of egress and ingress, packet match) */
#define ETH_DEBUG 0

#if ETH_DEBUG
    #define ETH_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
    #define ETH_DEBUG_PRINTF(...) do {} while (0)
#endif

// Set global log level here (1, 2, or 3)
// The tests use ETH_PRINTF_L1 (START/END banners, Actual/Expected, PASS/FAIL).
// The step-by-step log of the code is the FLOW log: FLOW_LOG in eth_flow_utils.h
#define DEBUG_LEVEL 3

// Define macros for each log level
#define ETH_PRINTF_L1(fmt, ...)  do { if (DEBUG_LEVEL >= 1) printf("[L1] " fmt "\r\n", ##__VA_ARGS__); } while (0)
#define ETH_PRINTF_L2(fmt, ...)  do { if (DEBUG_LEVEL >= 2) printf("[L2] " fmt "\r\n", ##__VA_ARGS__); } while (0)
#define ETH_PRINTF_L3(fmt, ...)  do { if (DEBUG_LEVEL >= 3) printf("[L3] " fmt "\r\n", ##__VA_ARGS__); } while (0)

#endif // ETH_UTILS_H
