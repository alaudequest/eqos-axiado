#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_tso_utils.h"
#include "eth_traffic_test.h"
#include "eth_printf.h"
#include "eth_flow_utils.h"

/*
 * TSO test (eth_run_test_ops = 6), through the MAC loopback: what the MSS context descriptor does.
 * TSO = TCP Segmentation Offload (RM0399 §61.5.6 p.2991-2997, programming guide §61.9.13 p.3045).
 * The CPU writes ONE TCP/IP frame whose payload is bigger than a segment, in ONE descriptor with
 * TDES3.TSE = 1. The DMA cuts the payload into pieces of MSS bytes (Maximum Segment Size, from the
 * context descriptor), puts a copy of the 54-byte header in front of each piece and fixes the header of
 * every piece (Table 567 p.2994): IP total length, IP identification (+1 per segment), IP checksum,
 * TCP sequence number (+MSS per segment), TCP checksum, PSH/FIN flags kept only in the last piece.
 * In loopback the pieces come back as separate frames: the test receives them one by one and checks
 * every field against what the CPU sent.
 *
 * Frame sent: Ethernet (14) | IPv4 (20) | TCP (20) | payload. Header in buffer 1 of the descriptor,
 * payload in buffer 2 (p.2996: buffer 1 must not hold any payload byte). Both checksums are sent as 0:
 * a correct checksum in a received segment can only be the MAC's work.
 *   A  context MSS 1000, payload 3000 -> 3 frames of 1054 bytes (seq +0 / +1000 / +2000, ID +0 / +1 / +2)
 *   B  context MSS 1460, payload 3000 -> 3 frames: 1514, 1514, 134 bytes (standard Ethernet MSS)
 *   C  no new context,   payload 500  -> 1 frame of 554 bytes: payload <= MSS, nothing to cut, checksums filled
 *   D  DMA_CH0_Control.MSS = 1000, no context, payload 3000 -> 3 frames of 1054: the register is the other way to give
 *      the MSS (p.3078); the DMA uses the last value written, register or context (here it replaces B's 1460)
 * Limits (p.2996): header + MSS + PBL bytes (64 beats = 256) < TX queue (2048); header + MSS <= 16383;
 * DMA_CH0_Tx_Control.TXPBL >= 4; full duplex only; TDES3.THL >= 5 (TCP header in 32-bit words; 2 = UDP, USO).
 *
 * Byte offsets in the frame (big-endian fields):
 *    0..5  DA        6..11 SA       12..13 EtherType 0x0800
 *   14     IP ver/IHL 0x45   15 TOS   16..17 total length   18..19 ID   20..21 flags/frag   22 TTL
 *   23     protocol 6        24..25 IP checksum    26..29 source IP    30..33 destination IP
 *   34..35 source port       36..37 dest port      38..41 seq          42..45 ack
 *   46     data offset 0x50  47 flags   48..49 window   50..51 TCP checksum   52..53 urgent
 *   54..   payload
 */

#define TS_HDR_LEN		54		// Ethernet 14 + IPv4 20 + TCP 20: TDES2.HL
#define TS_THL			5		// TDES3.THL: TCP header 20 bytes = 5 words of 32 bits
#define TS_PAYLOAD		3000	// cases A, B
#define TS_PAYLOAD_C	500		// case C: smaller than the MSS
#define TS_MSS_A		1000
#define TS_MSS_B		1460
#define TS_IP_ID		0x1234
#define TS_TCP_SEQ		0x10000000UL
#define TS_TCP_PSH		0x08
#define TS_TCP_FLAGS	0x18	// PSH | ACK: the MAC must clear PSH in every segment but the last (p.2994)
#define TS_EXTRA_US		2000	// wait for a frame that must not come
#define TS_NEW_CTX		1		// ts_case(): write a context descriptor with the MSS
#define TS_NO_CTX		0		// ts_case(): reuse the MSS stored by the DMA
#define TS_REG_MSS		2		// ts_case(): MSS written in DMA_CH0_Control.MSS [13:0] instead of a context descriptor
#define TS_DUMP_PAY		30		// ts_frame_dump(): payload bytes shown at the start and at the end

static uint8_t ts_frame[TS_HDR_LEN + TS_PAYLOAD] __attribute__((aligned(32)));	// read by the DMA
static uint8_t ts_rxbuf[MAC_100M_BUF_SIZE];

/* Internet checksum (RFC 1071): add len bytes as big-endian 16-bit words to sum */
static uint32_t csum_add(uint32_t sum, const uint8_t *p, int len)
{
	int i;

	for (i = 0; i + 1 < len; i += 2) {
		sum += ((uint32_t)p[i] << 8) | p[i + 1];
	}
	if (i < len) {
		sum += (uint32_t)p[i] << 8;
	}
	return sum;
}
/* fold to 16 bits. A block that contains its own correct checksum folds to 0xFFFF */
static uint16_t csum_fold(uint32_t sum)
{
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}
	return (uint16_t)sum;
}
/* 1: the IPv4 header checksum of the frame rx verifies (bytes 14..33) */
static int ip_csum_ok(const uint8_t *rx)
{
	return csum_fold(csum_add(0, rx + 14, 20)) == 0xFFFF;
}
/* 1: the TCP checksum verifies: pseudo header (src IP, dst IP, 0, protocol 6, TCP length) + TCP header + payload */
static int tcp_csum_ok(const uint8_t *rx, int tcp_len)
{
	uint8_t pseudo[12];

	memcpy(pseudo, rx + 26, 8);				// source IP, destination IP
	pseudo[8] = 0;
	pseudo[9] = 6;							// protocol TCP
	pseudo[10] = (tcp_len >> 8) & 0xFF;
	pseudo[11] = tcp_len & 0xFF;
	return csum_fold(csum_add(csum_add(0, pseudo, 12), rx + 34, tcp_len)) == 0xFFFF;
}
/* One byte of a header dump; the fields the MAC changes (Table 567) are bracketed: [xx] */
static void ts_dump_byte(const uint8_t *f, int i, int mark_from, int mark_to)
{
	if (i >= mark_from && i <= mark_to) {
		eth_printf("%s%02X%s", (i == mark_from) ? "[" : " ", f[i], (i == mark_to) ? "]" : "");
	} else {
		eth_printf(" %02X", f[i]);
	}
}
/* Dump of one TSO frame: Ethernet, IP (len, ID, csum bracketed), TCP (seq, flags, csum bracketed), then the
 * first and last TS_DUMP_PAY payload bytes (all of them when the payload is short) */
static void ts_frame_dump(const char *title, const uint8_t *f, int len)
{
	int pay = len - TS_HDR_LEN;
	int i;

	eth_printf("      %s: %d bytes = header %d + payload %d\n", title, len, TS_HDR_LEN, pay);
	eth_printf("        [ 0..13] ETH    :");
	for (i = 0; i < 14; i++) {
		eth_printf(" %02X", f[i]);
	}
	eth_printf("\n        [14..33] IP     :");
	for (i = 14; i < 34; i++) {
		if (i >= 16 && i <= 17) {
			ts_dump_byte(f, i, 16, 17);				// [16..17] total length
		} else if (i >= 18 && i <= 19) {
			ts_dump_byte(f, i, 18, 19);				// [18..19] identification
		} else if (i >= 24 && i <= 25) {
			ts_dump_byte(f, i, 24, 25);				// [24..25] header checksum
		} else {
			eth_printf(" %02X", f[i]);
		}
	}
	eth_printf("   len[16..17] ID[18..19] csum[24..25]\n        [34..53] TCP    :");
	for (i = 34; i < 54; i++) {
		if (i >= 38 && i <= 41) {
			ts_dump_byte(f, i, 38, 41);				// [38..41] sequence number
		} else if (i == 47) {
			ts_dump_byte(f, i, 47, 47);				// [47] flags
		} else if (i == 50 || i == 51) {
			ts_dump_byte(f, i, 50, 51);				// [50..51] checksum
		} else {
			eth_printf(" %02X", f[i]);
		}
	}
	eth_printf("   seq[38..41] flags[47] csum[50..51]\n");
	if (pay <= 0) {
		eth_printf("        no payload\n");
		return;
	}
	if (pay <= 2 * TS_DUMP_PAY) {
		eth_printf("        [%2d..%4d] payload:", TS_HDR_LEN, len - 1);
		for (i = TS_HDR_LEN; i < len; i++) {
			eth_printf(" %02X", f[i]);
		}
		eth_printf("\n");
		return;
	}
	eth_printf("        [%2d..%4d] payload:", TS_HDR_LEN, TS_HDR_LEN + TS_DUMP_PAY - 1);
	for (i = TS_HDR_LEN; i < TS_HDR_LEN + TS_DUMP_PAY; i++) {
		eth_printf(" %02X", f[i]);
	}
	eth_printf("\n        [%4d..%4d] ...    :", len - TS_DUMP_PAY, len - 1);
	for (i = len - TS_DUMP_PAY; i < len; i++) {
		eth_printf(" %02X", f[i]);
	}
	eth_printf("\n");
}
/* Build the unsegmented frame: DA | SA | 0x0800 | IPv4 | TCP (PSH ACK) | payload bytes 0, 1, 2, ... */
static void ts_build(int pay_len)
{
	unsigned char da[ETH_TEST_MAC_SIZE] = ETH_TEST_DEST_MAC;
	unsigned char sa[ETH_TEST_MAC_SIZE] = ETH_TEST_SRC_MAC;
	int ip_len = 20 + 20 + pay_len;				// IP total length of the whole frame
	int i;

	memcpy(ts_frame, da, ETH_TEST_MAC_SIZE);
	memcpy(ts_frame + ETH_TEST_MAC_SIZE, sa, ETH_TEST_MAC_SIZE);
	ts_frame[12] = 0x08;						// EtherType 0x0800 = IPv4
	ts_frame[13] = 0x00;
	/* IPv4 header */
	ts_frame[14] = 0x45;						// version 4, header 5 words
	ts_frame[15] = 0x00;						// TOS
	ts_frame[16] = (ip_len >> 8) & 0xFF;		// total length
	ts_frame[17] = ip_len & 0xFF;
	ts_frame[18] = (TS_IP_ID >> 8) & 0xFF;		// identification
	ts_frame[19] = TS_IP_ID & 0xFF;
	ts_frame[20] = 0x40;						// flags DF, fragment offset 0
	ts_frame[21] = 0x00;
	ts_frame[22] = 64;							// TTL
	ts_frame[23] = 6;							// protocol TCP
	ts_frame[24] = 0x00;						// header checksum: left to the MAC
	ts_frame[25] = 0x00;
	ts_frame[26] = 192;							// source 192.168.1.10
	ts_frame[27] = 168;
	ts_frame[28] = 1;
	ts_frame[29] = 10;
	ts_frame[30] = 192;							// destination 192.168.1.20
	ts_frame[31] = 168;
	ts_frame[32] = 1;
	ts_frame[33] = 20;
	/* TCP header */
	ts_frame[34] = 0x9C;						// source port 40000
	ts_frame[35] = 0x40;
	ts_frame[36] = 0x00;						// destination port 80
	ts_frame[37] = 0x50;
	ts_frame[38] = (TS_TCP_SEQ >> 24) & 0xFF;	// sequence number
	ts_frame[39] = (TS_TCP_SEQ >> 16) & 0xFF;
	ts_frame[40] = (TS_TCP_SEQ >> 8) & 0xFF;
	ts_frame[41] = TS_TCP_SEQ & 0xFF;
	ts_frame[42] = 0x00;						// ack number 0
	ts_frame[43] = 0x00;
	ts_frame[44] = 0x00;
	ts_frame[45] = 0x00;
	ts_frame[46] = TS_THL << 4;					// data offset 5 words
	ts_frame[47] = TS_TCP_FLAGS;				// PSH | ACK
	ts_frame[48] = 0x20;						// window 0x2000
	ts_frame[49] = 0x00;
	ts_frame[50] = 0x00;						// checksum: left to the MAC
	ts_frame[51] = 0x00;
	ts_frame[52] = 0x00;						// urgent pointer
	ts_frame[53] = 0x00;
	/* payload */
	for (i = 0; i < pay_len; i++) {
		ts_frame[TS_HDR_LEN + i] = (uint8_t)i;
	}
}
/**
 * Check segment i of n and print one line.
 *
 * What the MAC should have done (RM0399 Table 567 p.2994). The CPU sent ONE frame:
 *
 *     ts_frame:  [ ETH 14 | IP 20 | TCP 20 | payload 0 1 2 3 ... pay_len-1 ]
 *
 * and the DMA cut the payload into n pieces of mss bytes (the last one is shorter), each behind a copy
 * of the same header. Segment i that comes back must look like:
 *
 *     rx:        [ ETH 14 | IP 20 | TCP 20 | payload bytes i*mss .. i*mss+seg-1 ]
 *                   same    len = 40+seg   seq = SEQ + i*mss     same bytes as ts_frame + 54 + i*mss
 *                           ID  = ID + i   PSH only in the last
 *                           csum fixed     csum fixed
 *
 * So the check is: (1) the length, (2) the Ethernet header unchanged, (3) the IP header: 2 fields changed
 * (total length, ID), 1 recomputed (checksum), the rest unchanged, (4) the TCP header: 2 fields changed
 * (seq, flags), 1 recomputed (checksum), the rest unchanged, (5) the payload bytes = the right slice.
 * The two checksums were sent as 0, so "verifies" means the MAC computed them.
 *
 * @param rx       the frame received (ts_rxbuf), rx_len bytes
 * @param i, n     segment number 0..n-1, number of segments expected
 * @param mss      payload bytes per segment (the context descriptor), pay_len = payload of the frame sent
 * Return 1: every field as Table 567 says, 0: else
 */
static int ts_check_seg(const uint8_t *rx, int rx_len, int i, int n, int mss, int pay_len, const char *id)
{
	int seg = pay_len - i * mss;				// payload bytes expected in this segment (what is left ...)
	int last = (i == n - 1);
	uint8_t exp_flags = last ? TS_TCP_FLAGS : (TS_TCP_FLAGS & (uint8_t)~TS_TCP_PSH);	// PSH cleared except in the last
	unsigned ip_len, ip_id;
	unsigned long seq;
	int ok_len, ok_eth, ok_ip, ok_ipcs, ok_tcp, ok_flags, ok_tcpcs, ok_pay, ok;

	if (seg > mss) {
		seg = mss;								// ... but at most mss. Example pay 3000, mss 1460: 1460, 1460, 80
	}
	if (rx_len < TS_HDR_LEN) {
		eth_printf("  %s.%d  %4d  too short\n", id, i, rx_len);	// no header to read: stop here
		return 0;
	}
	/* fields the MAC must change, read as big-endian numbers from the received bytes */
	ip_len = ((unsigned)rx[16] << 8) | rx[17];	// IP total length: IP header + TCP header + payload of THIS segment
	ip_id = ((unsigned)rx[18] << 8) | rx[19];	// IP identification: the one sent + i
	seq = ((unsigned long)rx[38] << 24) | ((unsigned long)rx[39] << 16) | ((unsigned long)rx[40] << 8) | rx[41];
												// TCP sequence number: the one sent + i*mss

	/* (1) length: header + this segment's payload (FCS already stripped by MAC_Configuration.CST) */
	ok_len = (rx_len == TS_HDR_LEN + seg);
	/* (2) Ethernet header: DA, SA, EtherType copied as they are */
	ok_eth = (memcmp(rx, ts_frame, 14) == 0);
	/* (3) IP header: total length and ID changed, every other field copied */
	ok_ip = (ip_len == 20 + 20 + (unsigned)seg)							// total length of this segment
		 && (ip_id == TS_IP_ID + (unsigned)i)							// ID + 1 per segment
		 && (rx[14] == 0x45) && (rx[15] == 0x00)							// version/IHL, TOS
		 && (memcmp(rx + 20, ts_frame + 20, 4) == 0)						// flags/frag, TTL, protocol
		 && (memcmp(rx + 26, ts_frame + 26, 8) == 0);					// source, destination IP
	/* (3b) IP checksum: recomputed over the 20 header bytes above; sent as 0, so only the MAC can make it right */
	ok_ipcs = ip_csum_ok(rx);
	/* (4) TCP header: seq changed, every other field copied (flags checked apart, they have their own rule) */
	ok_tcp = (seq == TS_TCP_SEQ + (unsigned long)i * (unsigned long)mss)	// seq + MSS per segment
		  && (memcmp(rx + 34, ts_frame + 34, 4) == 0)						// ports
		  && (memcmp(rx + 42, ts_frame + 42, 4) == 0)						// ack
		  && (rx[46] == ts_frame[46])										// data offset
		  && (memcmp(rx + 48, ts_frame + 48, 2) == 0);					// window
	/* (4b) flags: PSH (and FIN) must be cleared in every segment but the last, which keeps the original */
	ok_flags = (rx[47] == exp_flags);
	/* (4c) TCP checksum: over pseudo header + TCP header + payload of THIS segment (rx_len - 34 bytes) */
	ok_tcpcs = ok_len && tcp_csum_ok(rx, rx_len - 34);
	/* (5) payload: bytes 54.. of the segment = bytes i*mss .. of the payload sent */
	ok_pay = ok_len && (memcmp(rx + TS_HDR_LEN, ts_frame + TS_HDR_LEN + i * mss, (size_t)seg) == 0);

	ok = ok_len && ok_eth && ok_ip && ok_ipcs && ok_tcp && ok_flags && ok_tcpcs && ok_pay;
	/* one line per segment: id.i, length, IP.len, ID, TCP.seq offset, flags, the 3 computed checks, result */
	eth_printf("  %s.%d  %4d  %4u  0x%04X  +%-5lu  0x%02X   %s     %s      %s   | %s%s%s\n",
			   id, i, rx_len, ip_len, ip_id, seq - TS_TCP_SEQ, rx[47],
			   ok_ipcs ? "OK " : "BAD", ok_tcpcs ? "OK " : "BAD", ok_pay ? "OK " : "BAD",
			   ok ? "OK " : "BAD", (ok_eth && ok_ip && ok_tcp) ? "" : " header field changed",
			   ok_flags ? "" : " flags");
	return ok;
}
/**
 * One case: (context MSS) -> one TSO descriptor -> n segments back -> no extra frame -> MMC TX good + n.
 *
 * Ring usage, example case A (mss 1000, payload 3000, n = 3), tx_head = 1 at the start:
 *
 *     tx_desc[1] = context  TDES2.MSS = 1000, TDES3 = OWN CTXT TCMSSV      (egress_context_mss, no tail write)
 *     tx_desc[2] = frame    TDES0 = header, TDES1 = payload, TDES3 = OWN FD LD TSE THL TPL
 *                           tail = &tx_desc[3]  -> the DMA reads [1] (stores 1000), then [2], cuts, sends 3 frames
 *     rx_desc[k], [k+1], [k+2] = the 3 segments, 1054 bytes each, read one by one with ingress_packet_status()
 *
 * The DMA closes tx_desc[2] (OWN = 0) only after the 3rd segment is in the MTL queue, so when egress_tso()
 * returns, all the segments are already on their way back: the RX ring may hold 2-3 frames at once.
 *
 * @param ctx      TS_NEW_CTX: context descriptor with mss before the frame, TS_NO_CTX: none, the DMA keeps
 *                 the MSS of the previous context (case C: shows the value is stored, like the VLAN tag),
 *                 TS_REG_MSS: mss written in DMA_CH0_Control.MSS, no context (case D: the register path)
 * @param mss      payload bytes per segment; pay_len payload bytes of the frame sent
 * Return 1: as expected, 0: else
 */
static int ts_case(uint8_t mac_idx, const char *id, int ctx, int mss, int pay_len, const char *what)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t ctx_idx = 0;						// ring index of the context descriptor (to read its write-back)
	uint32_t ctx_des3 = 0;						// its TDES3 after the DMA closed it
	uint32_t dmaccr;							// TS_REG_MSS: DMA_CH0_Control with the new MSS
	uint32_t rdes3 = 0;							// RDES3 write-back of the last frame received
	uint32_t tx_good0, tx_good1;				// MMC "TX packets good" before / after: counts the segments sent
	int n = (pay_len + mss - 1) / mss;			// segments expected = payload / mss rounded up (3000/1000 = 3, 3000/1460 = 3, 500/1460 = 1)
	int i, tx, rx_len, ok = 1;

	eth_printf("  %s  MSS %d, payload %d -> expect %d frame(s): %s\n", id, mss, pay_len, n, what);
	ts_build(pay_len);							// the big frame: 54-byte header + pay_len bytes 0, 1, 2, ... in ts_frame
	ts_frame_dump("frame sent (one TSO descriptor, checksums 0)", ts_frame, TS_HDR_LEN + pay_len);
	tx_good0 = AX_READ_MAC_REG32(mac_idx, MMC_STATS_TX_PKT_GOOD);

	/* 1. context descriptor "MSS = mss": written in tx_desc[head], head + 1, NO tail write (the DMA does not run yet).
	 *    TS_REG_MSS: the same value goes to DMA_CH0_Control.MSS [13:0] (p.3078) and no context is written */
	if (ctx == TS_NEW_CTX) {
		ctx_idx = egress_context_mss((uint16_t)mss);
	} else if (ctx == TS_REG_MSS) {
		dmaccr = (AX_BASE_READ_REG32(mac_base, R_DMA_CH0_CONTROL) & ~(uint32_t)DMA_CH0_CONTROL_MSS_MASK)	// keep PBLX8, DSL
			   | ((uint32_t)mss & DMA_CH0_CONTROL_MSS_MASK);										// MSS [13:0]
		AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_CONTROL, dmaccr);
		flow_regw("DMA_CH0_Control (MSS)", mac_base, R_DMA_CH0_CONTROL, dmaccr, FF_DMA_CH0_CONTROL);
	}
	/* 2. the frame: next descriptor, header in buffer 1, payload in buffer 2, TSE = 1. egress_tso() writes the
	 *    tail past both descriptors, the DMA reads the context then the frame, sends every segment, then
	 *    closes the frame descriptor. egress_tso() returns when OWN = 0, i.e. after the LAST segment */
	tx = egress_tso(ts_frame, TS_HDR_LEN, pay_len, TS_THL);
	if (ctx == TS_NEW_CTX) {
		ctx_des3 = tx_desc[ctx_idx].des3;		// read after the frame: the DMA closed the context before it (OWN = 0, CDE = 0)
	}
	if (tx < 0) {
		eth_printf("  %s  TX error %d\n", id, tx);	// -2 busy, -3 timeout, -4 TDES3.ES
		return 0;
	}
	if (ctx == TS_NEW_CTX && (ctx_des3 & (TDES3_OWN | TDES3_CTXT_CDE)) != 0U) {
		eth_printf("  %s  context TDES3=0x%08lX: not closed by the DMA, or CDE\n", id, (unsigned long)ctx_des3);
		ok = 0;									// CDE [23] = context was in the middle of a frame (p.2996)
	}
	/* 3. the segments come back through the loopback, one frame per RX descriptor, in order.
	 *    Each one is read into ts_rxbuf and checked against ts_frame (header rules + payload slice i) */
	for (i = 0; i < n; i++) {
		rx_len = ingress_packet_status(mac_idx, ts_rxbuf, sizeof(ts_rxbuf), ETH_TEST_RX_TIMEOUT_US, NULL, &rdes3);
		if (rx_len < 0) {
			eth_printf("  %s.%d  RX error %d (RDES3=0x%08lX)\n", id, i, rx_len, (unsigned long)rdes3);	// -1 nothing came
			ok = 0;
			break;								// segment i missing: the later ones are not waited for
		}
		if (!ts_check_seg(ts_rxbuf, rx_len, i, n, mss, pay_len, id)) {
			ok = 0;								// the line printed by ts_check_seg says which field is wrong
		}
		ts_frame_dump("segment received", ts_rxbuf, rx_len);
	}
	/* 4. two independent proofs that exactly n frames were made:
	 *    - a short wait (2 ms) for one more frame must time out (-1): the DMA did not send a 4th piece
	 *    - the MAC statistics counter of good TX packets grew by n: the MAC saw n frames, not 1 */
	rx_len = ingress_packet_status(mac_idx, ts_rxbuf, sizeof(ts_rxbuf), TS_EXTRA_US, NULL, &rdes3);
	tx_good1 = AX_READ_MAC_REG32(mac_idx, MMC_STATS_TX_PKT_GOOD);
	if (rx_len != -1 || tx_good1 - tx_good0 != (uint32_t)n) {
		ok = 0;
	}
	eth_printf("  %s  extra frame: %s, MMC TX good +%lu, ctx TDES3 ", id, (rx_len == -1) ? "none" : "YES",
			   (unsigned long)(tx_good1 - tx_good0));
	if (ctx == TS_NEW_CTX) {
		eth_printf("0x%08lX", (unsigned long)ctx_des3);	// expected 0x40000000: CTXT only, OWN 0, CDE 0
	} else if (ctx == TS_REG_MSS) {
		eth_printf("- (DMA_CH0_Control.MSS = %d)", mss);
	} else {
		eth_printf("-");
	}
	eth_printf(" | %s\n", ok ? "OK " : "BAD");
	return ok;
}
/**
 * TSO test through the MAC loopback (eth_run_test_ops = 6), cases A-D.
 * Same bits as eth_mac_internal_lb() (eth_functions.c) plus:
 *
 *   Register / descriptor  Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   MAC_HW_Feature1 p.3144    TSOEN [18]           1       read: TSO exists on this core (else the test stops)
 *   DMA_CH0_Tx_Control p.3079    TSE [12]             1       DMA segments the frames with TDES3.TSE = 1 (TXPBL >= 4)
 *   TDES3 context p.3059   CTXT [30], TCMSSV [26] 1     context descriptor with a valid MSS, OSTC [27] = 0
 *   TDES2 context p.3059   MSS [13:0]           mss     payload bytes per segment
 *   DMA_CH0_Control p.3078      MSS [13:0]           1000    case D: the MSS from the register instead of a context
 *   TDES3 frame p.3052     TSE [18]             1       segment this frame
 *                          THL [22:19]          5       TCP header length in 32-bit words
 *                          TPL [17:0]           pay_len TCP payload length
 *   TDES2 frame p.3051     HL [9:0]             54      header length (Ethernet + IP + TCP) in buffer 1
 *                          B2L [29:16]          pay_len payload in buffer 2
 * Return 1: every case as expected, 0: else
 */
uint16_t eth_mac_tso_lb(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t machwf1r;
	uint32_t dmactxcr;
	uint32_t ok = 0;
	eth_printf("Start Init: Port[%d]\n", mac_idx);

	if (eth_reset_mac(mac_idx) == 0) {
		return 0;
	}
	mac_init_100m(mac_idx);
	/* TSO must exist on this core: MAC_HW_Feature1.TSOEN (p.3144) */
	machwf1r = AX_BASE_READ_REG32(mac_base, R_MAC_HW_FEATURE1);
	eth_printf("MAC_HW_Feature1 = 0x%08lX: TSOEN [18] = %d\n", (unsigned long)machwf1r, (machwf1r & MAC_HW_FEATURE1_TSOEN) != 0);
	if ((machwf1r & MAC_HW_FEATURE1_TSOEN) == 0U) {
		eth_printf("TSO is not available on this core, test stopped\n");
		return 0;
	}
	/* TSE = 1 while the TX DMA is still stopped (mac_start() sets ST) */
	dmactxcr = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_TX_CONTROL)
			 | DMA_CH0_TX_CONTROL_TSE;										// TSE [12]: segment the frames with TDES3.TSE
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_TX_CONTROL, dmactxcr);
	FLOW("[8] TSO:\n");
	flow_regw("DMA_CH0_Tx_Control (TSE)", mac_base, R_DMA_CH0_TX_CONTROL, dmactxcr, FF_DMA_CH0_TX_CONTROL);
	init_mac_internal_lb(mac_idx);
	mac_start(mac_idx);

	eth_printf("\n================ TSO TEST (MSS context descriptor, TDES3.TSE, DMA_CH0_Tx_Control.TSE) ================\n");
	eth_printf("  one TCP/IP frame in one descriptor (header %d bytes in buffer 1, payload in buffer 2, TSE = 1)\n", TS_HDR_LEN);
	eth_printf("  comes back as frames of <= MSS payload bytes. IP.len / ID / TCP.seq / flags per segment as RM0399 Table 567;\n");
	eth_printf("  both checksums were sent as 0, so a good one is the MAC's work. Case B prints the [10]/[11] FLOW lines.\n");
	eth_printf("  Dumps: the frame sent once per case, every segment received under its line; [..] = fields the MAC changes;\n");
	eth_printf("  payload = first and last %d bytes.\n", TS_DUMP_PAY);
	eth_printf("  id    len  IP.len  ID      TCP.seq flags  ip.csum tcp.csum payload | result\n");

	eth_flow_trace = 1;
	ok += ts_case(mac_idx, "A", TS_NEW_CTX, TS_MSS_A, TS_PAYLOAD,   "3 equal segments");
	eth_flow_trace = 1;
	ok += ts_case(mac_idx, "B", TS_NEW_CTX, TS_MSS_B, TS_PAYLOAD,   "new context: 1460 + 1460 + 80");
	eth_flow_trace = 1;
	ok += ts_case(mac_idx, "C", TS_NO_CTX,  TS_MSS_B, TS_PAYLOAD_C, "no new context, payload <= MSS: not cut, checksums filled");
	ok += ts_case(mac_idx, "D", TS_REG_MSS, TS_MSS_A, TS_PAYLOAD,   "DMA_CH0_Control.MSS = 1000, no context: register path replaces the stored 1460");

	eth_printf("\nResult: %lu/4 cases as expected.\n", (unsigned long)ok);
	eth_print_mac_counters(mac_idx);
	return (ok == 4) ? 1 : 0;
}
