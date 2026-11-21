#ifndef ETH_FLOW_UTILS_H
#define ETH_FLOW_UTILS_H

#include <stdint.h>
#include "eth_printf.h"

/* ================================ FLOW LOG ==================================
 * Print each step of the code on the UART, to follow the code flow at run time.
 * Each line starts with the step number:
 *   [1] board (main.c)               [2] D-cache check            [3] DWT, RMII pins, SYSCFG, RCC
 *   [4] software reset (SWR)         [5] DMA channel 0            [6] descriptor rings
 *   [7] MTL queue 0                  [8] MAC, loopback, start     [9] test frame
 *   [10] send 1 frame (TX)           [11] receive 1 frame (RX), compare
 *
 *   FLOW_LOG = 0 : no FLOW line (eth_printf and ETH_PRINTF_L1 lines stay)
 *   FLOW_LOG = 1 : 1-2 lines per step + register read-back (hex)
 *   FLOW_LOG = 2 : + step title with time (ms), chip info, value written vs read back,
 *                  bit fields, DMA/MTL/MAC state, descriptor rings, TDES3/RDES3 fields,
 *                  MMC counters, frame hex dump
 * [10] and [11] are printed for the first FLOW_PKT_MAX frames of eth_test_traffic();
 * the other frames only print "." each.
 * Both can be set here or on the command line (-DFLOW_LOG=1).
 * UART 115200 baud = ~87 us per character: no FLOW inside the TX/RX polling loops,
 * and none in an interrupt handler (BSP_PB_Callback).
 */
#ifndef FLOW_LOG
#define FLOW_LOG 2
#endif
#ifndef FLOW_PKT_MAX
#define FLOW_PKT_MAX 1
#endif

// Off: "if (0)" keeps the arguments visible to the compiler (format check, no "unused" warning)
// but no code is generated
#if FLOW_LOG >= 1
#define FLOW(...) eth_printf(__VA_ARGS__)
#else
#define FLOW(...) do { if (0) { eth_printf(__VA_ARGS__); } } while (0)
#endif
// FLOW2: only at FLOW_LOG >= 2 (detailed log)
#if FLOW_LOG >= 2
#define FLOW2(...) eth_printf(__VA_ARGS__)
#else
#define FLOW2(...) do { if (0) { eth_printf(__VA_ARGS__); } } while (0)
#endif
// FLOW_PKT / FLOW2_PKT: same, only for a frame being traced (eth_flow_trace = 1)
#define FLOW_PKT(...)  do { if (eth_flow_trace) { FLOW(__VA_ARGS__); } } while (0)
#define FLOW2_PKT(...) do { if (eth_flow_trace) { FLOW2(__VA_ARGS__); } } while (0)
// Title of a step with the time since boot (FLOW_LOG >= 2)
#define FLOW_STEP(tag, title) flow_step((tag), (title))

/* 1 while egress_packet / ingress_packet print [10] / [11] for the current frame
 * (set by eth_test_traffic for the first FLOW_PKT_MAX frames) */
extern uint8_t eth_flow_trace;

/* Bit field tables used by flow_reg() / flow_fields() (eth_flow_functions.c) */
enum flow_fields_id {
	FF_NONE,
	FF_DMA_MODE, FF_DMA_SYSBUS_MODE, FF_DMA_CH0_CONTROL, FF_DMA_CH0_TX_CONTROL, FF_DMA_CH0_RX_CONTROL, FF_DMA_CH0_STATUS,
	FF_MTL_TXQ0_OPERATION_MODE, FF_MTL_RXQ0_OPERATION_MODE,
	FF_MAC_CONFIGURATION, FF_MAC_PACKET_FILTER, FF_MAC_WD_JB_TIMEOUT, FF_MAC_TIMESTAMP_CONTROL,
	FF_MAC_VLAN_TAG, FF_MAC_VLAN_INCL, FF_MAC_INNER_VLAN_INCL,
	FF_TDES3_RD, FF_TDES3_WB, FF_RDES3_RD, FF_RDES3_WB,
};

// === Registers ===
void flow_reg(const char *name, uint32_t base, uint32_t offset, enum flow_fields_id f);	// read-back (1) + bit fields (2)
void flow_reg2(const char *name, uint32_t base, uint32_t offset, enum flow_fields_id f);	// same, only at FLOW_LOG 2
void flow_regw(const char *name, uint32_t base, uint32_t offset, uint32_t written,
			   enum flow_fields_id f);													// read-back, + value written if it differs
void flow_fields(enum flow_fields_id f, uint32_t v);									// "-> ST=1 OSF=1 TXPBL=8" (2)

// === Steps and time ===
void flow_step(const char *tag, const char *title);										// ==== [n] title (t = x ms) ==== (2)

// === State (FLOW_LOG 2) ===
void flow_dma_state(uint8_t mac_idx);	// DMA process state TPS/RPS, current descriptors, DMA_CH0_Status
void flow_mtl_state(uint8_t mac_idx);	// the 2 MTL FIFOs
void flow_mac_state(uint8_t mac_idx);	// MAC transmitter / receiver
void flow_mmc(uint8_t mac_idx);			// MMC counters
void flow_rings(uint8_t mac_idx, const char *why);	// both descriptor rings
void flow_frame_dump(const char *title, const uint8_t *p, uint32_t len, uint32_t payload_len);

#endif // ETH_FLOW_UTILS_H
