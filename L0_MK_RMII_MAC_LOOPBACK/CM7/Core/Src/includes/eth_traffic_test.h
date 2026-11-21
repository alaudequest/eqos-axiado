#ifndef ETH_TRAFFIC_TEST_H
#define ETH_TRAFFIC_TEST_H

#include "eth_registers.h"   // MAC_100M_BUF_SIZE

#define ETH_TEST_MAC_SIZE			  6
#define ETH_TEST_HEADER_SIZE		  14
#define ETH_TEST_MIN_PAYLOAD		  46
/* frame + 4-byte FCS must fit one RX buffer (no frame over 2 descriptors): 2304 - 14 - 4 = 2286 */
#define ETH_TEST_MAX_PAYLOAD		  (MAC_100M_BUF_SIZE - ETH_TEST_HEADER_SIZE - 4)
//#define ETH_TEST_TRAFFIC_SIZE		  2286		/* 2300-byte frame: RX watchdog test, see eth_registers.h */
#define ETH_TEST_TRAFFIC_SIZE		  64
#define ETH_TEST_TRAFFIC_MAX_BUFFER	  MAC_100M_BUF_SIZE
#define ETH_TEST_ETHER_TYPE			  0x88B5
#define ETH_TEST_TEST_TRAFFIC_PATTERN 0xAA
#define ETH_TEST_DFLT_FRAMES		  0x10

/* Frame addresses. DA is unicast because STM32H755 MMC only counts unicast RX packets */
#define ETH_TEST_DEST_MAC			  { 0x6C, 0x1F, 0xF7, 0xCA, 0x2B, 0xFD }
#define ETH_TEST_SRC_MAC			  { 0x00, 0x80, 0xE1, 0x00, 0x00, 0x00 }	/* board MAC (MAC_Address0) */

/* Polling timeouts (1 us steps) of egress_packet() / ingress_packet() */
#define ETH_TEST_TX_TIMEOUT_US		  1000000	/* 1 s */
#define ETH_TEST_RX_TIMEOUT_US		  10000		/* 10 ms: a looped-back frame is back in ~200 us */

#endif // ETH_TRAFFIC_TEST_H
