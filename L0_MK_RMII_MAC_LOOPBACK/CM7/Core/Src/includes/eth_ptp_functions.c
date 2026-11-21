#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_ptp_utils.h"
#include "eth_traffic_test.h"
#include "eth_printf.h"
#include "eth_flow_utils.h"

/*
 * PTP timestamp test (eth_run_test_ops = 3), through the MAC loopback.
 * The MAC has a system time counter (IEEE 1588 PTP, RM0399 §61.5) and stamps a frame when its SFD
 * passes the MII: TX into the descriptor (TDES0/1, TDES3.TTSS), RX into a context descriptor written
 * after the frame (RDES0/1, RDES3.CTXT). The counter is started in its simplest form (coarse update,
 * SSINC = 1: the sub-second register is a raw tick counter of the PTP clock = HCLK) and the raw values
 * written by the hardware are printed. No PTP protocol, no conversion to ns.
 * Each frame of PT_LENS[]: send with TDES2.TTSE -> TX timestamp -> receive -> RX timestamp -> compare.
 * Expected: frame SAME, both timestamps written, RX >= TX, TX increasing, system time higher after.
 * Then the one-step timestamp (OST, RM0399 p.2983-2985): a TX context descriptor with OSTC = 1 before a PTP
 * Sync frame (PTP over Ethernet, type 0x88F7) makes the MAC write the system time INTO the frame
 * (originTimestamp, bytes 48..57) while it goes out, instead of only reporting it in TDES0/1 (two-step).
 *   1  context OSTC + Sync with originTimestamp = 0 -> received Sync carries the time, close to the TX timestamp
 *   2  the same Sync without a new context           -> shows whether OSTC stays valid for the next frames
 *
 * RM0399: programming guide §61.9.9 p.3036; MAC_Timestamp_Control p.3176; MAC_Sub_Second_Increment p.3178; MAC_System_Time_Seconds/STNR p.3180;
 * MAC_System_Time_Seconds_Update/STNUR p.3181; TDES2.TTSE p.3050; TDES0/1 write-back p.3054; RDES1.TSA p.3066;
 * RX context descriptor p.3072-3073. One-step: Table 564 p.2983, enabling p.2985, TDES0-3 context p.3058-3060,
 * MAC_HW_Feature1.OSTEN p.3145, MAC_Timestamp_Control TSIPENA / TSVER2ENA / TSEVNTENA / TSMSTRENA p.3177.
 */

/* PTP Sync over Ethernet, IEEE 1588-2008 header (34 bytes) + originTimestamp (10 bytes) = 44, frame 58 -> sent as 60.
 * Byte offsets in the frame: 12..13 EtherType 88 F7 | 14 messageType 0 (Sync) | 15 versionPTP 2 | 16..17 messageLength 44 |
 * 18 domain 0 | 20..21 flagField (twoStepFlag [bit 1 of byte 20] = 0: one-step) | 22..29 correctionField |
 * 34..43 sourcePortIdentity | 44..45 sequenceId | 46 controlField 0 | 47 logMessageInterval | 48..53 originTimestamp
 * seconds (48 bits) | 54..57 originTimestamp nanoseconds | 58..59 pad */
#define OS_FRAME_LEN	60
#define OS_CORR_OFF		22		// correctionField, 8 bytes (the MAC may touch it too: Table 564 "subnanosecond correction")
#define OS_ORIG_OFF		48		// originTimestamp, 10 bytes: what the MAC writes in master mode
#define OS_PTP_SYNC		0x00	// messageType Sync, transportSpecific 0
#define OS_PTP_VER2		0x02
#define OS_PTP_LEN		44
#define OS_PTP_TYPE_HI	0x88	// EtherType 0x88F7: PTP over Ethernet
#define OS_PTP_TYPE_LO	0xF7

/* Frame lengths in bytes (no FCS, >= 60 so the MAC adds no pad) */
static const uint16_t PT_LENS[] = {60, 64, 128, 256, 512, 1024, 1514};
#define PT_N_LENS (sizeof PT_LENS / sizeof PT_LENS[0])

/* Result of one frame */
struct ptp_result {
	int      tx;               // egress_packet() return value
	int      rx;               // ingress_packet_status() return value
	int      same;             // 1 = frame received = frame sent
	struct eth_timestamp tts;  // TX timestamp: TDES1 (s), TDES0 (sub-second)
	struct eth_timestamp rts;  // RX timestamp: RDES1 (s), RDES0 (sub-second) of the context descriptor
};

/**
 * Start the system time of the MAC (RM0399 §61.9.9 p.3036), coarse update: every PTP clock tick the
 * sub-second register adds SSINC. Call after mac_init_100m() and before mac_start() (TE/RE after it).
 *
 *   Register               Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   MAC_Timestamp_Control p.3176     TSENA [0]            1       timestamp block on, counter runs
 *                          TSCFUPDT [1]         0       coarse update: + SSINC per clock tick
 *                          TSCTRLSSR [9]        0       binary rollover: sub-second 0..0x7FFFFFFF
 *                          TSINIT [2]           1 -> 0  load MAC_System_Time_Seconds_Update/STNUR into the counter (self-clearing)
 *                          TSENALL [8]          1       stamp every received frame (context descriptor)
 *   MAC_Sub_Second_Increment p.3178     SSINC [23:16]        1       sub-second + 1 per tick: raw tick counter
 *   MAC_System_Time_Seconds_Update p.3181    TSS [31:0]           0       start value, seconds
 *   MAC_System_Time_Nanoseconds_Update p.3181    TSSS [30:0]          0       start value, sub-second
 * Return 1: OK, 0: TSINIT did not clear
 */
// MARK: 08: mac_ptp_init
uint8_t mac_ptp_init(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t mactscr;
	uint32_t macssir;
	uint32_t n = 1000;

	/* MAC_Interrupt_Enable.TSIE stays 0 (reset value): the test polls, no interrupt */
	mactscr = MAC_TIMESTAMP_CONTROL_TSENA;									// TSENA [0]: timestamp block on
	macssir = (MAC_SUB_SECOND_INCREMENT_SSINC_TICK << MAC_SUB_SECOND_INCREMENT_SSINC_bit);		// SSINC [23:16]: + 1 per PTP clock tick

	FLOW_STEP("[8]", "PTP: start the system time (timestamp counter) before TE/RE");
	AX_BASE_WRITE_REG32(mac_base, R_MAC_TIMESTAMP_CONTROL, mactscr);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_SUB_SECOND_INCREMENT, macssir);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_SYSTEM_TIME_SECONDS_UPDATE, 0U);				// TSS [31:0]: 0 s
	AX_BASE_WRITE_REG32(mac_base, R_MAC_SYSTEM_TIME_NANOSECONDS_UPDATE, 0U);				// TSSS [30:0]: 0 sub-second
	AX_BASE_WRITE_REG32(mac_base, R_MAC_TIMESTAMP_CONTROL, mactscr | MAC_TIMESTAMP_CONTROL_TSINIT);	// TSINIT [2]: load, self-clearing
	while ((AX_BASE_READ_REG32(mac_base, R_MAC_TIMESTAMP_CONTROL) & MAC_TIMESTAMP_CONTROL_TSINIT) && n > 0) {
		udelay(1);
		n--;
	}
	if (n == 0) {
		eth_printf("PTP: MAC_Timestamp_Control.TSINIT did not clear, MAC_Timestamp_Control = 0x%08lX\n",
				   (unsigned long)AX_BASE_READ_REG32(mac_base, R_MAC_TIMESTAMP_CONTROL));
		return 0;
	}
	mactscr |= MAC_TIMESTAMP_CONTROL_TSENALL;									// TSENALL [8]: stamp every RX frame
	AX_BASE_WRITE_REG32(mac_base, R_MAC_TIMESTAMP_CONTROL, mactscr);

	FLOW("[8] PTP:\n");
	flow_regw("MAC_Timestamp_Control", mac_base, R_MAC_TIMESTAMP_CONTROL, mactscr, FF_MAC_TIMESTAMP_CONTROL);
	flow_regw("MAC_Sub_Second_Increment", mac_base, R_MAC_SUB_SECOND_INCREMENT, macssir, FF_NONE);
	flow_reg("MAC_System_Time_Seconds (seconds)", mac_base, R_MAC_SYSTEM_TIME_SECONDS, FF_NONE);
	flow_reg("MAC_System_Time_Nanoseconds (sub-second)", mac_base, R_MAC_SYSTEM_TIME_NANOSECONDS, FF_NONE);

	ETH_DEBUG_PRINTF("Done mac_ptp_init(): %d\r\n", mac_idx);
	return 1;
}
/* Build the Sync frame in f: DA | SA | 88 F7 | PTP header with originTimestamp = 0 */
static void ost_build_sync(uint8_t *f, uint16_t seq)
{
	unsigned char da[ETH_TEST_MAC_SIZE] = ETH_TEST_DEST_MAC;
	unsigned char sa[ETH_TEST_MAC_SIZE] = ETH_TEST_SRC_MAC;

	memset(f, 0, OS_FRAME_LEN);
	memcpy(f, da, ETH_TEST_MAC_SIZE);
	memcpy(f + ETH_TEST_MAC_SIZE, sa, ETH_TEST_MAC_SIZE);
	f[12] = OS_PTP_TYPE_HI;					// EtherType 0x88F7
	f[13] = OS_PTP_TYPE_LO;
	f[14] = OS_PTP_SYNC;					// transportSpecific 0, messageType 0 = Sync
	f[15] = OS_PTP_VER2;					// versionPTP 2
	f[16] = (OS_PTP_LEN >> 8) & 0xFF;		// messageLength 44
	f[17] = OS_PTP_LEN & 0xFF;
	f[18] = 0;								// domainNumber 0
	f[20] = 0;								// flagField: twoStepFlag = 0 -> one-step Sync
	f[21] = 0;
	memcpy(f + 34, sa, ETH_TEST_MAC_SIZE);	// sourcePortIdentity: clockIdentity (SA) + port 1
	f[42] = 0;
	f[43] = 1;
	f[44] = (seq >> 8) & 0xFF;				// sequenceId
	f[45] = seq & 0xFF;
	f[46] = 0;								// controlField 0 = Sync
	f[47] = 0;								// logMessageInterval
	/* 48..57 originTimestamp = 0: whatever is there after the loopback was written by the MAC */
}
/* Big-endian read of n bytes (n <= 4) at p */
static uint32_t ost_be(const uint8_t *p, int n)
{
	uint32_t v = 0;
	int i;

	for (i = 0; i < n; i++) {
		v = (v << 8) | p[i];
	}
	return v;
}
/**
 * One Sync frame of the one-step test: (context OSTC) -> Sync with TDES2.TTSE -> receive -> read what the MAC
 * wrote at bytes 48..57 and compare it with the TX timestamp of the same frame (TDES0/1 write-back).
 * Pass (ctx = 1): rest of the frame unchanged, originTimestamp != 0, seconds = TX timestamp seconds (or + 1
 * at a rollover), context closed without CDE. For ctx = 0 the line is information: the result is not counted.
 * Return 1: as expected, 0: else
 */
static int ost_case(uint8_t mac_idx, const char *id, int ctx, uint16_t seq, const char *what)
{
	static uint8_t frame[MAC_100M_BUF_SIZE];
	static uint8_t rxbuf[MAC_100M_BUF_SIZE];
	uint32_t ctx_idx = 0, ctx_des3 = 0;
	uint32_t rdes3 = 0;
	uint32_t orig_s, orig_ns, corr_hi, corr_lo;
	struct eth_timestamp tts;
	int tx, rx_len, same, ok;

	ost_build_sync(frame, seq);
	/* 1. context descriptor OSTC (no ingress timestamp: a master fills originTimestamp, not the correction) */
	if (ctx) {
		ctx_idx = egress_context_ost(0U, 0U);
	}
	/* 2. the Sync: TDES2.TTSE is set too (eth_ptp_on), the TX timestamp is independent of OST (p.2985 note 2) */
	tx = egress_packet(frame, OS_FRAME_LEN);
	tts = eth_tx_ts;
	if (ctx) {
		ctx_des3 = tx_desc[ctx_idx].des3;
	}
	if (tx != OS_FRAME_LEN) {
		eth_printf("  %s  TX error %d\n", id, tx);
		return 0;
	}
	/* 3. receive (the RX context descriptor after it is consumed by ingress_packet_status) */
	rx_len = ingress_packet_status(mac_idx, rxbuf, sizeof(rxbuf), ETH_TEST_RX_TIMEOUT_US, NULL, &rdes3);
	if (rx_len < 0) {
		eth_printf("  %s  RX error %d (RDES3=0x%08lX)\n", id, rx_len, (unsigned long)rdes3);
		return 0;
	}
	/* 4. what the MAC wrote: originTimestamp 48..57 (seconds low 32 bits at 50..53), correctionField 22..29 */
	orig_s = ost_be(rxbuf + OS_ORIG_OFF + 2, 4);
	orig_ns = ost_be(rxbuf + OS_ORIG_OFF + 6, 4);
	corr_hi = ost_be(rxbuf + OS_CORR_OFF, 4);
	corr_lo = ost_be(rxbuf + OS_CORR_OFF + 4, 4);
	same = (rx_len == OS_FRAME_LEN) && (memcmp(rxbuf, frame, OS_CORR_OFF) == 0)
		&& (memcmp(rxbuf + OS_CORR_OFF + 8, frame + OS_CORR_OFF + 8, OS_ORIG_OFF - OS_CORR_OFF - 8) == 0)
		&& (memcmp(rxbuf + OS_ORIG_OFF + 10, frame + OS_ORIG_OFF + 10, OS_FRAME_LEN - OS_ORIG_OFF - 10) == 0);
	ok = same && tts.valid && (orig_s != 0 || orig_ns != 0)
	  && (orig_s == tts.s || orig_s == tts.s + 1)
	  && (!ctx || (ctx_des3 & (TDES3_OWN | TDES3_CTXT_CDE)) == 0U);
	/* 5. one line: origin written by the MAC | TX timestamp of the same frame | correction | rest | ctx | result */
	eth_printf("  %s  %-10s  origin s %4lu ns 0x%08lX | TX ts s %4lu ns 0x%08lX | corr %08lX%08lX | rest %s | ctx ",
			   id, ctx ? "ctx OSTC" : "no ctx", (unsigned long)orig_s, (unsigned long)orig_ns,
			   (unsigned long)tts.s, (unsigned long)tts.ns, (unsigned long)corr_hi, (unsigned long)corr_lo,
			   same ? "SAME" : "DIFF");
	if (ctx) {
		eth_printf("0x%08lX | %s  %s\n", (unsigned long)ctx_des3, ok ? "OK " : "BAD", what);
	} else {
		eth_printf("-          | %s  %s\n", (orig_s != 0 || orig_ns != 0) ? "written" : "left 0 ", what);
	}
	return ok;
}
/**
 * One-step timestamp through the MAC loopback: two Sync frames, see the top of the file.
 * Same bits as eth_mac_ptp_lb() plus:
 *
 *   Register / descriptor  Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   MAC_HW_Feature1 p.3145    OSTEN [11]           1       read: one-step exists on this core (else skipped, not failed)
 *   MAC_Timestamp_Control p.3177     TSVER2ENA [10]       1       PTP version 2 header
 *                          TSIPENA [11]         1       PTP over Ethernet (0x88F7) is parsed
 *                          TSEVNTENA [14]       1       event messages (Sync, ...)
 *                          TSMSTRENA [15]       1       master side: Sync is the message to stamp
 *                          SNAPTYPSEL [17:16]   00      with the two above: "Ordinary or Boundary Master" (Table 564)
 *   TDES3 context p.3059   CTXT, OSTC [27], TCMSSV [26] 1  one-step on for the frame that follows
 *   TDES0/1 context        TTSL / TTSH          0       ingress timestamp, not used by a master
 *   TDES2 frame            TTSE [30]            1       TX timestamp as well (eth_ptp_on), to compare
 * Return 1: case 1 as expected (or OST not on this core), 0: else
 */
static uint8_t ptp_one_step(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t machwf1r = AX_BASE_READ_REG32(mac_base, R_MAC_HW_FEATURE1);
	uint32_t mactscr;
	int ok;

	eth_printf("\n================ ONE-STEP TIMESTAMP (TX context descriptor OSTC, PTP Sync over Ethernet) ================\n");
	eth_printf("MAC_HW_Feature1 = 0x%08lX: OSTEN [11] = %d, POUOST [23] = %d (one-step for PTP over UDP)\n",
			   (unsigned long)machwf1r, (machwf1r & MAC_HW_FEATURE1_OSTEN) != 0, (machwf1r & MAC_HW_FEATURE1_POUOST) != 0);
	if ((machwf1r & MAC_HW_FEATURE1_OSTEN) == 0U) {
		eth_printf("one-step timestamp is not available on this core, part skipped\n");
		return 1;
	}
	/* PTP packet processing on, master mode (Table 564 p.2983: SNAPTYPSEL 00, TSMSTRENA 1, TSEVNTENA 1 -> Sync) */
	mactscr = AX_BASE_READ_REG32(mac_base, R_MAC_TIMESTAMP_CONTROL)
			| MAC_TIMESTAMP_CONTROL_TSVER2ENA											// TSVER2ENA [10]: PTP v2
			| MAC_TIMESTAMP_CONTROL_TSIPENA											// TSIPENA [11]: PTP over Ethernet
			| MAC_TIMESTAMP_CONTROL_TSEVNTENA											// TSEVNTENA [14]: event messages
			| MAC_TIMESTAMP_CONTROL_TSMSTRENA;										// TSMSTRENA [15]: master -> Sync; SNAPTYPSEL [17:16] stays 00
	AX_BASE_WRITE_REG32(mac_base, R_MAC_TIMESTAMP_CONTROL, mactscr);
	flow_regw("MAC_Timestamp_Control (one-step)", mac_base, R_MAC_TIMESTAMP_CONTROL, mactscr, FF_MAC_TIMESTAMP_CONTROL);

	eth_printf("  Sync frame %d bytes, EtherType 88 F7, originTimestamp bytes 48..57 sent as 0: a value there is the MAC's work.\n", OS_FRAME_LEN);
	eth_printf("  origin = seconds [50..53] / nanoseconds [54..57] received; TX ts = TDES1 / TDES0 of the same frame (TTSE).\n");
	eth_printf("  Both come from the same system time, so s must match and ns differ by a few ticks. Frame 1 prints FLOW.\n");
	eth_ptp_on = 1;
	eth_flow_trace = 1;
	ok = ost_case(mac_idx, "1", 1, 1, "context OSTC: the MAC fills originTimestamp of the Sync");
	eth_flow_trace = 0;
	ost_case(mac_idx, "2", 0, 2, "no new context: does OSTC stay valid for the next frame? (information)");
	eth_ptp_on = 0;
	eth_printf("\nResult: one-step %s\n", ok ? "OK" : "BAD");
	return (uint8_t)ok;
}
/* Read the system time: seconds, sub-second, seconds again; repeat if the seconds changed in between (p.3180) */
static void ptp_read_time(uint32_t mac_base, uint32_t *s, uint32_t *sub)
{
	uint32_t s1, s2;

	do {
		s1 = AX_BASE_READ_REG32(mac_base, R_MAC_SYSTEM_TIME_SECONDS);
		*sub = AX_BASE_READ_REG32(mac_base, R_MAC_SYSTEM_TIME_NANOSECONDS);
		s2 = AX_BASE_READ_REG32(mac_base, R_MAC_SYSTEM_TIME_SECONDS);
	} while (s1 != s2);
	*s = s1;
}
/* 1: (s1, sub1) is after (s0, sub0) */
static int ptp_after(uint32_t s0, uint32_t sub0, uint32_t s1, uint32_t sub1)
{
	return (s1 > s0) || (s1 == s0 && sub1 > sub0);
}
/* One frame: send with TTSE, receive (+ context descriptor), compare, keep both timestamps */
static void ptp_run_frame(uint8_t mac_idx, uint16_t len, struct ptp_result *r)
{
	static uint8_t frame[MAC_100M_BUF_SIZE];
	static uint8_t rxbuf[MAC_100M_BUF_SIZE];
	unsigned char da[ETH_TEST_MAC_SIZE] = ETH_TEST_DEST_MAC;
	unsigned char sa[ETH_TEST_MAC_SIZE] = ETH_TEST_SRC_MAC;
	uint32_t rdes3 = 0;
	uint32_t i;

	memset(r, 0, sizeof(*r));
	// Frame: DA | SA | EtherType | payload 0x10..0x6F
	memcpy(frame, da, ETH_TEST_MAC_SIZE);
	memcpy(frame + ETH_TEST_MAC_SIZE, sa, ETH_TEST_MAC_SIZE);
	frame[12] = (ETH_TEST_ETHER_TYPE >> 8) & 0xFF;
	frame[13] = ETH_TEST_ETHER_TYPE & 0xFF;
	for (i = ETH_TEST_HEADER_SIZE; i < len; i++) {
		frame[i] = (uint8_t)(0x10 + (i % 0x60));
	}

	r->tx = egress_packet(frame, len);		// TDES2.TTSE set by eth_ptp_on; TX timestamp -> eth_tx_ts
	r->tts = eth_tx_ts;
	if (r->tx != len) {
		return;
	}
	r->rx = ingress_packet_status(mac_idx, rxbuf, sizeof(rxbuf), ETH_TEST_RX_TIMEOUT_US, NULL, &rdes3);
	r->rts = eth_rx_ts;						// from the context descriptor after the frame
	if (r->rx < 0) {
		return;
	}
	r->same = (r->rx == len) && (memcmp(rxbuf, frame, len) == 0);
}
/* Table of every frame, then the summary. Return 1: every check OK */
static uint8_t ptp_print_results(const struct ptp_result res[])
{
	uint32_t n_same = 0, n_ts = 0, n_order = 0, n_inc = 0;
	uint32_t i;

	eth_printf("\n================ PTP TIMESTAMP TEST (MAC_Timestamp_Control, TDES2.TTSE, RX context descriptor) ================\n");
	eth_printf("  s = seconds register, sub = sub-second register (raw: + 1 per PTP clock tick, SSINC = 1)\n");
	eth_printf("  TX = TDES1 / TDES0 after write-back (TTSS = 1), RX = RDES1 / RDES0 of the context descriptor\n");
	eth_printf("   len   TX s  TX sub       RX s  RX sub       RX-TX sub  data\n");
	for (i = 0; i < PT_N_LENS; i++) {
		const struct ptp_result *r = &res[i];

		eth_printf("  %4u   ", (unsigned int)PT_LENS[i]);
		if (r->tx != PT_LENS[i]) {
			eth_printf("TX error %d\n", r->tx);
			continue;
		}
		if (r->tts.valid) {
			eth_printf("%4lu  0x%08lX   ", (unsigned long)r->tts.s, (unsigned long)r->tts.ns);
		} else {
			eth_printf("(no TX timestamp)  ");
		}
		if (r->rx < 0) {
			eth_printf("RX error %d\n", r->rx);
			continue;
		}
		if (r->rts.valid) {
			eth_printf("%4lu  0x%08lX   ", (unsigned long)r->rts.s, (unsigned long)r->rts.ns);
		} else {
			eth_printf("(no RX timestamp)  ");
		}
		if (r->tts.valid && r->rts.valid) {
			eth_printf("%9lu  ", (unsigned long)(r->rts.ns - r->tts.ns));
		} else {
			eth_printf("        -  ");
		}
		eth_printf("%s", r->same ? "SAME" : "DIFF");

		n_same += (uint32_t)r->same;
		if (r->tts.valid && r->rts.valid) {
			n_ts++;
			if (ptp_after(r->tts.s, r->tts.ns, r->rts.s, r->rts.ns) || (r->tts.s == r->rts.s && r->tts.ns == r->rts.ns)) {
				n_order++;
			} else {
				eth_printf("  RX before TX");
			}
			if (i == 0 || !res[i - 1].tts.valid || ptp_after(res[i - 1].tts.s, res[i - 1].tts.ns, r->tts.s, r->tts.ns)) {
				n_inc++;
			} else {
				eth_printf("  TX not increasing");
			}
		}
		eth_printf("\n");
	}
	eth_printf("\nResult: data SAME %lu/%lu, both timestamps %lu/%lu, RX >= TX %lu/%lu, TX increasing %lu/%lu\n",
			   (unsigned long)n_same, (unsigned long)PT_N_LENS, (unsigned long)n_ts, (unsigned long)PT_N_LENS,
			   (unsigned long)n_order, (unsigned long)PT_N_LENS, (unsigned long)n_inc, (unsigned long)PT_N_LENS);
	return (n_same == PT_N_LENS) && (n_ts == PT_N_LENS) && (n_order == PT_N_LENS) && (n_inc == PT_N_LENS);
}
/**
 * PTP timestamp test through the MAC loopback (eth_run_test_ops = 3)
 * Init MAC, start the system time (mac_ptp_init), enable the loopback, start DMA and MAC, then one
 * stamped frame per PT_LENS[] entry with eth_ptp_on = 1 (TDES2.TTSE, context descriptors read).
 * Same bits as eth_mac_internal_lb() (eth_functions.c) plus the table on top of mac_ptp_init().
 * The MAC stays with the timestamp block on; the next test starts with eth_reset_mac().
 * Return 1: PASS, 0: FAIL
 */
uint16_t eth_mac_ptp_lb(uint8_t mac_idx)
{
	static struct ptp_result res[PT_N_LENS];   // kept for the table at the end
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t s0, sub0, s1, sub1;
	uint32_t i;
	uint8_t ok;
	eth_printf("Start Init: Port[%d]\n", mac_idx);

	if (eth_reset_mac(mac_idx) == 0) {
		return 0;
	}
	mac_init_100m(mac_idx);
	if (mac_ptp_init(mac_idx) == 0) {
		return 0;
	}
	init_mac_internal_lb(mac_idx);
	mac_start(mac_idx);

	ptp_read_time(mac_base, &s0, &sub0);
	eth_ptp_on = 1;
	eth_printf("Send %lu frames with TDES2.TTSE: frames 1..%d step by step ([10]/[11] + context descriptor), then one \".\" per frame\n",
			   (unsigned long)PT_N_LENS, FLOW_PKT_MAX);
	for (i = 0; i < PT_N_LENS; i++) {
		eth_flow_trace = (i < FLOW_PKT_MAX);	// [10] / [11] FLOW lines for this frame
		ptp_run_frame(mac_idx, PT_LENS[i], &res[i]);
		eth_printf(".");
	}
	eth_flow_trace = 0;
	eth_printf("\n");
	eth_ptp_on = 0;
	ptp_read_time(mac_base, &s1, &sub1);

	ok = ptp_print_results(res);
	eth_printf("System time MAC_System_Time_Seconds / MAC_System_Time_Nanoseconds: %lu / 0x%08lX before the frames, %lu / 0x%08lX after: %s\n",
			   (unsigned long)s0, (unsigned long)sub0, (unsigned long)s1, (unsigned long)sub1,
			   ptp_after(s0, sub0, s1, sub1) ? "counting" : "NOT counting");
	if (!ptp_after(s0, sub0, s1, sub1)) {
		ok = 0;
	}
	/* one-step timestamp: the system time is running and the loopback is up, same MAC state */
	if (ptp_one_step(mac_idx) == 0) {
		ok = 0;
	}
	eth_print_mac_counters(mac_idx);
	return ok;
}
