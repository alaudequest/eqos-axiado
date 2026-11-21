#ifndef ETH_FUNCTION_UTILS
#define ETH_FUNCTION_UTILS

#include <stdint.h>
#include "eth_registers.h"   // MAC_100M_TX_DESC_CNT, MAC_100M_RX_DESC_CNT
#include "mac_defines.h"     // struct eth_desc

extern int eth_run_test_ops;

/* Platform functions (eth_function_stm32.c) */
void mdelay(uint32_t n);
void udelay(uint32_t n);
uint32_t eth_csr_clock_hz(void);	// MAC CSR clock (HCLK): MAC_1US_Tic_Counter
uint32_t eth_get_ms(void);			// ms since boot (FLOW step titles)
/**
 * Declare functions
 */
// === Test entry (src/L0_MK_RMII_MAC_LOOPBACK.c) ===
int do_test(void);

// === Primary Tests ===
uint16_t eth_test_traffic(int8_t mac_idx, uint16_t pkt_cnt);
uint16_t eth_mac_internal_lb(uint8_t mac_idx);

// === MAC config and reset ===
uint8_t eth_reset_mac(uint8_t mac_idx);
uint8_t mac_reset_100m(uint8_t mac_idx);
void mac_init_100m(uint8_t mac_idx);
void mac_dma_init(uint8_t mac_idx);
void mac_desc_init(uint8_t mac_idx);
void mac_mtl_init(uint8_t mac_idx);
void mac_basic_init(uint8_t mac_idx);
void mac_start(uint8_t mac_idx);

// === MAC diagnostics and status ===
void eth_print_mac_counters(uint32_t mac_idx);

// === Low-level packet operations ===
int compare_packets(unsigned char *received, int received_len, unsigned char *sent, int sent_len);
int compare_packets_full(unsigned char *received, int received_len, unsigned char *sent, int sent_len);
int ingress_packet(int mac_idx, unsigned char buf[], int blen);
int ingress_packet_status(int mac_idx, unsigned char buf[], int blen, int timeout_us, uint32_t *rdes2, uint32_t *rdes3);
int egress_packet(unsigned char buf[], int len);
uint32_t egress_context_vlan(uint16_t tag);	// TX context descriptor "insert this VLAN tag"; returns its index
uint32_t egress_context_qinq(uint16_t outer, uint16_t inner, uint8_t ivtir);	// context with outer + inner tag (QinQ); ivtir = TDES3_CTXT_IVTIR_*
uint32_t egress_context_bad(uint16_t tag);	// context with CTXT + FD + LD (invalid): sent alone, returns its write-back TDES3 (CDE expected)
uint32_t egress_context_mss(uint16_t mss);	// context "segment size for TSO = mss" (TSO test)
uint32_t egress_context_ost(uint32_t ts_lo, uint32_t ts_hi);	// context "one-step timestamp" (OSTC + TCMSSV, TDES0/1 = ingress timestamp)
int egress_tso(const unsigned char *frame, int hdr_len, int pay_len, int thl);	// one TSO descriptor: header buffer 1, payload buffer 2
int generate_ethernet_packet(unsigned char *tx_buffer, unsigned char *dest_mac, unsigned char *src_mac, int payload_len);
void eth_rx_flush(uint8_t mac_idx);

// === MAC Loopback specific ===
void init_mac_internal_lb(uint8_t mac_idx);
void disable_mac_internal_lb(uint8_t mac_idx);
void enable_mac_internal_lb(uint8_t mac_idx);

// === Descriptor rings (eth_functions.c) ===
extern struct eth_desc tx_desc[MAC_100M_TX_DESC_CNT];
extern struct eth_desc rx_desc[MAC_100M_RX_DESC_CNT];
extern uint32_t eth_tx_head;	// next TX descriptor used by egress_packet()
extern uint32_t eth_rx_head;	// next RX descriptor read by ingress_packet()

// === PTP timestamps (eth_functions.c; test in eth_ptp_functions.c) ===
struct eth_timestamp {
	uint32_t s;			// seconds (TTSH / RTSH)
	uint32_t ns;		// nanoseconds (TTSL / RTSL): 0..999999999 with MAC_Timestamp_Control.TSCTRLSSR = 1
	uint8_t  valid;		// 1: written by the DMA for the last frame
};
extern uint8_t eth_ptp_on;				// 1: TDES2.TTSE on every TX frame, RX context descriptors consumed
extern uint8_t eth_tx_vtir;				// TDES2.VTIR of the next frames sent (TDES2_VTIR_NONE / INSERT ...)
extern uint8_t eth_tx_cic;				// TDES3.CIC of the next frames sent (TDES3_CIC_NONE / IP / IP_PAYLOAD / FULL)
extern uint8_t eth_tx_saic;				// TDES3.SAIC of the next frames sent (TDES3_SAIC_NONE / INSERT / REPLACE, | TDES3_SAIC_MACA1)
extern uint8_t eth_tx_cpc;
extern uint8_t eth_tx_ioc;				// TDES2.IOC of the next frames sent (0 / 1, interrupt status test only)
extern uint8_t eth_rx_ioc;				// RDES3.IOC of the next RX descriptors given back (0 / 1, interrupt status test only)				// TDES3.CPC of the next frames sent (TDES3_CPC_CRC_PAD / CRC_NOPAD / NONE / CRC_REPLACE)
extern uint32_t eth_rx_rdes1;			// write-back RDES1 of the last frame received (RX COE status, p.3066)
extern struct eth_timestamp eth_tx_ts;	// timestamp of the last frame sent (egress_packet)
extern struct eth_timestamp eth_rx_ts;	// timestamp of the last frame received (ingress_packet_status)

#endif //// ETH_FUNCTION_UTILS
