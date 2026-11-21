#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_dma_order_utils.h"
#include "eth_traffic_test.h"
#include "eth_printf.h"

/*
 * DMA start order test (eth_run_test_ops = 13), through the MAC loopback.
 * Same init as mode 0 (eth_mac_internal_lb): eth_reset_mac(), mac_init_100m(), init_mac_internal_lb().
 * mac_start() is NOT called: the test starts the DMA / MAC itself, in its own order. One run, 2 cases:
 *
 *   A  TX tail pointer first, then DMA_CH0_Tx_Control.ST = 1
 *      RX side started as usual: DMA_CH0_Rx_Control.SR = 1, MAC_Configuration.TE = RE = 1. TX DMA still stopped (ST = 0).
 *      1. tx_desc[head] filled (OWN = 1, FD, LD), DMA_CH0_TxDesc_Tail_Pointer = &tx_desc[head + 1]
 *      2. wait DO_WAIT_US: nothing must go out (TDES3.OWN = 1, Tx_Packet_Count_Good unchanged, TPS0 = 0 stopped)
 *      3. ST = 1: the DMA starts at DMA_CH0_TxDesc_List_Address (= tx_desc[0]) and sends up to the tail
 *      -> TDES3.OWN = 0, ES = 0, the frame comes back the same (ingress_packet)
 *
 *   B  RX DMA stopped, frame held in the MTL RX FIFO for DO_HOLD_MS, then DMA_CH0_Rx_Control.SR = 1
 *      1. SR = 0 -> RPS0 = 0 (stopped)
 *      2. send 1 frame (egress_packet): MAC loopback -> MTL RX FIFO (2 KB, threshold mode RSF = 0); no DMA to read it
 *      3. check: MTL_RxQ0_Debug.PRXQ = 1, rx_desc[rx_head].RDES3.OWN = 1 (nothing written), nothing dropped
 *      4. hold DO_HOLD_MS (10 s), check again
 *      5. SR = 1 -> the DMA reads the frame from the FIFO into rx_desc[rx_head]
 *      -> the frame comes back the same, PRXQ = 0, MTL_RxQ0_Missed_Packet_Overflow_Cnt = 0, DMA_CH0_Miss_Frame_Cnt = 0
 *
 *   Register / descriptor                 Bit [pos]          Used for
 *   ------------------------------------- ------------------ ------------------------------
 *   DMA_CH0_Tx_Control p.3085             ST [0]             A: set after the tail pointer
 *   DMA_CH0_Rx_Control p.3086             SR [0]             B: 0 during the hold, then 1
 *   DMA_CH0_TxDesc_Tail_Pointer p.3088    TDT [31:2]         A: written while ST = 0
 *   DMA_Debug_Status0 p.3078              TPS0 [15:12]       A: 0 = stopped before ST
 *                                         RPS0 [11:8]        B: 0 = stopped during the hold
 *   MTL_RxQ0_Debug p.3109                 PRXQ [29:16]       B: frames waiting in the RX FIFO
 *   MTL_RxQ0_Missed_Packet_Overflow_Cnt   OVFPKTCNT [10:0]   B: 0 (clear on read)
 *     p.3108                              MISPKTCNT [26:16]  B: 0 (clear on read)
 *   DMA_CH0_Miss_Frame_Cnt p.3096         MFC [10:0]         B: 0 (clear on read)
 *   Tx_Packet_Count_Good p.3163           [31:0]             A: unchanged before ST, +1 after
 * Return 1: PASS (A and B OK), 0: FAIL
 */

#define DO_FRAME_LEN	60			// 60 bytes + 4 FCS: smallest frame, no pad
#define DO_WAIT_US		1000		// A: time given to the TX DMA before ST = 1 (it must not send)
#define DO_SETTLE_US	200			// after a stop command / a frame: time for the state to land
#define DO_HOLD_MS		10000		// B: time the frame stays in the MTL RX FIFO
#define DO_ID_A			0xA0		// first payload byte: frame of case A
#define DO_ID_B			0xB0		// first payload byte: frame of case B

static uint8_t do_frame[MAC_100M_BUF_SIZE];
static uint8_t do_rxbuf[MAC_100M_BUF_SIZE];
static uint8_t do_txbuf[MAC_100M_BUF_SIZE] __attribute__((aligned(32)));	// A: TX buffer of the descriptor written by hand

/* Build the test frame: DA | SA | 88 B5 | id, 0x11, 0x12 ... (DO_FRAME_LEN bytes, no FCS) */
static void do_build(uint8_t id)
{
	unsigned char da[ETH_TEST_MAC_SIZE] = ETH_TEST_DEST_MAC;
	unsigned char sa[ETH_TEST_MAC_SIZE] = ETH_TEST_SRC_MAC;
	int i;

	memcpy(do_frame, da, ETH_TEST_MAC_SIZE);
	memcpy(do_frame + ETH_TEST_MAC_SIZE, sa, ETH_TEST_MAC_SIZE);
	do_frame[12] = (ETH_TEST_ETHER_TYPE >> 8) & 0xFF;
	do_frame[13] = ETH_TEST_ETHER_TYPE & 0xFF;
	for (i = ETH_TEST_HEADER_SIZE; i < DO_FRAME_LEN; i++) {
		do_frame[i] = (uint8_t)(0x10 + i);
	}
	do_frame[ETH_TEST_HEADER_SIZE] = id;
}
/* DMA_Debug_Status0: TPS0 (tx = 1) or RPS0 (tx = 0) */
static uint32_t do_ps(uint32_t mac_base, int tx)
{
	uint32_t v = AX_BASE_READ_REG32(mac_base, R_DMA_DEBUG_STATUS0);

	return (v >> (tx ? DMA_DEBUG_STATUS0_TPS0_bit : DMA_DEBUG_STATUS0_RPS0_bit)) & DMA_DEBUG_STATUS0_PS_MASK;
}
/* MTL_RxQ0_Debug.PRXQ: frames waiting in the MTL RX FIFO */
static uint32_t do_prxq(uint32_t mac_base)
{
	return (AX_BASE_READ_REG32(mac_base, R_MTL_RXQ0_DEBUG) >> MTL_RXQ0_DEBUG_PRXQ_bit) & MTL_RXQ0_DEBUG_PRXQ_MASK;
}
/* Receive one frame and compare it with do_frame; 1 = same */
static int do_recv_check(uint8_t mac_idx)
{
	int rx = ingress_packet(mac_idx, do_rxbuf, sizeof(do_rxbuf));

	if (rx < 0) {
		eth_printf("      RX: no frame (%d)\n", rx);
		return 0;
	}
	if (!compare_packets(do_rxbuf, rx, do_frame, DO_FRAME_LEN)) {
		eth_printf("      RX: %d bytes, differs from the frame sent (%d bytes), first payload byte 0x%02X\n",
				   rx, DO_FRAME_LEN, do_rxbuf[ETH_TEST_HEADER_SIZE]);
		return 0;
	}
	eth_printf("      RX: %d bytes, same as the frame sent (first payload byte 0x%02X)\n", rx, do_rxbuf[ETH_TEST_HEADER_SIZE]);
	return 1;
}

/* A: TX tail pointer written while ST = 0, then ST = 1 */
static int do_case_a(uint8_t mac_idx, uint32_t mac_base)
{
	uint32_t idx = eth_tx_head;
	struct eth_desc *tx = &tx_desc[idx];
	uint32_t reg_val, status, good0, good1, good2, tps;
	int count = ETH_TEST_TX_TIMEOUT_US;
	int ok = 1;

	eth_printf("\n--- A: TX tail pointer first, then DMA_CH0_Tx_Control.ST = 1 ---\n");

	/* start everything except the TX DMA: SR = 1, then MAC TE = RE = 1 (same bits as mac_start) */
	reg_val = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_RX_CONTROL);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_RX_CONTROL, reg_val | DMA_CH0_RX_CONTROL_SR);
	reg_val = AX_BASE_READ_REG32(mac_base, R_MAC_CONFIGURATION);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_CONFIGURATION, reg_val | MAC_CONFIGURATION_TE | MAC_CONFIGURATION_RE);
	udelay(DO_SETTLE_US);
	eth_printf("      DMA_CH0_Tx_Control = 0x%08lX (ST = 0), DMA_CH0_Rx_Control = 0x%08lX (SR = 1), TPS0 = %lu\n",
			   (unsigned long)AX_BASE_READ_REG32(mac_base, R_DMA_CH0_TX_CONTROL),
			   (unsigned long)AX_BASE_READ_REG32(mac_base, R_DMA_CH0_RX_CONTROL), (unsigned long)do_ps(mac_base, 1));

	/* 1. descriptor + tail pointer while the TX DMA is stopped (same words as egress_packet) */
	do_build(DO_ID_A);
	memcpy(do_txbuf, do_frame, DO_FRAME_LEN);
	good0 = AX_READ_MAC_REG32(mac_idx, MMC_STATS_TX_PKT_GOOD);
	tx->des0 = (uint32_t)(uintptr_t)do_txbuf;			// TDES0: buffer 1 address
	tx->des1 = 0;										// TDES1: no buffer 2
	tx->des2 = DO_FRAME_LEN & TDES2_B1L_MASK;			// TDES2: buffer 1 length, no IOC
	ETH_DSB();											// descriptor words before OWN
	tx->des3 = TDES3_OWN | TDES3_FD | TDES3_LD | (DO_FRAME_LEN & TDES3_FL_MASK);
	ETH_DSB();											// OWN before the tail pointer write
	eth_tx_head = (idx + 1) & (MAC_100M_TX_DESC_CNT - 1);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_TXDESC_TAIL_POINTER, (uint32_t)(uintptr_t)&tx_desc[eth_tx_head]);
	eth_printf("      tx_desc[%lu] TDES3 = 0x%08lX (OWN = 1), DMA_CH0_TxDesc_Tail_Pointer = &tx_desc[%lu] written with ST = 0\n",
			   (unsigned long)idx, (unsigned long)tx->des3, (unsigned long)eth_tx_head);

	/* 2. ST = 0: nothing may go out */
	udelay(DO_WAIT_US);
	status = tx->des3;
	good1 = AX_READ_MAC_REG32(mac_idx, MMC_STATS_TX_PKT_GOOD);
	tps = do_ps(mac_base, 1);
	eth_printf("      after %d us with ST = 0: TDES3 = 0x%08lX (OWN = %d), Tx_Packet_Count_Good +%lu, TPS0 = %lu\n",
			   DO_WAIT_US, (unsigned long)status, (status & TDES3_OWN) != 0, (unsigned long)(good1 - good0), (unsigned long)tps);
	eth_printf("      expect: OWN = 1, +0, TPS0 = 0 (stopped)\n");
	if (!(status & TDES3_OWN) || good1 != good0 || tps != DMA_DEBUG_STATUS0_PS_STOPPED) {
		eth_printf("      BAD: the TX DMA moved before ST = 1\n");
		ok = 0;
	}

	/* 3. ST = 1: the DMA must send the descriptor already behind the tail */
	reg_val = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_TX_CONTROL);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_TX_CONTROL, reg_val | DMA_CH0_TX_CONTROL_ST);
	while (((status = tx->des3) & TDES3_OWN) && count > 0) {
		udelay(1);
		count--;
	}
	good2 = AX_READ_MAC_REG32(mac_idx, MMC_STATS_TX_PKT_GOOD);
	eth_printf("      ST = 1: TDES3 write-back = 0x%08lX (OWN = %d, ES = %d) after %d us, Tx_Packet_Count_Good +%lu, TPS0 = %lu\n",
			   (unsigned long)status, (status & TDES3_OWN) != 0, (status & TDES3_ES) != 0,
			   ETH_TEST_TX_TIMEOUT_US - count, (unsigned long)(good2 - good0), (unsigned long)do_ps(mac_base, 1));
	eth_printf("      expect: OWN = 0, ES = 0, +1, frame back the same\n");
	if ((status & (TDES3_OWN | TDES3_ES)) != 0 || good2 - good0 != 1) {
		eth_printf("      BAD: frame not sent after ST = 1\n");
		ok = 0;
	}
	if (!do_recv_check(mac_idx)) {
		ok = 0;
	}
	eth_printf("  A: %s\n", ok ? "OK" : "BAD");
	return ok;
}

/* B: RX DMA stopped, frame held in the MTL RX FIFO for DO_HOLD_MS, then SR = 1 */
static int do_case_b(uint8_t mac_idx, uint32_t mac_base)
{
	uint32_t reg_val, rps, prxq, cnt, mfc, rdes3;
	struct eth_desc *rx = &rx_desc[eth_rx_head];
	int tx;
	int ok = 1;

	eth_printf("\n--- B: DMA_CH0_Rx_Control.SR = 0, 1 frame held in the MTL RX FIFO for %d s, then SR = 1 ---\n", DO_HOLD_MS / 1000);

	/* 1. stop the RX DMA */
	reg_val = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_RX_CONTROL);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_RX_CONTROL, reg_val & ~DMA_CH0_RX_CONTROL_SR);
	udelay(DO_SETTLE_US);
	rps = do_ps(mac_base, 0);
	(void)AX_BASE_READ_REG32(mac_base, R_MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT);	// clear on read
	(void)AX_BASE_READ_REG32(mac_base, R_DMA_CH0_MISS_FRAME_CNT);				// clear on read
	eth_printf("      SR = 0: RPS0 = %lu (expect 0 stopped), PRXQ = %lu\n", (unsigned long)rps, (unsigned long)do_prxq(mac_base));
	if (rps != DMA_DEBUG_STATUS0_PS_STOPPED) {
		eth_printf("      BAD: RX DMA not stopped\n");
		ok = 0;
	}

	/* 2. send 1 frame: it loops back into the MTL RX FIFO, no DMA reads it */
	do_build(DO_ID_B);
	tx = egress_packet(do_frame, DO_FRAME_LEN);
	if (tx != DO_FRAME_LEN) {
		eth_printf("      BAD: TX error %d\n", tx);
		eth_printf("  B: BAD\n");
		return 0;
	}
	udelay(DO_SETTLE_US);

	/* 3. the frame is in the FIFO, not in memory */
	prxq = do_prxq(mac_base);
	rdes3 = rx->des3;
	eth_printf("      frame sent: PRXQ = %lu, rx_desc[%lu] RDES3 = 0x%08lX (OWN = %d)\n",
			   (unsigned long)prxq, (unsigned long)eth_rx_head, (unsigned long)rdes3, (rdes3 & RDES3_OWN) != 0);
	eth_printf("      expect: PRXQ = 1, OWN = 1 (the DMA wrote nothing)\n");
	if (prxq != 1 || !(rdes3 & RDES3_OWN)) {
		ok = 0;
	}

	/* 4. hold */
	eth_printf("      holding %d s ...\n", DO_HOLD_MS / 1000);
	mdelay(DO_HOLD_MS);
	prxq = do_prxq(mac_base);
	rdes3 = rx->des3;
	cnt = AX_BASE_READ_REG32(mac_base, R_MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT);
	eth_printf("      after %d s: PRXQ = %lu, RDES3 = 0x%08lX (OWN = %d), RPS0 = %lu, missed = %lu overflow = %lu\n",
			   DO_HOLD_MS / 1000, (unsigned long)prxq, (unsigned long)rdes3, (rdes3 & RDES3_OWN) != 0,
			   (unsigned long)do_ps(mac_base, 0),
			   (unsigned long)((cnt >> MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT_MISPKTCNT_bit) & MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT_CNT_MASK),
			   (unsigned long)(cnt & MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT_CNT_MASK));
	eth_printf("      expect: PRXQ = 1, OWN = 1, RPS0 = 0, missed = 0, overflow = 0\n");
	if (prxq != 1 || !(rdes3 & RDES3_OWN) || cnt != 0) {
		ok = 0;
	}

	/* 5. SR = 1: the DMA takes the frame from the FIFO */
	reg_val = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_RX_CONTROL);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_RX_CONTROL, reg_val | DMA_CH0_RX_CONTROL_SR);
	eth_printf("      SR = 1:\n");
	if (!do_recv_check(mac_idx)) {
		ok = 0;
	}
	prxq = do_prxq(mac_base);
	cnt = AX_BASE_READ_REG32(mac_base, R_MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT);
	mfc = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_MISS_FRAME_CNT) & DMA_CH0_MISS_FRAME_CNT_MFC_MASK;
	eth_printf("      after: PRXQ = %lu, RPS0 = %lu, missed/overflow = 0x%08lX, DMA_CH0_Miss_Frame_Cnt = %lu\n",
			   (unsigned long)prxq, (unsigned long)do_ps(mac_base, 0), (unsigned long)cnt, (unsigned long)mfc);
	eth_printf("      expect: PRXQ = 0, RPS0 = 3 (waiting), 0, 0\n");
	if (prxq != 0 || cnt != 0 || mfc != 0) {
		ok = 0;
	}
	eth_printf("  B: %s\n", ok ? "OK" : "BAD");
	return ok;
}

/**
 * DMA start order test: same init as eth_mac_internal_lb() (mode 0), then cases A and B (header of this file).
 * Return 1: PASS, 0: FAIL
 */
uint16_t eth_mac_dma_order_lb(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	int a, b;
	eth_printf("Start Init: Port[%d]\n", mac_idx);

	if (eth_reset_mac(mac_idx) == 0) {
		return 0;
	}
	mac_init_100m(mac_idx);
	init_mac_internal_lb(mac_idx);
	eth_tx_ioc = 0;
	eth_rx_ioc = 0;

	eth_printf("\n================ DMA START ORDER TEST (MAC loopback, mode 0 init without mac_start) ================\n");
	a = do_case_a(mac_idx, mac_base);
	b = do_case_b(mac_idx, mac_base);

	eth_printf("\nCases: A:%s B:%s\n", a ? "OK" : "BAD", b ? "OK" : "BAD");
	eth_print_mac_counters(mac_idx);
	return (a && b) ? 1 : 0;
}
