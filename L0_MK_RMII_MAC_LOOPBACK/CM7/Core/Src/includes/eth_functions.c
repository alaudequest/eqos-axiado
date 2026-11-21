#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_traffic_test.h"
#include "eth_printf.h"
#include "eth_flow_utils.h"

/*
 * EQoS MAC functions: Synopsys DWC Ethernet QoS, RM0399 chapter 61.
 * Only AX_* register access, mdelay/udelay and eth_printf are used here (no STM32 HAL):
 * clocks, pins, delays and UART of the board are in eth_function_stm32.c.
 * FLOW / FLOW2 lines print each step [4] ... [11] (eth_flow_utils.h, FLOW_LOG).
 *
 *   TX: frame -> tx_buf -> DMA -> MTL TX FIFO -> MAC -> RMII
 *   RX: RMII -> MAC -> MTL RX FIFO -> DMA -> rx_buf -> frame
 */

/* Descriptor rings in section .eqos_nocache (linker script: RAM_D1, NOLOAD -> mac_desc_init() writes
 * every word). D-cache is off (eth_stm32_check_dcache), so the CPU and the DMA see the same RAM. */
struct eth_desc tx_desc[MAC_100M_TX_DESC_CNT] __attribute__((aligned(32), section(".eqos_nocache")));
struct eth_desc rx_desc[MAC_100M_RX_DESC_CNT] __attribute__((aligned(32), section(".eqos_nocache")));
/* One buffer per descriptor (.bss in RAM_D1) */
static uint8_t tx_buf[MAC_100M_TX_DESC_CNT][MAC_100M_BUF_SIZE] __attribute__((aligned(32)));
static uint8_t rx_buf[MAC_100M_RX_DESC_CNT][MAC_100M_BUF_SIZE] __attribute__((aligned(32)));
uint32_t eth_tx_head;
uint32_t eth_rx_head;
/* PTP timestamps (eth_ptp_functions.c). eth_ptp_on = 1: egress_packet() sets TDES2.TTSE on every frame and
 * reads the timestamp back from TDES0/1; ingress_packet_status() consumes the context descriptor
 * (RDES3.CTXT = 1) that the DMA writes after every frame when MAC_Timestamp_Control.TSENALL = 1. 0: unchanged */
uint8_t eth_ptp_on;
struct eth_timestamp eth_tx_ts;
struct eth_timestamp eth_rx_ts;
/* VLAN (eth_vlan_functions.c): TDES2.VTIR written in every frame sent, 0 = frame sent as it is */
uint8_t eth_tx_vtir;
/* Checksum offload (eth_coe_functions.c): TDES3.CIC written in every frame sent, 0 = no checksum inserted */
uint8_t eth_tx_cic;
/* SA / CRC test (eth_sa_functions.c): TDES3.SAIC and TDES3.CPC written in every frame sent, 0 = SA untouched, pad + FCS added */
uint8_t eth_tx_saic;
uint8_t eth_tx_cpc;
uint8_t eth_tx_ioc;		// TDES2.IOC of the next frames sent (interrupt status test only)
uint8_t eth_rx_ioc;		// RDES3.IOC of the next RX descriptors given back (interrupt status test only)
/* RX COE status: write-back RDES1 of the last frame received by ingress_packet_status() (IPHE, IPCE, PT, p.3066) */
uint32_t eth_rx_rdes1;

/* Ring index wrap (idx + 1) & (count - 1) needs a power of two; RBSZ is 14 bits */
_Static_assert(sizeof(struct eth_desc) == 16, "DMA descriptor must be 16 bytes (DSL = 0)");
_Static_assert((MAC_100M_TX_DESC_CNT & (MAC_100M_TX_DESC_CNT - 1)) == 0, "TX ring must be a power of two");
_Static_assert((MAC_100M_RX_DESC_CNT & (MAC_100M_RX_DESC_CNT - 1)) == 0, "RX ring must be a power of two");
_Static_assert(MAC_100M_BUF_SIZE % 32 == 0 && MAC_100M_BUF_SIZE <= 0x3FFF, "RX buffer must fit RBSZ");

/**
 * @brief read mac counter for given mac (MMC, RM0399 p.3156-3165)
 * Read only, no bit set. Every counter restarts from 0 at eth_reset_mac() (DMA_Mode.SWR).
 *   Counter                                  Offset  Counts
 *   ---------------------------------------- ------- ------------------------------
 *   Tx_Packet_Count_Good                 0x768   frames sent without error
 *   Tx_Single_Collision_Good_Packets     0x74C   frames sent after 1 collision (half duplex only)
 *   Tx_Multiple_Collision_Good_Packets   0x750   frames sent after 2+ collisions (half duplex only)
 *   Rx_Unicast_Packets_Good              0x7C4   unicast frames received without error
 *   Rx_CRC_Error_Packets                 0x794   frames received with a bad FCS
 *   Rx_Alignment_Error_Packets           0x798   frames received with a dribble error (not whole bytes)
 */
void eth_print_mac_counters(uint32_t mac_idx)
{
	uint32_t tx_good, tx_scol, tx_mcol, rx_good, rx_crc, rx_align;
	tx_good = AX_READ_MAC_REG32(mac_idx, MMC_STATS_TX_PKT_GOOD);
	tx_scol = AX_READ_MAC_REG32(mac_idx, MMC_STATS_TX_SINGLE_COL_GOOD);
	tx_mcol = AX_READ_MAC_REG32(mac_idx, MMC_STATS_TX_MULTI_COL_GOOD);
	rx_good = AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_UNICAST_GOOD);
	rx_crc = AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_CRC_ERR);
	rx_align = AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_ALIGN_ERR);

	eth_printf("Stats for MAC-idx: %lu\n", (unsigned long)mac_idx);
	eth_printf("RX_MAC(unicast good/crc err/align err): %lu %lu %lu\n",
			   (unsigned long)rx_good, (unsigned long)rx_crc, (unsigned long)rx_align);
	eth_printf("TX_MAC(good/single col/multi col): %lu %lu %lu\n",
			   (unsigned long)tx_good, (unsigned long)tx_scol, (unsigned long)tx_mcol);
}
/* Eth reset the given MAC: DMA_Mode.SWR [0] = 1 (self-clearing), then MAC_1US_Tic_Counter = 1 us tick.
 * Details in mac_reset_100m(). Return 1: done, 0: timeout */
uint8_t eth_reset_mac(uint8_t mac_idx) {
	return mac_reset_100m(mac_idx);
}
/**
 * Setup Ethernet MAC internal loopback (MAC_Configuration.LM, the PHY is not used)
 * Init MAC, enable the loopback, start DMA and MAC
 * Run traffic test
 * Compare the results
 *
 * Bits set, in this order (details and RM0399 pages in the note on top of each function):
 *   Function                 Register.bit [pos]            Value  Function of the bit
 *   ------------------------ ----------------------------- ------ ------------------------------
 *   eth_reset_mac()          DMA_Mode.SWR [0]             1      reset DMA + MTL + MAC (self-clearing)
 *                            MAC_1US_Tic_Counter [11:0]          223    1 us tick = HCLK 224 MHz - 1
 *   mac_init_100m():
 *    mac_dma_init()          DMA_CH0_Tx_Control.OSF [4]          1      TX DMA: next frame before the last status
 *                            DMA_CH0_Tx_Control.TXPBL [21:16]    8      TX burst 8 x 8 (PBLX8) = 64 beats
 *                            DMA_CH0_Rx_Control.RBSZ [14:1]      2304   RX buffer size in bytes
 *                            DMA_CH0_Rx_Control.RXPBL [21:16]    8      RX burst 8 x 8 = 64 beats
 *                            DMA_CH0_Control.PBLX8 [16]         1      TXPBL / RXPBL x 8
 *                            DMA_SysBus_Mode.FB [0]            1      fixed burst length
 *                            DMA_SysBus_Mode.AAL [12]          1      bursts start on an aligned address
 *    mac_desc_init()         TDES3.OWN [31]                0      TX descriptors kept by the CPU
 *                            RDES3.OWN [31]                1      RX descriptors given to the DMA
 *                            RDES3.BUF1V [24]              1      RDES0 = valid buffer address
 *                            DMA_CH0_TxDesc/RxDesc_List_Address, _Ring_Length, _Tail_Pointer         ring address, length - 1, tail pointer
 *    mac_mtl_init()          MTL_TxQ0_Operation_Mode.TQS [18:16]     7      TX queue 2 KB
 *                            MTL_TxQ0_Operation_Mode.TXQEN [3:2]     2      TX queue 0 on
 *                            MTL_RxQ0_Operation_Mode.DIS_TCP_EF [6]  1      keep frames with a TCP/IP checksum error
 *    mac_basic_init()        MAC_Packet_Filter.PR [0]             1      promiscuous: receive every DA
 *                            MAC_Address0_High, MAC_Address0_Low             board address 00:80:E1:00:00:00
 *                            MAC_WD_JB_Timeout.PWE [8]            1      RX watchdog cut-off set by WTO
 *                            MAC_WD_JB_Timeout.WTO [3:0]          0      cut RX frames after 2048 bytes
 *                            MAC_Configuration.CST [21], ACS [20]  1      strip the FCS of received frames
 *                            MAC_Configuration.JE [16]             1      jumbo frames (up to 9018 bytes)
 *                            MAC_Configuration.FES [14]            1      100 Mbit/s
 *                            MAC_Configuration.DM [13]             1      full duplex (needed by the loopback)
 *   init_mac_internal_lb()   MAC_Configuration.LM [12]             1      loopback: TX goes back to RX inside the MAC
 *   mac_start()              DMA_CH0_Tx_Control.ST [0]           1      start TX DMA
 *                            DMA_CH0_Rx_Control.SR [0]           1      start RX DMA
 *                            MAC_Configuration.TE [1]              1      start MAC transmitter
 *                            MAC_Configuration.RE [0]              1      start MAC receiver (last)
 *   eth_test_traffic()       TDES0-3, DMA_CH0_TxDesc_Tail_Pointer              send each frame (egress_packet)
 *                            RDES0-3, DMA_CH0_RxDesc_Tail_Pointer              receive each frame (ingress_packet)
 *   eth_print_mac_counters() MMC counters                         read only
 * Return 1: PASS (every frame came back the same), 0: FAIL
 */
uint16_t eth_mac_internal_lb(uint8_t mac_idx)
{
	uint16_t status;
	eth_printf("Start Init: Port[%d]\n", mac_idx);

	if (eth_reset_mac(mac_idx) == 0) {
		return 0;
	}
	mac_init_100m(mac_idx);
	init_mac_internal_lb(mac_idx);
	mac_start(mac_idx);

	status = eth_test_traffic(mac_idx, ETH_TEST_DFLT_FRAMES);
	eth_print_mac_counters(mac_idx);
	return status;
}
/* Helpers for traffic test */
/* @brief: compare rx and tx pkt.
 */
int compare_packets_full(unsigned char *received, int received_len, unsigned char *sent, int sent_len)
{
	if (received_len < sent_len) {
		return 0;
	}
	// comparisons of tx/rx data byte by byte
	for (int i = 0; i < sent_len; i++) {
		//eth_printf("Sent: %x | Received: %x\n", sent[i], received[i]);
		if (received[i] != sent[i]) {
			return 0;
		}
	}

	ETH_DEBUG_PRINTF("Packets match successfully.\r\n");
	eth_printf(".");
	return 1;
}
/* Helpers for traffic test */
/* @brief: compare rx and tx pkt.
 */
int compare_packets(unsigned char *received, int received_len, unsigned char *sent, int sent_len)
{
	if (received_len != sent_len) {
		return 0;
	}
	ETH_DEBUG_PRINTF("Received_len: %d\r\n", received_len);
	// comparisons of tx/rx data byte by byte
	for (int i = 0; i < received_len; i++) {
		//eth_printf("Sent: %x | Received: %x\n", sent[i], received[i]);
		if (received[i] != sent[i]) {
			return 0;
		}
	}

	ETH_DEBUG_PRINTF("Packets match successfully.\r\n");
	eth_printf(".");
	return 1;
}

/* pkt tx/rx */
/* Give rx_desc[idx] back to the DMA: rewrite the 4 words (the write-back overwrote them), OWN last,
 * then RX tail = this descriptor so the DMA can fill it again (RDES read format p.3063-3064) */
static void rx_desc_give_back(uint32_t mac_base, uint32_t idx)
{
	struct eth_desc *rx = &rx_desc[idx];

	rx->des0 = (uint32_t)(uintptr_t)rx_buf[idx];	// RDES0: buffer 1 address
	rx->des1 = 0;
	rx->des2 = 0;
	ETH_DSB();										// descriptor words before OWN
	rx->des3 = RDES3_OWN | RDES3_BUF1V				// OWN [31], BUF1V [24]
			 | (eth_rx_ioc ? RDES3_IOC : 0U);		// IOC [30]: DMA_CH0_Status.RI when this descriptor is filled (interrupt status test only)
	ETH_DSB();										// OWN before the tail pointer write
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_RXDESC_TAIL_POINTER, (uint32_t)(uintptr_t)rx);
	eth_rx_head = (idx + 1) & (MAC_100M_RX_DESC_CNT - 1);
}
/* PTP: read the context descriptor the DMA writes after a stamped packet (RDES1.TSA = 1), then give it back.
 *   RDES0  RTSL [31:0]   sub-second of the RX timestamp (p.3072)
 *   RDES1  RTSH [31:0]   seconds (p.3073); RTSL = RTSH = 0xFFFFFFFF means a corrupt timestamp
 *   RDES3  CTXT [30]     1 = context descriptor, OWN [31] = 0 when written
 * Result in eth_rx_ts. Not there (RDES1.TD, timestamp dropped) or a packet instead: nothing is consumed */
static void ingress_context(int mac_idx, int timeout_us)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t idx = eth_rx_head;
	struct eth_desc *rx = &rx_desc[idx];
	uint32_t status;

	eth_rx_ts.valid = 0;
	while (((status = rx->des3) & RDES3_OWN) && timeout_us > 0) {
		udelay(1);
		timeout_us--;
	}
	if ((status & RDES3_OWN) || (status & RDES3_CTXT) == 0) {
		return;
	}
	ETH_DSB();
	eth_rx_ts.ns = rx->des0;								// RTSL
	eth_rx_ts.s = rx->des1;									// RTSH
	eth_rx_ts.valid = !(rx->des0 == 0xFFFFFFFFU && rx->des1 == 0xFFFFFFFFU);
	FLOW_PKT("[11] rx_desc[%lu] context descriptor: RX timestamp %lu / 0x%08lX (RDES3=0x%08lX)\n",
			 (unsigned long)idx, (unsigned long)rx->des1, (unsigned long)rx->des0, (unsigned long)status);
	rx_desc_give_back(mac_base, idx);
}
/* @brief Same as ingress_packet(), plus:
 * @param timeout_us	max wait for the packet in us (0: check once)
 * @param rdes2, rdes3	if not NULL: write-back RDES2 (filter status, p.3067) and RDES3 of the packet
 * No error is printed: the caller decides (the filter test expects dropped packets).
 * [11] FLOW lines only for a traced frame (eth_flow_trace).
 *
 *   Descriptor             Bit [pos]      Value          Function
 *   ---------------------- -------------- -------------- ------------------------------
 *   read: RX write-back format written by the DMA (p.3065-3071)
 *   RDES3                  OWN [31]       0              the DMA wrote a frame here (1: still waiting)
 *                          CTXT [30]      0              normal descriptor (1: timestamp context)
 *                          FD [29]        1              first descriptor of the frame
 *                          LD [28]        1              last descriptor: whole frame in one buffer
 *                          ES [15]        0              no RX error (1: see RWT/GP/CE/OE/RE)
 *                          PL [14:0]      length         frame length in bytes, FCS removed
 *   write: give the descriptor back, read format (p.3063-3064)
 *   RDES0                  BUF1AP [31:0]  &rx_buf[idx]   buffer address
 *   RDES1, RDES2           all            0
 *   RDES3                  OWN [31]       1              given to the DMA (written last, after DSB)
 *                          BUF1V [24]     1              RDES0 is valid
 *   DMA_CH0_RxDesc_Tail_Pointer p.3086  RDT [31:2]     &rx_desc[idx]  RX tail: the DMA may fill it again
 */
int ingress_packet_status(int mac_idx, unsigned char buf[], int blen, int timeout_us, uint32_t *rdes2, uint32_t *rdes3)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t idx = eth_rx_head;
	struct eth_desc *rx = &rx_desc[idx];
	uint32_t status;
	uint32_t rdes1;
	int len;
	int rlen;

	/* wait for data available: the DMA closes the descriptor (OWN = 0). No print in this loop */
	while (((status = rx->des3) & RDES3_OWN) && timeout_us > 0) {
		udelay(1);
		timeout_us--;
	}
	if (status & RDES3_OWN) {
		return -1;
	}
	ETH_DSB();	// read the buffer only after OWN = 0

	len = status & RDES3_PL_MASK;
	rdes1 = rx->des1;	// write-back RDES1 (TSA, p.3066): read before the descriptor is rewritten
	eth_rx_rdes1 = rdes1;	// kept for the checksum offload test (IPHE, IPCE, PT)
	if (rdes2 != NULL) {
		*rdes2 = rx->des2;
	}
	if (rdes3 != NULL) {
		*rdes3 = status;
	}
	if (eth_flow_trace) {
		FLOW_STEP("[11]", "Receive 1 frame (RX)");
		FLOW("[11] rx_desc[%lu] OWN = 0 -> the DMA is done: RDES3=0x%08lX (CTXT=%d FD=%d LD=%d ES=%d PL=%d)\n",
			 (unsigned long)idx, (unsigned long)status, (status & RDES3_CTXT) != 0, (status & RDES3_FD) != 0,
			 (status & RDES3_LD) != 0, (status & RDES3_ES) != 0, len);
		flow_fields(FF_RDES3_WB, status);
		FLOW2("[11] RDES0=0x%08lX RDES1=0x%08lX RDES2=0x%08lX (write-back format)\n",
			  (unsigned long)rx->des0, (unsigned long)rx->des1, (unsigned long)rx->des2);
	}

	if (status & RDES3_CTXT) {
		/* context descriptor (timestamp), not a packet: ingress_context() reads them when PTP is on */
		rlen = -4;
	} else if (status & RDES3_ES) {
		rlen = -3;
	} else if ((status & (RDES3_FD | RDES3_LD)) != (RDES3_FD | RDES3_LD) || len == 0 || len > MAC_100M_BUF_SIZE) {
		/* packet over several descriptors: not supported, one buffer holds a whole frame */
		rlen = -4;
	} else if (len > blen) {
		/* packet doesn't fit in buffer drop the packet*/
		rlen = -2;
	} else {
		/** read packet from the DMA buffer to buffer */
		memcpy(buf, rx_buf[idx], len);
		rlen = len;
	}

	rx_desc_give_back(mac_base, idx);

	if (eth_flow_trace) {
		FLOW("[11] Give rx_desc[%lu] back to the DMA (RDES3=0x%08lX), RX tail = &rx_desc[%lu], next rx_head = %lu\n",
			 (unsigned long)idx, (unsigned long)rx->des3, (unsigned long)idx, (unsigned long)eth_rx_head);
		flow_dma_state(mac_idx);
		flow_mtl_state(mac_idx);
		flow_mmc(mac_idx);
		flow_rings(mac_idx, "after RX");
	}

	/* PTP (MAC_Timestamp_Control.TSENALL = 1): the timestamp of this packet is in a context descriptor just after it */
	if (eth_ptp_on && (status & RDES3_LD) && (status & RDES3_RS1V) && (rdes1 & RDES1_TSA)) {
		ingress_context(mac_idx, timeout_us);
	}
	return rlen;
}
/* @brief Recv buffer on the MAC (RX descriptor ring, RM0399 p.3063-3071).
 * Wait for the next RX descriptor (OWN = 0), copy the packet to buf,
 * then give the descriptor back to the DMA.
 * @param buf	data will be written, caller allocated.
 * @param blen	len of buf
 * @return len of the packet in bytes (RDES3.PL: FCS stripped, pad kept),
 *         -1 timeout, -2 packet doesn't fit buf, -3 RX error (RDES3.ES), -4 not a one-descriptor packet
 */
int ingress_packet(int mac_idx, unsigned char buf[], int blen)
{
	uint32_t rdes3 = 0;
	int rlen;

	rlen = ingress_packet_status(mac_idx, buf, blen, ETH_TEST_RX_TIMEOUT_US, NULL, &rdes3);

	if (rlen == -1) {
		eth_printf("Timeout on rx \n");
	} else if (rlen == -2) {
		eth_printf("RX packet of %lu bytes doesn't fit the buffer (%d bytes)\n",
				   (unsigned long)(rdes3 & RDES3_PL_MASK), blen);
	} else if (rlen == -3) {
		eth_printf("RX error: RDES3 = 0x%08lX, PL = %lu (RWT %d, GP %d, CE %d, OE %d, RE %d)\n",
				   (unsigned long)rdes3, (unsigned long)(rdes3 & RDES3_PL_MASK),
				   (rdes3 & RDES3_RWT) != 0, (rdes3 & RDES3_GP) != 0, (rdes3 & RDES3_CE) != 0,
				   (rdes3 & RDES3_OE) != 0, (rdes3 & RDES3_RE) != 0);
	} else if (rlen == -4) {
		eth_printf("RX: not a one-descriptor packet, RDES3 = 0x%08lX\n", (unsigned long)rdes3);
	}

	ETH_DEBUG_PRINTF("End of ingress\r\n");
	/** return length in bytes */
	return (rlen);
}
/* Give back to the DMA every RX descriptor that already holds a packet (old packets) */
void eth_rx_flush(uint8_t mac_idx)
{
	while (ingress_packet_status(mac_idx, NULL, 0, 0, NULL, NULL) != -1) {
	}
}
/* @brief Send out buffer on the MAC (TX descriptor ring, RM0399 p.3049-3057).
 * Copy the frame to the TX buffer, give the descriptor to the DMA (OWN = 1), move the TX tail
 * pointer to wake the DMA up, then wait until the DMA gives the descriptor back (OWN = 0).
 * The MAC adds the pad and the FCS (TDES3.CPC = 0).
 * [10] FLOW lines only for a traced frame (eth_flow_trace).
 *
 *   Descriptor             Bit [pos]      Value           Function
 *   ---------------------- -------------- --------------- ------------------------------
 *   write: TX read format (p.3049-3053)
 *   TDES0                  BUF1AP [31:0]  &tx_buf[idx]    address of the frame
 *   TDES1                  BUF2AP [31:0]  0               no buffer 2
 *   TDES2                  B1L [13:0]     len             bytes in buffer 1
 *                          IOC [31]       eth_tx_ioc      0: no interrupt (the CPU polls OWN); 1 sets DMA_CH0_Status.TI (interrupt status test only)
 *   TDES3                  OWN [31]       1               given to the DMA (written last, after DSB)
 *                          FD [29]        1               first descriptor of the frame
 *                          LD [28]        1               last descriptor: one buffer = whole frame
 *                          CPC [27:26]    eth_tx_cpc      0: MAC adds the pad (to 60 bytes) and the FCS (SA/CRC test only)
 *                          SAIC [25:23]   eth_tx_saic     0: SA bytes untouched; insert / replace with MAC_Address0/1 (SA/CRC test only)
 *                          CIC [17:16]    eth_tx_cic      checksum insertion, 0 = none (COE test only)
 *                          FL [14:0]      len             frame length without FCS
 *   DMA_CH0_TxDesc_Tail_Pointer p.3085  TDT [31:2]     &tx_desc[next]  TX tail: wakes up the TX DMA
 *   read: TX write-back format written by the DMA (p.3055-3057)
 *   TDES3                  OWN [31]       0               frame sent, descriptor back to the CPU
 *                          ES [15]        0               no TX error
 *                          PCE [12]       0               CIC != 0: TCP/UDP checksum inserted (1: could not)
 *                          IHE [0]        0               CIC != 0: IP header checksum inserted (1: could not)
 * @param buf	data to send
 * @param len	len of data
 * @return len, -1 bad len, -2 descriptor still owned by the DMA, -3 timeout, -4 TX error (TDES3.ES)
 */
int egress_packet(unsigned char buf[], int len)
{
	uint32_t mac_base = MAC_BASE_ADRS(0);	// STM32H755: one MAC
	uint32_t idx = eth_tx_head;
	struct eth_desc *tx = &tx_desc[idx];
	uint32_t status;
	int count = ETH_TEST_TX_TIMEOUT_US;

	ETH_DEBUG_PRINTF("Start Egress: tx_desc[%lu], len %d\r\n", (unsigned long)idx, len);
	if (eth_flow_trace) {
		FLOW_STEP("[10]", "Send 1 frame (TX)");
		FLOW("[10] Use tx_desc[%lu] @0x%08lX, frame %d bytes\n", (unsigned long)idx,
			 (unsigned long)(uintptr_t)tx, len);
	}

	if (len <= 0 || len > MAC_100M_BUF_SIZE) {
		eth_printf("TX: bad packet length %d\n", len);
		return -1;
	}
	/* never overwrite a descriptor the DMA still owns (after a TX timeout) */
	if (tx->des3 & TDES3_OWN) {
		eth_printf("TX: tx_desc[%lu] still owned by the DMA\n", (unsigned long)idx);
		return -2;
	}

	memcpy(tx_buf[idx], buf, len);
	tx->des0 = (uint32_t)(uintptr_t)tx_buf[idx];	// TDES0: buffer 1 address
	tx->des1 = 0;									// TDES1: no buffer 2
	tx->des2 = (len & TDES2_B1L_MASK)				// TDES2: buffer 1 length
			 | ((uint32_t)eth_tx_vtir << TDES2_VTIR_bit)	// VTIR [15:14]: VLAN tag insert/remove (VLAN test only)
			 | (eth_ptp_on ? TDES2_TTSE : 0U)		// TTSE [30]: timestamp this frame (PTP test only)
			 | (eth_tx_ioc ? TDES2_IOC : 0U);		// IOC [31]: DMA_CH0_Status.TI for this frame (interrupt status test only)
	ETH_DSB();										// descriptor words before OWN
	/* one buffer = whole packet: FD + LD, FL = packet length without FCS */
	tx->des3 = TDES3_OWN | TDES3_FD | TDES3_LD					// OWN [31], FD [29], LD [28]
			 | ((uint32_t)eth_tx_cic << TDES3_CIC_bit)			// CIC [17:16]: checksum insertion (COE test only)
			 | ((uint32_t)eth_tx_saic << TDES3_SAIC_bit)		// SAIC [25:23]: SA insert / replace (SA/CRC test only)
			 | ((uint32_t)eth_tx_cpc << TDES3_CPC_bit)			// CPC [27:26]: pad / FCS control (SA/CRC test only)
			 | (len & TDES3_FL_MASK);							// FL [14:0]: frame length
	ETH_DSB();										// OWN before the tail pointer write

	/* TX tail = next descriptor: the DMA sends every owned descriptor before the tail */
	eth_tx_head = (idx + 1) & (MAC_100M_TX_DESC_CNT - 1);
	if (eth_flow_trace) {
		FLOW("[10] TDES0=0x%08lX TDES1=0x%08lX TDES2=0x%08lX TDES3=0x%08lX (OWN=1 -> given to the DMA)\n",
			 (unsigned long)tx->des0, (unsigned long)tx->des1, (unsigned long)tx->des2, (unsigned long)tx->des3);
		flow_fields(FF_TDES3_RD, tx->des3);
		flow_frame_dump("[10] tx_buf (the DMA reads from here)", tx_buf[idx], len,
						(len > ETH_TEST_HEADER_SIZE) ? len - ETH_TEST_HEADER_SIZE : 0);
		FLOW("[10] Write TX tail = &tx_desc[%lu] to wake up the DMA, then poll OWN...\n", (unsigned long)eth_tx_head);
	}
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_TXDESC_TAIL_POINTER, (uint32_t)(uintptr_t)&tx_desc[eth_tx_head]);

	/* wait for the DMA to close the descriptor (no print here: UART is slow) */
	while (((status = tx->des3) & TDES3_OWN) && count > 0) {
		udelay(1);
		count--;
	}

	/* TX timestamp (TDES2.TTSE = 1): the write-back sets TDES3.TTSS and puts it in TDES0 (ns) / TDES1 (s), p.3054 */
	if (eth_ptp_on) {
		eth_tx_ts.valid = ((status & (TDES3_OWN | TDES3_TTSS)) == TDES3_TTSS);
		eth_tx_ts.ns = tx->des0;
		eth_tx_ts.s = tx->des1;
	}

	if (eth_flow_trace) {
		FLOW("[10] Poll done after %d us: TDES3 write-back = 0x%08lX (OWN=%d ES=%d)\n",
			 ETH_TEST_TX_TIMEOUT_US - count, (unsigned long)status,
			 (status & TDES3_OWN) != 0, (status & TDES3_ES) != 0);
		flow_fields(FF_TDES3_WB, status);
		if (FLOW_LOG >= 2) {
			FLOW2("[10] TDES0=0x%08lX TDES1=0x%08lX TDES2=0x%08lX (after write-back; TDES0/1 = timestamp if TTSS=1)\n",
				  (unsigned long)tx->des0, (unsigned long)tx->des1, (unsigned long)tx->des2);
			flow_dma_state(0);
			flow_mtl_state(0);
			flow_mac_state(0);
		}
	}

	if (status & TDES3_OWN) {
		eth_printf("TX: timeout, TDES3 = 0x%08lX, DMA_CH0_Status = 0x%08lX\n", (unsigned long)status,
				   (unsigned long)AX_BASE_READ_REG32(mac_base, R_DMA_CH0_STATUS));
		return -3;
	}
	if (status & TDES3_ES) {
		eth_printf("TX: error, TDES3 = 0x%08lX\n", (unsigned long)status);
		return -4;
	}
	ETH_DEBUG_PRINTF("End Egress: TDES3 = 0x%08lX after ~%d us\r\n", (unsigned long)status,
					 ETH_TEST_TX_TIMEOUT_US - count);
	return len;
}
/* @brief TX context descriptor (RM0399 p.3057-3061): "insert VLAN tag `tag` in the frames that follow".
 * Uses tx_desc[eth_tx_head] like a frame but with no buffer; the DMA reads it, keeps the tag, and
 * write-backs OWN = 0 (CDE [23] = 1 if the descriptor was in a wrong place). The tail pointer is not
 * moved here: egress_packet() of the next frame moves it past both descriptors. The frame must set
 * TDES2.VTIR = insert (eth_tx_vtir) and MAC_VLAN_Incl.VLTI must be 1 (tag from the descriptor).
 *
 *   Descriptor             Bit [pos]      Value           Function
 *   ---------------------- -------------- --------------- ------------------------------
 *   TDES0, TDES1, TDES2    all            0               no buffer, no inner tag, no MSS
 *   TDES3                  OWN [31]       1               given to the DMA
 *                          CTXT [30]      1               context descriptor, not a frame
 *                          VLTV [16]      1               VT is valid
 *                          VT [15:0]      tag             priority [15:13], CFI [12], VID [11:0]
 * @return index of the descriptor used (read tx_desc[i].des3 after the frame: OWN = 0, CDE)
 */
uint32_t egress_context_vlan(uint16_t tag)
{
	uint32_t idx = eth_tx_head;
	struct eth_desc *tx = &tx_desc[idx];

	if (tx->des3 & TDES3_OWN) {
		eth_printf("TX context: tx_desc[%lu] still owned by the DMA\n", (unsigned long)idx);
		return idx;
	}
	tx->des0 = 0;
	tx->des1 = 0;
	tx->des2 = 0;
	ETH_DSB();										// descriptor words before OWN
	tx->des3 = TDES3_OWN | TDES3_CTXT				// OWN [31], CTXT [30]
			 | TDES3_CTXT_VLTV						// VLTV [16]: VT valid
			 | ((uint32_t)tag & TDES3_CTXT_VT_MASK);	// VT [15:0]
	ETH_DSB();
	eth_tx_head = (idx + 1) & (MAC_100M_TX_DESC_CNT - 1);
	FLOW_PKT("[10] tx_desc[%lu] = context descriptor: TDES3=0x%08lX (OWN CTXT VLTV, VT=0x%04X), no tail write yet\n",
			 (unsigned long)idx, (unsigned long)tx->des3, (unsigned int)tag);
	return idx;
}
/* @brief TX context descriptor with two tags (QinQ, RM0399 p.3059-3061): outer tag in TDES3.VT (VLTV),
 * inner tag in TDES2.IVT (IVLTV). The outer tag stays valid until the next context; the inner tag is used
 * only for the frame that immediately follows (RM0399 p.3058). What the MAC does with the inner tag:
 * MAC_Inner_VLAN_Incl.VLP = 1 -> MAC_Inner_VLAN_Incl.VLC for every frame (ivtir ignored); VLP = 0 -> IVTIR of this context.
 * The tag value comes from TDES2.IVT when MAC_Inner_VLAN_Incl.VLTI = 1; the outer tag as in egress_context_vlan().
 *
 *   Descriptor             Bit [pos]      Value           Function
 *   ---------------------- -------------- --------------- ------------------------------
 *   TDES0, TDES1           all            0               no buffer
 *   TDES2                  IVT [31:16]    inner           inner VLAN tag (0x8100 type unless MAC_Inner_VLAN_Incl.CSVL)
 *   TDES3                  OWN, CTXT      1               context descriptor
 *                          IVTIR [19:18]  ivtir           inner tag: 0 nothing, 1 remove, 2 insert, 3 replace (p.3060)
 *                          VLTV [16]      1               VT valid
 *                          IVLTV [17]     1               IVT valid
 *                          VT [15:0]      outer           outer VLAN tag (0x88A8 type if MAC_VLAN_Incl.CSVL)
 * @return index of the descriptor used
 */
uint32_t egress_context_qinq(uint16_t outer, uint16_t inner, uint8_t ivtir)
{
	uint32_t idx = eth_tx_head;
	struct eth_desc *tx = &tx_desc[idx];

	if (tx->des3 & TDES3_OWN) {
		eth_printf("TX context: tx_desc[%lu] still owned by the DMA\n", (unsigned long)idx);
		return idx;
	}
	tx->des0 = 0;
	tx->des1 = 0;
	tx->des2 = ((uint32_t)inner << TDES2_CTXT_IVT_bit);	// IVT [31:16]: inner tag
	ETH_DSB();
	tx->des3 = TDES3_OWN | TDES3_CTXT				// OWN [31], CTXT [30]
			 | TDES3_CTXT_VLTV						// VLTV [16]: VT valid
			 | TDES3_CTXT_IVLTV						// IVLTV [17]: IVT valid
			 | ((uint32_t)ivtir << TDES3_CTXT_IVTIR_bit)	// IVTIR [19:18]: inner tag action when MAC_Inner_VLAN_Incl.VLP = 0
			 | ((uint32_t)outer & TDES3_CTXT_VT_MASK);	// VT [15:0]: outer tag
	ETH_DSB();
	eth_tx_head = (idx + 1) & (MAC_100M_TX_DESC_CNT - 1);
	FLOW_PKT("[10] tx_desc[%lu] = context descriptor QinQ: TDES2=0x%08lX (IVT=0x%04X) TDES3=0x%08lX (VT=0x%04X, IVTIR=%u)\n",
			 (unsigned long)idx, (unsigned long)tx->des2, (unsigned int)inner, (unsigned long)tx->des3, (unsigned int)outer,
			 (unsigned int)ivtir);
	return idx;
}
/* @brief A WRONG context descriptor on purpose (RM0399 p.3060, CDE causes): CTXT together with FD and LD.
 * Unlike the other egress_context_*() this one writes the tail pointer and waits, because nothing follows it:
 * the DMA must read it alone and close it with CDE [23] = 1 (and DMA_CH0_Status.CDE [13], an abnormal event).
 * After it, RM0399 says the TX DMA is only recovered by a software reset: use it as the last thing of a test.
 *
 *   Descriptor             Bit [pos]      Value           Function
 *   ---------------------- -------------- --------------- ------------------------------
 *   TDES0, TDES1, TDES2    all            0               no buffer
 *   TDES3                  OWN, CTXT      1               context descriptor ...
 *                          FD [29], LD [28] 1             ... that also claims to be a one-descriptor frame: invalid
 *                          VLTV [16], VT  1, tag          a tag, so the context is otherwise well formed
 *   DMA_CH0_TxDesc_Tail_Pointer         TDT            &tx_desc[next]  tail past the descriptor: the DMA reads it now
 * @return TDES3 after the DMA closed it (OWN = 0 expected, CDE [23] = 1 expected); OWN still 1 = timeout
 */
uint32_t egress_context_bad(uint16_t tag)
{
	uint32_t mac_base = MAC_BASE_ADRS(0);
	uint32_t idx = eth_tx_head;
	struct eth_desc *tx = &tx_desc[idx];
	uint32_t status;
	int count = ETH_TEST_TX_TIMEOUT_US;

	if (tx->des3 & TDES3_OWN) {
		eth_printf("TX context: tx_desc[%lu] still owned by the DMA\n", (unsigned long)idx);
		return tx->des3;
	}
	tx->des0 = 0;
	tx->des1 = 0;
	tx->des2 = 0;
	ETH_DSB();
	tx->des3 = TDES3_OWN | TDES3_CTXT				// OWN [31], CTXT [30]
			 | TDES3_FD | TDES3_LD					// FD [29], LD [28]: not allowed with CTXT -> CDE
			 | TDES3_CTXT_VLTV						// VLTV [16]: VT valid
			 | ((uint32_t)tag & TDES3_CTXT_VT_MASK);	// VT [15:0]
	ETH_DSB();
	eth_tx_head = (idx + 1) & (MAC_100M_TX_DESC_CNT - 1);
	FLOW_PKT("[10] tx_desc[%lu] = BAD context descriptor: TDES3=0x%08lX (OWN CTXT FD LD VLTV), tail = &tx_desc[%lu]\n",
			 (unsigned long)idx, (unsigned long)tx->des3, (unsigned long)eth_tx_head);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_TXDESC_TAIL_POINTER, (uint32_t)(uintptr_t)&tx_desc[eth_tx_head]);
	while (((status = tx->des3) & TDES3_OWN) && count > 0) {
		udelay(1);
		count--;
	}
	return status;
}
/* @brief TX context descriptor "MSS = mss" (RM0399 p.2995, p.3059-3060): the DMA stores the value and uses
 * it for every frame sent with TDES3.TSE = 1 until the next context (DMA_CH0_Control.MSS is the other way to
 * give it; the last one written wins). Ring usage as egress_context_vlan(): no tail write here.
 *
 *   Descriptor             Bit [pos]      Value           Function
 *   ---------------------- -------------- --------------- ------------------------------
 *   TDES0, TDES1           all            0               no buffer
 *   TDES2                  MSS [13:0]     mss             payload bytes per segment
 *   TDES3                  OWN, CTXT      1               context descriptor
 *                          TCMSSV [26]    1               MSS valid (OSTC [27] = 0: not a timestamp correction)
 * @return index of the descriptor used
 */
uint32_t egress_context_mss(uint16_t mss)
{
	uint32_t idx = eth_tx_head;
	struct eth_desc *tx = &tx_desc[idx];

	if (tx->des3 & TDES3_OWN) {
		eth_printf("TX context: tx_desc[%lu] still owned by the DMA\n", (unsigned long)idx);
		return idx;
	}
	tx->des0 = 0;
	tx->des1 = 0;
	tx->des2 = ((uint32_t)mss & TDES2_CTXT_MSS_MASK);	// MSS [13:0]
	ETH_DSB();
	tx->des3 = TDES3_OWN | TDES3_CTXT				// OWN [31], CTXT [30]
			 | TDES3_CTXT_TCMSSV;					// TCMSSV [26]: MSS valid, OSTC [27] = 0
	ETH_DSB();
	eth_tx_head = (idx + 1) & (MAC_100M_TX_DESC_CNT - 1);
	FLOW_PKT("[10] tx_desc[%lu] = context descriptor MSS: TDES2=0x%08lX (MSS=%u) TDES3=0x%08lX (OWN CTXT TCMSSV), no tail write yet\n",
			 (unsigned long)idx, (unsigned long)tx->des2, (unsigned int)mss, (unsigned long)tx->des3);
	return idx;
}
/* @brief TX context descriptor "one-step timestamp" (RM0399 p.2983-2985, p.3058-3060): the MAC writes the
 * system time into the PTP packet that follows (originTimestamp of a Sync in master mode, Table 564) instead
 * of only reporting it in TDES0/1. TDES0/1 carry the ingress timestamp a transparent clock needs for the
 * correctionField; a master does not use them (0). Ring usage as egress_context_vlan(): no tail write here.
 *
 *   Descriptor             Bit [pos]      Value           Function
 *   ---------------------- -------------- --------------- ------------------------------
 *   TDES0                  TTSL [31:0]    ts_lo           ingress timestamp, low word (transparent clock only)
 *   TDES1                  TTSH [31:0]    ts_hi           ingress timestamp, high word
 *   TDES2                  all            0               no MSS (TDES2 is the timestamp input when OSTC = 1)
 *   TDES3                  OWN, CTXT      1               context descriptor
 *                          OSTC [27]      1               one-step timestamp correction on
 *                          TCMSSV [26]    1               with OSTC: TDES0/1 valid
 * @return index of the descriptor used
 */
uint32_t egress_context_ost(uint32_t ts_lo, uint32_t ts_hi)
{
	uint32_t idx = eth_tx_head;
	struct eth_desc *tx = &tx_desc[idx];

	if (tx->des3 & TDES3_OWN) {
		eth_printf("TX context: tx_desc[%lu] still owned by the DMA\n", (unsigned long)idx);
		return idx;
	}
	tx->des0 = ts_lo;								// TTSL: ingress timestamp low (0 for a master)
	tx->des1 = ts_hi;								// TTSH: ingress timestamp high
	tx->des2 = 0;
	ETH_DSB();
	tx->des3 = TDES3_OWN | TDES3_CTXT				// OWN [31], CTXT [30]
			 | TDES3_CTXT_OSTC						// OSTC [27]: one-step timestamp correction
			 | TDES3_CTXT_TCMSSV;					// TCMSSV [26]: with OSTC, TDES0/1 are valid
	ETH_DSB();
	eth_tx_head = (idx + 1) & (MAC_100M_TX_DESC_CNT - 1);
	FLOW_PKT("[10] tx_desc[%lu] = context descriptor one-step: TDES0=0x%08lX TDES1=0x%08lX TDES3=0x%08lX (OWN CTXT OSTC TCMSSV), no tail write yet\n",
			 (unsigned long)idx, (unsigned long)tx->des0, (unsigned long)tx->des1, (unsigned long)tx->des3);
	return idx;
}
/* @brief Send one TCP/IP frame with TCP Segmentation Offload (RM0399 §61.5.6 p.2991-2997, §61.9.13 p.3045):
 * the DMA cuts the payload into MSS-byte segments (MSS from egress_context_mss() or DMA_CH0_Control.MSS) and
 * sends each one behind a copy of the header, with IP length/ID/checksum and TCP seq/flags/checksum
 * fixed (Table 567 p.2994). One descriptor: header in buffer 1, payload in buffer 2 (p.2996: buffer 1
 * holds no payload byte). DMA_CH0_Tx_Control.TSE must be 1. The frame buffer is the caller's (DMA reads it).
 * Same tail write and OWN poll as egress_packet(); the DMA closes the descriptor after the last segment.
 *
 *   Descriptor             Bit [pos]      Value           Function
 *   ---------------------- -------------- --------------- ------------------------------
 *   TDES0 p.3049           BUF1AP [31:0]  frame           header address (Ethernet + IP + TCP)
 *   TDES1                  BUF2AP [31:0]  frame + hdr_len payload address
 *   TDES2 p.3051           HL [9:0]       hdr_len         header length (B1L field when TSE = 1)
 *                          B2L [29:16]    pay_len         payload bytes in buffer 2
 *   TDES3 p.3052           OWN, FD, LD    1               one descriptor for the whole frame
 *                          TSE [18]       1               segment this frame
 *                          THL [22:19]    thl             TCP header length in 32-bit words (>= 5)
 *                          TPL [17:0]     pay_len         TCP payload length (replaces FL and CIC)
 * @return hdr_len + pay_len, or -1 bad length, -2 descriptor busy, -3 timeout, -4 TX error
 */
int egress_tso(const unsigned char *frame, int hdr_len, int pay_len, int thl)
{
	uint32_t mac_base = MAC_BASE_ADRS(0);	// STM32H755: one MAC
	uint32_t idx = eth_tx_head;				// next free ring entry (the context descriptor, if any, is just before)
	struct eth_desc *tx = &tx_desc[idx];
	uint32_t status;
	int count = ETH_TEST_TX_TIMEOUT_US;

	/* 1. limits of the two length fields: HL has 10 bits (header <= 1023), B2L 14 bits (payload <= 16383 in
	 *    one buffer; more needs extra descriptors, not supported here) */
	if (hdr_len <= 0 || hdr_len > (int)TDES2_HL_MASK || pay_len <= 0 || pay_len > (int)TDES2_B2L_MASK) {
		eth_printf("TX TSO: bad length, header %d payload %d\n", hdr_len, pay_len);
		return -1;
	}
	/* 2. never overwrite a descriptor the DMA still owns (after a TX timeout) */
	if (tx->des3 & TDES3_OWN) {
		eth_printf("TX TSO: tx_desc[%lu] still owned by the DMA\n", (unsigned long)idx);
		return -2;
	}
	/* 3. the two buffers. No memcpy: the DMA reads the caller's frame in place
	 *      frame:  [ ETH | IP | TCP ][ payload ........ ]
	 *               ^ TDES0           ^ TDES1 = frame + hdr_len
	 *    RM0399 p.2996: the header must be alone in buffer 1 of the first descriptor, the payload starts in
	 *    buffer 2. The DMA reads buffer 1 again for every segment, buffer 2 once, MSS bytes at a time */
	tx->des0 = (uint32_t)(uintptr_t)frame;					// TDES0: buffer 1 = header
	tx->des1 = (uint32_t)(uintptr_t)(frame + hdr_len);		// TDES1: buffer 2 = payload
	/* 4. TDES2: with TSE = 1 the B1L field means "header length" (p.3051), B2L is the payload length */
	tx->des2 = ((uint32_t)hdr_len & TDES2_HL_MASK)			// HL [9:0]: header length
			 | ((uint32_t)pay_len << TDES2_B2L_bit);			// B2L [29:16]: payload length
	ETH_DSB();												// descriptor words before OWN
	/* 5. TDES3: FD + LD because this one descriptor holds the whole frame. With TSE = 1, bits [17:0] are no
	 *    longer FL + CIC but TPL, the TCP payload length (p.3052-3053): the DMA computes the number of
	 *    segments as TPL / MSS, and the MAC does both checksums itself (no CIC field left to choose).
	 *    THL = TCP header length in 32-bit words: 5 = 20 bytes; >= 5 means TSO, 2 means UDP (USO) */
	tx->des3 = TDES3_OWN | TDES3_FD | TDES3_LD				// OWN [31], FD [29], LD [28]
			 | TDES3_TSE									// TSE [18]: segment this frame
			 | ((uint32_t)thl << TDES3_THL_bit)				// THL [22:19]: TCP header in 32-bit words
			 | ((uint32_t)pay_len & TDES3_TPL_MASK);			// TPL [17:0]: TCP payload length
	ETH_DSB();												// OWN before the tail pointer write
	eth_tx_head = (idx + 1) & (MAC_100M_TX_DESC_CNT - 1);
	if (eth_flow_trace) {
		FLOW_STEP("[10]", "Send 1 TSO frame (TX)");
		FLOW("[10] tx_desc[%lu]: TDES0=0x%08lX (header) TDES1=0x%08lX (payload) TDES2=0x%08lX (HL=%d B2L=%d)\n",
			 (unsigned long)idx, (unsigned long)tx->des0, (unsigned long)tx->des1, (unsigned long)tx->des2, hdr_len, pay_len);
		FLOW("[10] TDES3=0x%08lX (OWN FD LD TSE, THL=%d words, TPL=%d bytes): the DMA cuts TPL into MSS pieces\n",
			 (unsigned long)tx->des3, thl, pay_len);
		FLOW("[10] Write TX tail = &tx_desc[%lu], then poll OWN: OWN = 0 only after the last segment\n", (unsigned long)eth_tx_head);
	}
	/* 6. tail = the entry after this one: the DMA wakes up and walks every owned descriptor before the tail,
	 *    so a context descriptor written just before (egress_context_mss) is read first, then this frame */
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_TXDESC_TAIL_POINTER, (uint32_t)(uintptr_t)&tx_desc[eth_tx_head]);

	/* 7. wait for OWN = 0. For a TSO frame the DMA (p.2993 steps 3-10) reads the header, fixes it, pushes it
	 *    to the MTL queue, pushes MSS payload bytes, and repeats for every segment; it writes the descriptor
	 *    back only after the LAST segment. So this poll covers all the segments, not just the first */
	while (((status = tx->des3) & TDES3_OWN) && count > 0) {
		udelay(1);
		count--;
	}
	if (eth_flow_trace) {
		FLOW("[10] Poll done after %d us: TDES3 write-back = 0x%08lX (OWN=%d ES=%d)\n",
			 ETH_TEST_TX_TIMEOUT_US - count, (unsigned long)status,
			 (status & TDES3_OWN) != 0, (status & TDES3_ES) != 0);
		flow_fields(FF_TDES3_WB, status);
	}
	/* 8. result: the write-back does not say how many segments were made; the caller counts the frames
	 *    received and the MMC TX good counter for that */
	if (status & TDES3_OWN) {
		eth_printf("TX TSO: timeout, TDES3 = 0x%08lX, DMA_CH0_Status = 0x%08lX\n", (unsigned long)status,
				   (unsigned long)AX_BASE_READ_REG32(mac_base, R_DMA_CH0_STATUS));
		return -3;
	}
	if (status & TDES3_ES) {
		eth_printf("TX TSO: error, TDES3 = 0x%08lX\n", (unsigned long)status);
		return -4;
	}
	return hdr_len + pay_len;
}
/* @brief generate ethernet with total 64 byte len.
 * @param tx_buffer: tx-buff
 * @param dest_mac: destination mac.
 * @param src_mac: src mac in frame
 * @return len of frame including hdrs.
 */
int generate_ethernet_packet(unsigned char *tx_buffer, unsigned char *dest_mac, unsigned char *src_mac,
							 int payload_len)
{
	int fit;
	unsigned short ether_type = ETH_TEST_ETHER_TYPE;

	FLOW_STEP("[9]", "Build the test frame");
	memcpy(tx_buffer, dest_mac, ETH_TEST_MAC_SIZE);
	memcpy(tx_buffer + ETH_TEST_MAC_SIZE, src_mac, ETH_TEST_MAC_SIZE);
	tx_buffer[12] = (ether_type >> 8) & 0xFF;
	tx_buffer[13] = ether_type & 0xFF;

	payload_len = (payload_len < ETH_TEST_MIN_PAYLOAD) ? ETH_TEST_MIN_PAYLOAD : payload_len;
	payload_len = (payload_len > ETH_TEST_MAX_PAYLOAD) ? ETH_TEST_MAX_PAYLOAD : payload_len;

	/*Align to be a 4-Byte boundary (h/w bug of the Kirkwood host FIFO). Not needed by EQoS,
	  kept so both platforms send the same frames */
	fit = (ETH_TEST_HEADER_SIZE + payload_len) % 4;
	payload_len = payload_len - fit;

	for (int i = 0; i < payload_len; i++) {
		tx_buffer[ETH_TEST_HEADER_SIZE + i] = ETH_TEST_TEST_TRAFFIC_PATTERN+i;
	}

	eth_printf("Ethernet packet generated successfully.\n");
	FLOW("[9] Frame %d bytes: DA %02X:%02X:%02X:%02X:%02X:%02X | SA %02X:%02X:%02X:%02X:%02X:%02X | EtherType 0x%04X | payload %d bytes (0x%02X 0x%02X ...)\n",
		 payload_len + ETH_TEST_HEADER_SIZE,
		 tx_buffer[0], tx_buffer[1], tx_buffer[2], tx_buffer[3], tx_buffer[4], tx_buffer[5],
		 tx_buffer[6], tx_buffer[7], tx_buffer[8], tx_buffer[9], tx_buffer[10], tx_buffer[11],
		 ether_type, payload_len, tx_buffer[14], tx_buffer[15]);
	flow_frame_dump("[9] Frame to send (pad and FCS are added by the MAC)", tx_buffer,
					payload_len + ETH_TEST_HEADER_SIZE, payload_len);
	return payload_len + ETH_TEST_HEADER_SIZE;
}
/**
 * @brief Traffic test: build 1 test frame, then pkt_cnt times: send it (egress_packet),
 * receive it (ingress_packet), compare byte by byte. Stops at the first error.
 * No register bit set here: egress_packet() / ingress_packet_status() write the descriptors
 * (TDES / RDES) and the tail pointers.
 * @return 1: every frame came back the same, 0: else
 */
uint16_t eth_test_traffic(int8_t mac_idx, uint16_t pkt_cnt)
{
    uint16_t status = 0;
	uint16_t i = 0;
	int frame_len, tx_len, rx_len = 0;
	/* static: 2 x MAC_100M_BUF_SIZE is too big for the stack */
	static unsigned char txbuff[ETH_TEST_TRAFFIC_MAX_BUFFER];
	static unsigned char rxbuff[ETH_TEST_TRAFFIC_MAX_BUFFER];
	unsigned char dest_mac[ETH_TEST_MAC_SIZE] = ETH_TEST_DEST_MAC;
	unsigned char src_mac[ETH_TEST_MAC_SIZE] = ETH_TEST_SRC_MAC;
	frame_len = generate_ethernet_packet(txbuff, dest_mac, src_mac, ETH_TEST_TRAFFIC_SIZE);

	eth_printf("\nStart Egress and Ingress packet(s): %d\n", pkt_cnt);
	FLOW("[10/11] Frames 1..%d step by step, then one \".\" per frame\n", FLOW_PKT_MAX);

	for (i = 0; i < pkt_cnt; i++) {
		status = 0;
		tx_len = rx_len = 0;
		eth_flow_trace = (i < FLOW_PKT_MAX);	// [10] / [11] FLOW lines for this frame
		tx_len = egress_packet(txbuff, frame_len);
		if (tx_len == frame_len) {
			if ((rx_len = ingress_packet(mac_idx, rxbuff, ETH_TEST_TRAFFIC_MAX_BUFFER)) < 0) {
				break;
			}
			if (compare_packets(rxbuff, rx_len, txbuff, tx_len)) {
				status = 1;
				FLOW_PKT("\n[11] Compare with the frame sent (%d bytes): SAME -> loopback OK\n", tx_len);
				if (eth_flow_trace) {
					flow_frame_dump("[11] rxbuff (copy of rx_buf)", rxbuff, rx_len, frame_len - ETH_TEST_HEADER_SIZE);
				}
			} else {
				eth_printf("\nPacket %d: RX (%d bytes) differs from TX (%d bytes)\n", i, rx_len, tx_len);
				flow_frame_dump("TX frame", txbuff, tx_len, frame_len - ETH_TEST_HEADER_SIZE);
				flow_frame_dump("RX frame", rxbuff, rx_len, frame_len - ETH_TEST_HEADER_SIZE);
				break;
			}
		} else {
			break;
		}
	}
	eth_flow_trace = 0;
	eth_printf("\n");
	ETH_PRINTF_L1("Traffic test finished. Tx/Rx Stats: Actual/Expected %d/%d", i, pkt_cnt);
	return status;
}

/**
 * Reset the MAC to re-init MAC (RM0399 §61.9.1 step 1-2, p.3031)
 *
 *   Register               Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   DMA_Mode p.3074       SWR [0]              1       reset DMA, MTL and MAC: every register back to
 *                                                       its reset value; the hardware clears SWR when done
 *   MAC_1US_Tic_Counter p.3140   TIC_1US_CNTR [11:0]  223     1 us tick of the MAC = CSR clock (HCLK) in MHz - 1
 *
 * The reset needs the RMII REF_CLK (50 MHz from the PHY on PA1): no clock -> SWR stays 1 -> timeout.
 * Return 1: reset done, 0: timeout
 */
uint8_t mac_reset_100m(uint8_t mac_idx) {
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t regval, ustcr;

	FLOW_STEP("[4]", "Read version, software reset DMA/MTL/MAC, 1 us tick");
	eth_printf("MAC version: 0x%08lX\n", (unsigned long)AX_BASE_READ_REG32(mac_base, R_MAC_VERSION));

	regval = AX_BASE_READ_REG32(mac_base, R_DMA_MODE);
	// print BEFORE the write, so the UART time is not counted in the SWR time
	FLOW("[4] Write DMA_Mode.SWR = 1 (software reset DMA/MTL/MAC), wait for the hardware to clear it...\n");
	flow_reg2("DMA_Mode (before SWR)", mac_base, R_DMA_MODE, FF_DMA_MODE);

	AX_BASE_WRITE_REG32(mac_base, R_DMA_MODE, regval | DMA_MODE_SWR); //Reset MAC

	int timeout = 100000;	// ~100 ms
	do {
		regval = AX_BASE_READ_REG32(mac_base, R_DMA_MODE);
		if ((regval & DMA_MODE_SWR) == 0) {
			break;  // SWR Clear <=> Reset Done
		}
		udelay(1);
		timeout--;
	} while (timeout > 0);

	if (timeout == 0) {
		eth_printf("MAC SW_RESET timeout! Check the 50 MHz REF_CLK on PA1\n");
		return 0;
	}
	eth_printf("MAC Reset done.\n");
	FLOW("[4] SWR cleared after ~%d us -> reset done\n", 100000 - timeout);
	flow_reg2("DMA_Mode (after SWR)", mac_base, R_DMA_MODE, FF_DMA_MODE);

	/* 1 us tick of the MAC: HCLK 224 MHz -> 223 */
	ustcr = eth_csr_clock_hz() / 1000000 - 1;
	AX_BASE_WRITE_REG32(mac_base, R_MAC_1US_TIC_COUNTER, ustcr);
	flow_regw("MAC_1US_Tic_Counter", mac_base, R_MAC_1US_TIC_COUNTER, ustcr, FF_NONE);
	return 1;
}
/**
 * DMA channel 0 (RM0399 §61.9.1 step 3-8, p.3031-3032): TX/RX control, burst, bus mode, no interrupt.
 * ST/SR stay 0: mac_start() starts the DMA after the rings exist
 * (the DMA takes the descriptor list address when ST/SR go 0 -> 1, p.3083).
 *
 *   Register               Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   DMA_CH0_Tx_Control p.3079    OSF [4]              1       TX DMA fetches the next frame before the status
 *                                                       of the last one comes back
 *                          TXPBL [21:16]        8       max TX burst = 8 x 8 (PBLX8) = 64 beats
 *                          ST [0]               0       TX DMA still stopped
 *   DMA_CH0_Rx_Control p.3081    RBSZ [14:1]          2304    RX buffer size in bytes (MAC_100M_BUF_SIZE)
 *                          RXPBL [21:16]        8       max RX burst = 64 beats
 *                          SR [0]               0       RX DMA still stopped
 *   DMA_CH0_Control p.3078      PBLX8 [16]           1       TXPBL / RXPBL are multiplied by 8
 *                          DSL [20:18]          0       no gap between descriptors (16 bytes each)
 *   DMA_SysBus_Mode p.3076     FB [0]               1       fixed burst length on the bus
 *                          AAL [12]             1       bursts start on an address aligned to the burst
 *   DMA_CH0_Interrupt_Enable p.3087     all                  0       no DMA interrupt: the test polls OWN
 */
// MARK: 02: mac_dma_init

void mac_dma_init(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t dmactxcr;
	uint32_t dmacrxcr;
	uint32_t dmaccr;
	uint32_t dmasbmr;
	uint32_t dmacier;

	/* ST = 0 here: mac_start() starts the TX DMA */
	dmactxcr = (DMA_CH0_TX_CONTROL_TXPBL_8 << DMA_CH0_TX_CONTROL_TXPBL_bit)		// TXPBL [21:16]: 8 beats
			 | DMA_CH0_TX_CONTROL_OSF;									// OSF [4]: operate on second packet
	/* SR = 0 here: mac_start() starts the RX DMA. RBSZ holds bytes but starts at bit 1 */
	dmacrxcr = (DMA_CH0_RX_CONTROL_RXPBL_8 << DMA_CH0_RX_CONTROL_RXPBL_bit)		// RXPBL [21:16]: 8 beats
			 | (MAC_100M_BUF_SIZE << DMA_CH0_RX_CONTROL_RBSZ_bit);		// RBSZ [14:1]: 2304-byte RX buffer
	dmaccr = DMA_CH0_CONTROL_PBLX8										// PBLX8 [16]: TXPBL/RXPBL x 8 = 64 beats
		   | (DMA_CH0_CONTROL_DSL_0 << DMA_CH0_CONTROL_DSL_bit);					// DSL [20:18]: descriptors packed (16 bytes)
	/* AHB bus on STM32H755 */
	dmasbmr = DMA_SYSBUS_MODE_AAL										// AAL [12]: address-aligned beats
			| DMA_SYSBUS_MODE_FB;										// FB [0]: fixed burst
	dmacier = 0U;												// all interrupts off: the test polls OWN

	FLOW_STEP("[5]", "DMA channel 0: TX control, RX control, channel control, bus mode");
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_TX_CONTROL, dmactxcr);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_RX_CONTROL, dmacrxcr);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_CONTROL, dmaccr);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_SYSBUS_MODE, dmasbmr);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_INTERRUPT_ENABLE, dmacier);

	FLOW("[5] DMA channel 0 (read back after the writes):\n");
	flow_regw("DMA_CH0_Tx_Control", mac_base, R_DMA_CH0_TX_CONTROL, dmactxcr, FF_DMA_CH0_TX_CONTROL);
	flow_regw("DMA_CH0_Rx_Control", mac_base, R_DMA_CH0_RX_CONTROL, dmacrxcr, FF_DMA_CH0_RX_CONTROL);
	flow_regw("DMA_CH0_Control", mac_base, R_DMA_CH0_CONTROL, dmaccr, FF_DMA_CH0_CONTROL);
	flow_regw("DMA_SysBus_Mode", mac_base, R_DMA_SYSBUS_MODE, dmasbmr, FF_DMA_SYSBUS_MODE);
	flow_regw("DMA_CH0_Interrupt_Enable", mac_base, R_DMA_CH0_INTERRUPT_ENABLE, dmacier, FF_NONE);
}
/**
 * Descriptor rings (RM0399 §61.9.1 step 4-6, p.3031; descriptors §61.10, p.3046)
 * TX: every descriptor owned by the CPU (OWN = 0), nothing to send.
 * RX: every descriptor points to its empty buffer and is given to the DMA (OWN = 1, BUF1V = 1).
 * The DMA is still stopped: DLAR may only be written while ST/SR = 0 (p.3083/3084).
 *
 *   Descriptor / register  Bit [pos]            Value         Function
 *   ---------------------- -------------------- ------------- ------------------------------
 *   TDES0..3 (each TX)     all                  0             OWN = 0: owned by the CPU, nothing to send
 *   RDES0 (each RX)        BUF1AP [31:0]        &rx_buf[i]    address of the empty buffer
 *   RDES3 (each RX)        OWN [31]             1             given to the DMA
 *                          BUF1V [24]           1             RDES0 holds a valid address
 *                          IOC [30]             0             no interrupt when a frame arrives
 *   DMA_CH0_TxDesc_List_Address p.3083  TDESLA [31:2]        &tx_desc[0]   start of the TX ring
 *   DMA_CH0_TxDesc_Ring_Length p.3086   TDRL [9:0]           3             TX ring length - 1 (MAC_100M_TX_DESC_CNT 4)
 *   DMA_CH0_RxDesc_List_Address p.3084  RDESLA [31:2]        &rx_desc[0]   start of the RX ring
 *   DMA_CH0_Rx_Control2 p.3087   RDRL [9:0]           7             RX ring length - 1 (MAC_100M_RX_DESC_CNT 8)
 *   DMA_CH0_TxDesc_Tail_Pointer p.3085  TDT [31:2]           &tx_desc[0]   TX tail = first: nothing to send yet
 *   DMA_CH0_RxDesc_Tail_Pointer p.3086  RDT [31:2]           &rx_desc[7]   RX tail = last: the RX ring is ready
 */
// MARK: 03: mac_desc_init

void mac_desc_init(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t i;

	FLOW_STEP("[6]", "Descriptors, register the rings with the DMA");
	for (i = 0; i < MAC_100M_TX_DESC_CNT; i++) {
		tx_desc[i].des0 = 0;
		tx_desc[i].des1 = 0;
		tx_desc[i].des2 = 0;
		tx_desc[i].des3 = 0;
	}
	for (i = 0; i < MAC_100M_RX_DESC_CNT; i++) {
		rx_desc[i].des0 = (uint32_t)(uintptr_t)rx_buf[i];	// RDES0: buffer 1 address
		rx_desc[i].des1 = 0;
		rx_desc[i].des2 = 0;
		ETH_DSB();											// address before OWN
		rx_desc[i].des3 = RDES3_OWN | RDES3_BUF1V;			// give to the DMA
	}
	ETH_DSB();
	// addresses must be in RAM_D1 0x2400xxxx (reachable by the Ethernet DMA)
	FLOW("[6] tx_desc @0x%08lX  rx_desc @0x%08lX  (16 bytes/descriptor, TX %d, RX %d)\n",
		 (unsigned long)(uintptr_t)tx_desc, (unsigned long)(uintptr_t)rx_desc, MAC_100M_TX_DESC_CNT, MAC_100M_RX_DESC_CNT);
	FLOW("[6] tx_buf  @0x%08lX  rx_buf  @0x%08lX  (%d bytes/buffer)\n",
		 (unsigned long)(uintptr_t)tx_buf, (unsigned long)(uintptr_t)rx_buf, MAC_100M_BUF_SIZE);
	FLOW("[6] rx_desc[0]: des0=0x%08lX (= rx_buf[0])  des3=0x%08lX (OWN|BUF1V)\n",
		 (unsigned long)rx_desc[0].des0, (unsigned long)rx_desc[0].des3);
	flow_fields(FF_RDES3_RD, rx_desc[0].des3);

	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_TXDESC_LIST_ADDRESS, (uint32_t)(uintptr_t)&tx_desc[0]);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_TXDESC_RING_LENGTH, MAC_100M_TX_DESC_CNT - 1);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_RXDESC_LIST_ADDRESS, (uint32_t)(uintptr_t)&rx_desc[0]);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_RX_CONTROL2, MAC_100M_RX_DESC_CNT - 1);
	/* TX tail = first descriptor: nothing to send yet */
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_TXDESC_TAIL_POINTER, (uint32_t)(uintptr_t)&tx_desc[0]);
	/* RX tail = last descriptor: the whole RX ring is ready */
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_RXDESC_TAIL_POINTER, (uint32_t)(uintptr_t)&rx_desc[MAC_100M_RX_DESC_CNT - 1]);

	eth_tx_head = 0;
	eth_rx_head = 0;

	FLOW("[6] Ring registers:\n");
	flow_reg("DMA_CH0_TxDesc_List_Address (&tx_desc[0])", mac_base, R_DMA_CH0_TXDESC_LIST_ADDRESS, FF_NONE);
	flow_reg("DMA_CH0_TxDesc_Ring_Length (count - 1)", mac_base, R_DMA_CH0_TXDESC_RING_LENGTH, FF_NONE);
	flow_reg("DMA_CH0_RxDesc_List_Address (&rx_desc[0])", mac_base, R_DMA_CH0_RXDESC_LIST_ADDRESS, FF_NONE);
	flow_reg("DMA_CH0_Rx_Control2 (count - 1)", mac_base, R_DMA_CH0_RX_CONTROL2, FF_NONE);
	flow_reg("DMA_CH0_TxDesc_Tail_Pointer (TX tail)", mac_base, R_DMA_CH0_TXDESC_TAIL_POINTER, FF_NONE);
	flow_reg("DMA_CH0_RxDesc_Tail_Pointer (RX tail)", mac_base, R_DMA_CH0_RXDESC_TAIL_POINTER, FF_NONE);
	flow_dma_state(mac_idx);
	flow_rings(mac_idx, "after init");
}
/**
 * MTL Tx/Rx queue 0, the FIFOs between DMA and MAC (RM0399 §61.9.2, p.3032)
 *
 *   Register               Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   MTL_TxQ0_Operation_Mode p.3101   TQS [18:16]          7       TX queue size (7 + 1) x 256 = 2048 bytes
 *                          TXQEN [3:2]          2       TX queue 0 enabled
 *                          TSF [1]              0       threshold mode: the MAC starts sending when
 *                          TTC [6:4]            0       32 bytes are in the queue
 *   MTL_RxQ0_Operation_Mode p.3106   RQS [22:20]          7       RX queue size 2048 bytes (read-only)
 *                          DIS_TCP_EF [6]       1       do not drop frames with a TCP/IP checksum error
 *                          RSF [5]              0       threshold mode: the DMA starts reading when
 *                          RTC [1:0]            0       64 bytes are in the queue
 * TSF = RSF = 0: the 2300-byte test frame is bigger than the 2 KB queue, and store-and-forward
 * needs the whole frame in the queue.
 */
// MARK: 04: mac_mtl_init
void mac_mtl_init(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t txqomr;
	uint32_t rxqomr;

	/* TXQEN is read-only (2) on the H755 single-queue core, but read/write and 0 after reset on a
	 * multi-queue core (Mk): write it anyway. Add MTL_TXQ0_OPERATION_MODE_TSF for store-and-forward (frame <= 2 KB) */
	txqomr = (MTL_TXQ0_OPERATION_MODE_TQS_2KB << MTL_TXQ0_OPERATION_MODE_TQS_bit)			// TQS [18:16]: 2048-byte queue
		   | (MTL_TXQ0_OPERATION_MODE_TTC_32 << MTL_TXQ0_OPERATION_MODE_TTC_bit)			// TTC [6:4]: 32 bytes (TSF = 0)
		   | (MTL_TXQ0_OPERATION_MODE_TXQEN_EN << MTL_TXQ0_OPERATION_MODE_TXQEN_bit);		// TXQEN [3:2]: queue 0 enabled
	/* RQS is read-only, written with its reset value. Add MTL_RXQ0_OPERATION_MODE_RSF for store-and-forward (frame <= 2 KB) */
	rxqomr = (MTL_RXQ0_OPERATION_MODE_RQS_2KB << MTL_RXQ0_OPERATION_MODE_RQS_bit)			// RQS [22:20]: 2048-byte queue
		   | MTL_RXQ0_OPERATION_MODE_DIS_TCP_EF								// DIS_TCP_EF [6]: keep checksum error frames
		   | (MTL_RXQ0_OPERATION_MODE_RTC_64 << MTL_RXQ0_OPERATION_MODE_RTC_bit);			// RTC [1:0]: 64 bytes (RSF = 0)

	FLOW_STEP("[7]", "MTL: TX/RX queue 0 (FIFO between DMA and MAC)");
	AX_BASE_WRITE_REG32(mac_base, R_MTL_TXQ0_OPERATION_MODE, txqomr);
	AX_BASE_WRITE_REG32(mac_base, R_MTL_RXQ0_OPERATION_MODE, rxqomr);

	FLOW("[7] MTL queue 0:\n");
	flow_regw("MTL_TxQ0_Operation_Mode", mac_base, R_MTL_TXQ0_OPERATION_MODE, txqomr, FF_MTL_TXQ0_OPERATION_MODE);
	flow_regw("MTL_RxQ0_Operation_Mode", mac_base, R_MTL_RXQ0_OPERATION_MODE, rxqomr, FF_MTL_RXQ0_OPERATION_MODE);
	flow_mtl_state(mac_idx);
}
/**
 * MAC (RM0399 §61.9.3 step 1-5, p.3032-3033): packet filter, MAC address 0, RX watchdog, MAC_Configuration.
 * TE/RE stay 0 until mac_start().
 *
 *   Register               Bit [pos]            Value       Function
 *   ---------------------- -------------------- ----------- ------------------------------
 *   MAC_Packet_Filter p.3118      PR [0]               1           promiscuous: receive every DA (the test DA is
 *                                                           not the board address)
 *   MAC_Address0_High p.3153     ADDRHI [15:0]        0x0000      address byte 5:4 (AE [31] always reads 1)
 *   MAC_Address0_Low p.3154     ADDRLO [31:0]        0x00E18000  address byte 3:0 -> 00:80:E1:00:00:00
 *   MAC_WD_JB_Timeout p.3120      PWE [8]              1           RX watchdog cut-off set by WTO
 *                          WTO [3:0]            0           cut RX frames after 2048 bytes (1 = 3 KB ...)
 *   MAC_Configuration p.3112       CST [21]             1           strip the FCS of Type frames (EtherType >= 0x600)
 *                          ACS [20]             1           strip pad/FCS of Length frames (< 0x600)
 *                          JE [16]              1           jumbo frames up to 9018 bytes
 *                          FES [14]             1           speed 100 Mbit/s (0 = 10 Mbit/s)
 *                          DM [13]              1           full duplex (needed by the loopback)
 *                          LM [12]              0           set later by enable_mac_internal_lb()
 *                          TE [1], RE [0]       0           transmitter / receiver off until mac_start()
 */
// MARK: 05: mac_basic_init
void mac_basic_init(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	unsigned char addr[ETH_TEST_MAC_SIZE] = ETH_TEST_SRC_MAC;	// board address 00:80:E1:00:00:00 (ST OUI)
	uint32_t macpfr;
	uint32_t maca0hr;
	uint32_t maca0lr;
	uint32_t macwtr;
	uint32_t maccr;

	/* Packet filter: receive every DA (the test DA is not the board address) */
	macpfr = MAC_PACKET_FILTER_PR;											// PR [0]: promiscuous
	/* MAC address 0: byte 0 (first on the wire) in the low bits. AE [31] always reads 1 */
	maca0hr = ((uint32_t)addr[5] << 8)							// ADDRHI [15:8]: byte 5
			| ((uint32_t)addr[4] << 0);							// ADDRHI [7:0]: byte 4
	maca0lr = ((uint32_t)addr[3] << 24)							// ADDRLO [31:24]: byte 3
			| ((uint32_t)addr[2] << 16)							// ADDRLO [23:16]: byte 2
			| ((uint32_t)addr[1] << 8)							// ADDRLO [15:8]: byte 1
			| ((uint32_t)addr[0] << 0);							// ADDRLO [7:0]: byte 0
	/* RX watchdog cut-off, used while MAC_Configuration.WD = 0. MAC_WD_JB_TIMEOUT_WTO_3KB lets the 2300-byte frame pass */
	macwtr = MAC_WD_JB_TIMEOUT_PWE											// PWE [8]: cut-off set by WTO
		   | (MAC_WD_JB_TIMEOUT_WTO_2KB << MAC_WD_JB_TIMEOUT_WTO_bit);					// WTO [3:0]: 2048 bytes
	/* LM is set later by enable_mac_internal_lb(), TE/RE by mac_start() */
	maccr = MAC_CONFIGURATION_CST											// CST [21]: strip FCS of Type frames
		  | MAC_CONFIGURATION_ACS											// ACS [20]: strip pad/FCS of Length frames
		  | MAC_CONFIGURATION_JE											// JE [16]: jumbo, giant 9018 / watchdog 10240 bytes
		  | MAC_CONFIGURATION_FES											// FES [14]: 100 Mbit/s
		  | MAC_CONFIGURATION_DM;											// DM [13]: full duplex (needed by loopback, p.2998)

	FLOW_STEP("[8]", "MAC: filter, address, watchdog, MAC_Configuration; loopback; start (DMA first, MAC last)");
	AX_BASE_WRITE_REG32(mac_base, R_MAC_PACKET_FILTER, macpfr);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_ADDRESS0_HIGH, maca0hr);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_ADDRESS0_LOW, maca0lr);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_WD_JB_TIMEOUT, macwtr);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_CONFIGURATION, maccr);

	FLOW("[8] MAC:\n");
	flow_regw("MAC_Packet_Filter", mac_base, R_MAC_PACKET_FILTER, macpfr, FF_MAC_PACKET_FILTER);
	flow_regw("MAC_Address0_High (byte 5:4)", mac_base, R_MAC_ADDRESS0_HIGH, maca0hr, FF_NONE);	// bit 31 AE reads 1
	flow_regw("MAC_Address0_Low (byte 3:0)", mac_base, R_MAC_ADDRESS0_LOW, maca0lr, FF_NONE);
	flow_regw("MAC_WD_JB_Timeout", mac_base, R_MAC_WD_JB_TIMEOUT, macwtr, FF_MAC_WD_JB_TIMEOUT);
	flow_regw("MAC_Configuration (TE/RE off)", mac_base, R_MAC_CONFIGURATION, maccr, FF_MAC_CONFIGURATION);	// bit 15 reads 1
}
/**
 * @brief Configure the 10/100 EQoS MAC in the RM0399 §61.9 order (p.3031-3033):
 * DMA -> descriptor rings -> MTL -> MAC. Call eth_reset_mac() before.
 * TX/RX stay off until mac_start(); the loopback is set by enable_mac_internal_lb().
 *
 *   Function           Registers written (bits in the note on top of each function)
 *   mac_dma_init()     DMA_CH0_Tx_Control, DMA_CH0_Rx_Control, DMA_CH0_Control, DMA_SysBus_Mode, DMA_CH0_Interrupt_Enable
 *   mac_desc_init()    TDES/RDES, DMA_CH0_TxDesc/RxDesc_List_Address, DMA_CH0_TxDesc_Ring_Length / DMA_CH0_Rx_Control2, DMA_CH0_TxDesc/RxDesc_Tail_Pointer
 *   mac_mtl_init()     MTL_TxQ0_Operation_Mode, MTL_RxQ0_Operation_Mode
 *   mac_basic_init()   MAC_Packet_Filter, MAC_Address0_High/LR, MAC_WD_JB_Timeout, MAC_Configuration
 *
 * @param mac_idx index of the MAC. STM32H755 has one MAC: 0.
 */
// MARK: 01: mac_init_100m
void mac_init_100m(uint8_t mac_idx)
{
	mac_dma_init(mac_idx);
	mac_desc_init(mac_idx);
	mac_mtl_init(mac_idx);
	mac_basic_init(mac_idx);

	ETH_DEBUG_PRINTF("Done mac_init_100m(): %d\r\n", mac_idx);
}
/**
 * Start the DMA, then the MAC (RM0399 §61.9.1 step 9 p.3032, §61.9.3 step 6 p.3033)
 * DMA first: it reads the descriptor list address when ST/SR go 0 -> 1 (p.3083/3084),
 * and the MAC receiver must not run before the RX DMA can take the packets.
 *
 *   Register               Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   DMA_CH0_Tx_Control p.3079    ST [0]               1       start TX DMA: suspended until the TX tail moves
 *   DMA_CH0_Rx_Control p.3081    SR [0]               1       start RX DMA: fetches rx_desc[0], waits for a frame
 *   MAC_Configuration p.3112       TE [1]               1       MAC transmitter on
 *                          RE [0]               1       MAC receiver on
 * Read-modify-write: the other bits keep the values of mac_dma_init() / mac_basic_init().
 */
// MARK: 07: mac_start
void mac_start(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t reg_val;

	reg_val = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_TX_CONTROL);
	reg_val |= DMA_CH0_TX_CONTROL_ST; //Start transmission
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_TX_CONTROL, reg_val);

	reg_val = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_RX_CONTROL);
	reg_val |= DMA_CH0_RX_CONTROL_SR; //Start receive
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_RX_CONTROL, reg_val);

	FLOW("[8] Start DMA: ST = 1 (TX), SR = 1 (RX)\n");
	flow_reg("DMA_CH0_Tx_Control", mac_base, R_DMA_CH0_TX_CONTROL, FF_DMA_CH0_TX_CONTROL);
	flow_reg("DMA_CH0_Rx_Control", mac_base, R_DMA_CH0_RX_CONTROL, FF_DMA_CH0_RX_CONTROL);
	// TX: nothing to send yet -> suspended; RX: rx_desc[0] owned by the DMA -> waiting for a packet
	flow_reg("DMA_Debug_Status0 (TPS/RPS)", mac_base, R_DMA_DEBUG_STATUS0, FF_NONE);
	flow_dma_state(mac_idx);

	reg_val = AX_BASE_READ_REG32(mac_base, R_MAC_CONFIGURATION);
	reg_val |= MAC_CONFIGURATION_TE | MAC_CONFIGURATION_RE; //Transmitter and receiver enable
	AX_BASE_WRITE_REG32(mac_base, R_MAC_CONFIGURATION, reg_val);

	FLOW("[8] Start MAC: TE = 1, RE = 1\n");
	flow_reg("MAC_Configuration", mac_base, R_MAC_CONFIGURATION, FF_MAC_CONFIGURATION);
	// summary from the value READ BACK, so it always matches the hardware
	reg_val = AX_BASE_READ_REG32(mac_base, R_MAC_CONFIGURATION);
	FLOW("[8] MAC: CST=%d ACS=%d LM=%d (loopback) FES=%d (%s) DM=%d (%s duplex) TE=%d RE=%d\n",
		 (reg_val & MAC_CONFIGURATION_CST) != 0, (reg_val & MAC_CONFIGURATION_ACS) != 0, (reg_val & MAC_CONFIGURATION_LM) != 0,
		 (reg_val & MAC_CONFIGURATION_FES) != 0, (reg_val & MAC_CONFIGURATION_FES) ? "100M" : "10M",
		 (reg_val & MAC_CONFIGURATION_DM) != 0, (reg_val & MAC_CONFIGURATION_DM) ? "full" : "half",
		 (reg_val & MAC_CONFIGURATION_TE) != 0, (reg_val & MAC_CONFIGURATION_RE) != 0);
	flow_mac_state(mac_idx);
	flow_mmc(mac_idx);
}
/**
 * MAC internal loopback: MAC_Configuration.LM (p.3112, §61.5.8 p.2998)
 * TX packets go back to the MAC receiver inside the MAC; RMII/PHY are not used.
 * Loopback needs full duplex (MAC_Configuration.DM = 1).
 *
 * disable: MAC_Configuration.LM [12] = 0 -> normal mode, TX goes out on RMII to the PHY.
 * Read-modify-write: the other MAC_Configuration bits do not change.
 */
void disable_mac_internal_lb(uint8_t mac_idx){
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t reg_val;

	reg_val = AX_BASE_READ_REG32(mac_base, R_MAC_CONFIGURATION);
	reg_val &= ~MAC_CONFIGURATION_LM; //LM
	AX_BASE_WRITE_REG32(mac_base, R_MAC_CONFIGURATION, reg_val);
}
/* enable: MAC_Configuration.LM [12] = 1 -> TX goes back to RX inside the MAC (read-modify-write) */
// MARK: 06: enable_mac_internal_lb
void enable_mac_internal_lb(uint8_t mac_idx){
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t reg_val;

	reg_val = AX_BASE_READ_REG32(mac_base, R_MAC_CONFIGURATION);
	reg_val |= MAC_CONFIGURATION_LM; //LM
	AX_BASE_WRITE_REG32(mac_base, R_MAC_CONFIGURATION, reg_val);
}
/* Init MAC Internal Loopback: MAC_Configuration.LM [12] = 1 (enable_mac_internal_lb), print MAC_Configuration */

void init_mac_internal_lb(uint8_t mac_idx){
	eth_printf("Init MAC Internal Loopback\n");

	enable_mac_internal_lb(mac_idx);
	FLOW("[8] MAC loopback: MAC_Configuration.LM = 1 (bit 12), TX goes back to RX inside the MAC\n");
	flow_reg("MAC_Configuration", MAC_BASE_ADRS(mac_idx), R_MAC_CONFIGURATION, FF_MAC_CONFIGURATION);
}
