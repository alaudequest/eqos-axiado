#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_vlan_utils.h"
#include "eth_traffic_test.h"
#include "eth_printf.h"
#include "eth_flow_utils.h"

/*
 * VLAN insertion tests through the MAC loopback: what a TX context descriptor does.
 * A context descriptor is a ring entry with no buffer (TDES3.CTXT = 1) that carries a parameter for the
 * frames after it. Here the parameter is a VLAN tag: the CPU writes "insert tag X" once, then every frame
 * sent with TDES2.VTIR = insert comes out of the MAC 4 bytes longer, with the tag after the source
 * address. The CPU never touches the frame bytes: the looped-back frame shows what the MAC added.
 *
 * Test 1 (eth_run_test_ops = 4, eth_mac_vlan_lb): one tag (C-VLAN 0x8100), frame 60 bytes, EtherType 0x88B5:
 *   A  no context,     VTIR = none    -> 60 bytes, unchanged
 *   B  context tag A,  VTIR = insert  -> 64 bytes, 81 00 00 64 at bytes 12..15
 *   C  no new context, VTIR = insert  -> 64 bytes, tag A again (the context stays valid)
 *   D  no new context, VTIR = none    -> 60 bytes, no tag (VTIR decides per frame)
 *   E  context tag B,  VTIR = insert  -> 64 bytes, 81 00 E0 C8 (new context replaces the old one)
 *   F  a WRONG context alone (CTXT + FD + LD, p.3060) -> the DMA closes it with CDE [23] = 1 and sets
 *      DMA_CH0_Status.CDE [13]. Last case: after it the TX DMA is only recovered by a software reset (p.3060).
 * Test 2 (eth_run_test_ops = 5, eth_mac_qinq_lb): two tags (QinQ, 802.1ad: an operator wraps the customer's
 * C-VLAN in its own S-VLAN 0x88A8). MAC_VLAN_Incl.CSVL = 1 makes the outer tag an S-tag. The inner tag has two
 * control paths: MAC_Inner_VLAN_Incl.VLP = 1 -> MAC_Inner_VLAN_Incl.VLC for every frame; VLP = 0 -> IVTIR [19:18] of the context:
 *   A  MAC_Inner_VLAN_Incl VLP=1 VLC=insert, context outer S 0x07D0 + inner C 0x0064, VTIR = insert -> 68 bytes, 88 A8 07 D0 81 00 00 64
 *   B  MAC_Inner_VLAN_Incl off, no new context, VTIR = insert             -> 64 bytes, 88 A8 07 D0 (outer stays, inner is per frame)
 *   C  MAC_Inner_VLAN_Incl VLP=0, context IVTIR = insert, VTIR = insert    -> 68 bytes, same as A, decided by the descriptor
 *   D  frame sent WITH two tags (S 07D0, C E0C8), context IVTIR = replace inner by 0064, VTIR = none -> 68 bytes, 88 A8 07 D0 81 00 00 64
 *   E  same frame, context IVTIR = remove, VTIR = none         -> 64 bytes, 88 A8 07 D0 then the EtherType
 *
 * RM0399: TX context descriptor p.3057-3061 (inner tag p.3058-3059, IVTIR and CDE p.3060); TDES2.VTIR p.3050;
 * MAC_VLAN_Incl p.3126; MAC_Inner_VLAN_Incl p.3127; MAC_VLAN_Tag p.3122-3124; RDES0.OVT/IVT p.3065; RDES3.LT p.3069;
 * DMA_CH0_Status.CDE p.3095.
 */

#define VT_FRAME_LEN	60		// frame sent (no FCS); + 4 per tag on the wire
#define VT_TAG_A		0x0064	// C-VLAN: priority 0, VID 100
#define VT_TAG_B		0xE0C8	// C-VLAN: priority 7, VID 200
#define VT_TAG_S		0x07D0	// S-VLAN (outer, operator): priority 0, VID 2000
#define VT_TPID_S_HI	0x88	// S-tag type 0x88A8 (802.1ad), byte 12 / 13 of a tagged frame
#define VT_TPID_S_LO	0xA8
#define VT_TPID_C_HI	0x81	// C-tag type 0x8100 (802.1Q)
#define VT_TPID_C_LO	0x00
#define VT_DUMP_PAYLOAD	16		// payload bytes shown by vt_frame_dump()
#define VT_NO_CTX		0		// vlan_case(): no context descriptor before the frame
#define VT_CTX_ONE		1		// vlan_case(): context with one tag (outer)
#define VT_CTX_QINQ		2		// vlan_case(): context with outer + inner tag (+ IVTIR)

static uint8_t vt_frame[MAC_100M_BUF_SIZE];		// frame sent, no tag: DA SA 88B5 payload (60 bytes)
static uint8_t vt_frame2[MAC_100M_BUF_SIZE];	// frame sent with two tags: DA SA 88A8 07D0 8100 E0C8 88B5 payload (68 bytes)
static uint8_t vt_rxbuf[MAC_100M_BUF_SIZE];		// frame received

/* Expected bytes 12.. of the RX frame for each tagged case (the EtherType 88 B5 follows them) */
static const uint8_t VT_HDR_A[4]    = {0x81, 0x00, 0x00, 0x64};
static const uint8_t VT_HDR_B[4]    = {0x81, 0x00, 0xE0, 0xC8};
static const uint8_t VT_HDR_S[4]    = {0x88, 0xA8, 0x07, 0xD0};
static const uint8_t VT_HDR_QINQ[8] = {0x88, 0xA8, 0x07, 0xD0, 0x81, 0x00, 0x00, 0x64};
static const uint8_t VT_HDR_SENT2[8] = {0x88, 0xA8, 0x07, 0xD0, 0x81, 0x00, 0xE0, 0xC8};	// the two tags in vt_frame2

/* Build the frames once: vt_frame = DA | SA | EtherType 0x88B5 | payload 0x10..; vt_frame2 = the same with
 * S tag 07D0 + C tag E0C8 already between SA and EtherType (what a double-tagged frame looks like on the wire) */
static void vlan_build_frame(void)
{
	unsigned char da[ETH_TEST_MAC_SIZE] = ETH_TEST_DEST_MAC;
	unsigned char sa[ETH_TEST_MAC_SIZE] = ETH_TEST_SRC_MAC;
	uint32_t i;

	memcpy(vt_frame, da, ETH_TEST_MAC_SIZE);
	memcpy(vt_frame + ETH_TEST_MAC_SIZE, sa, ETH_TEST_MAC_SIZE);
	vt_frame[12] = (ETH_TEST_ETHER_TYPE >> 8) & 0xFF;
	vt_frame[13] = ETH_TEST_ETHER_TYPE & 0xFF;
	for (i = ETH_TEST_HEADER_SIZE; i < VT_FRAME_LEN; i++) {
		vt_frame[i] = (uint8_t)(0x10 + (i % 0x60));
	}
	memcpy(vt_frame2, vt_frame, 12);							// DA, SA
	memcpy(vt_frame2 + 12, VT_HDR_SENT2, 8);					// 88 A8 07 D0 | 81 00 E0 C8
	memcpy(vt_frame2 + 20, vt_frame + 12, VT_FRAME_LEN - 12);	// EtherType + payload
}
/* Column header of the result table; vlan_case() / vlan_case_cde() print their line with the same widths */
#define VT_TABLE_HDR	"  id  len  bytes 12..21                     LT  RDES0       ctx TDES3   | result"

/* Name of a TDES2.VTIR / TDES3.IVTIR value: both use 00 none, 01 remove, 10 insert, 11 replace (p.3050, p.3060) */
static const char *vt_ir_name(uint8_t v)
{
	switch (v) {
	case TDES2_VTIR_REMOVE:  return "remove";
	case TDES2_VTIR_INSERT:  return "insert";
	case TDES2_VTIR_REPLACE: return "replace";
	default:                 return "none";
	}
}
/**
 * Dump of one frame split at the VLAN tags: DA | SA | tag(s) | EtherType | payload. A tag is 4 bytes that start
 * with 88 A8 (S-tag) or 81 00 (C-tag); the 2 bytes after the type hold PCP [15:13], DEI [12], VID [11:0].
 */
static void vt_frame_dump(const char *title, const uint8_t *f, int len)
{
	int i, p = 12;	// first byte after DA + SA

	eth_printf("      %s: %d bytes\n", title, len);
	eth_printf("        [ 0.. 5] DA     :");
	for (i = 0; i < 6; i++) {
		eth_printf(" %02X", f[i]);
	}
	eth_printf("\n        [ 6..11] SA     :");
	for (i = 6; i < 12; i++) {
		eth_printf(" %02X", f[i]);
	}
	eth_printf("\n");
	while (p + 4 <= len
		   && ((f[p] == VT_TPID_S_HI && f[p + 1] == VT_TPID_S_LO) || (f[p] == VT_TPID_C_HI && f[p + 1] == VT_TPID_C_LO))) {
		eth_printf("        [%2d..%2d] %s  : %02X %02X %02X %02X   (PCP %d, VID %d)\n", p, p + 3,
				   (f[p] == VT_TPID_S_HI) ? "S-tag" : "C-tag", f[p], f[p + 1], f[p + 2], f[p + 3],
				   f[p + 2] >> 5, ((f[p + 2] & 0x0F) << 8) | f[p + 3]);
		p += 4;
	}
	if (p + 2 <= len) {
		eth_printf("        [%2d..%2d] type   : %02X %02X\n", p, p + 1, f[p], f[p + 1]);
		p += 2;
	}
	eth_printf("        [%2d..%2d] payload:", p, len - 1);
	for (i = p; i < p + VT_DUMP_PAYLOAD && i < len; i++) {
		eth_printf(" %02X", f[i]);
	}
	eth_printf("%s (%d bytes)\n", (len - p > VT_DUMP_PAYLOAD) ? " ..." : "", len - p);
}
/**
 * One case: (context descriptor) -> frame with TDES2.VTIR = vtir -> receive -> one result line.
 * The frame received must be: frame[0..11] | ins (ins_len bytes) | frame[12 + skip ..len-1]
 *   insertion:   skip = 0, ins = the tag(s) the MAC adds
 *   replacement: skip = bytes of tag(s) in the frame sent that disappear, ins = what stands there now
 *   unchanged:   skip = 0, ins_len = 0
 * @param ctx    VT_NO_CTX, VT_CTX_ONE (tag = outer) or VT_CTX_QINQ (tag = outer, inner, ivtir = TDES3_CTXT_IVTIR_*)
 * @param vtir   TDES2.VTIR of the frame: TDES2_VTIR_NONE or TDES2_VTIR_INSERT (outer tag)
 * @param frame  bytes sent, len of them (vt_frame 60 or vt_frame2 68)
 * Return 1: as expected, 0: else
 */
static int vlan_case(uint8_t mac_idx, const char *id, int ctx, uint16_t tag, uint16_t inner, uint8_t ivtir,
					 uint8_t vtir, const uint8_t *frame, int len, int skip, const uint8_t *ins, int ins_len,
					 const char *what)
{
	uint32_t ctx_idx = 0;
	uint32_t ctx_des3 = 0;
	uint32_t rdes0, rdes3 = 0;
	uint32_t wait, k;
	int exp_len = len - skip + ins_len;
	int tx_len, rx_len, ok;
	int len_ok, addr_ok, tag_ok, rest_ok, ctx_ok;

	/* 0. description, setup and expected result of this case, before anything is sent */
	eth_printf("\n  --- case %s: %s\n", id, what);
	eth_printf("ctx: %d (0 none, 1 one tag, 2 outer + inner)\n", ctx);
	eth_printf("      setup : ");
	if (ctx == VT_NO_CTX) {
		eth_printf("no new context descriptor (the one stored in the MAC, if any, stays)");
	} else if (ctx == VT_CTX_ONE) {
		eth_printf("context descriptor: TDES3.VT = 0x%04X, VLTV = 1", tag);
	} else {
		eth_printf("context descriptor: outer TDES3.VT = 0x%04X, inner TDES2.IVT = 0x%04X, IVTIR = %s",
				   tag, inner, vt_ir_name(ivtir));
	}
	eth_printf("; frame: %d bytes, TDES2.VTIR = %s\n", len, vt_ir_name(vtir));
	eth_printf("      expect: %d bytes", exp_len);
	if (skip > 0) {
		eth_printf(", the %d tag bytes 12..%d of the frame sent gone", skip, 12 + skip - 1);
	}
	if (ins_len > 0) {
		eth_printf(", bytes 12..%d =", 12 + ins_len - 1);
		for (k = 0; k < (uint32_t)ins_len; k++) {
			eth_printf(" %02X", ins[k]);
		}
		eth_printf(", then the EtherType %02X %02X\n", frame[12 + skip], frame[13 + skip]);
	} else {
		eth_printf(", no tag: bytes 12..13 = EtherType %02X %02X as sent\n", frame[12], frame[13]);
	}
	/* 1. context descriptor: tx_desc[head], head + 1, no tail write */
	if (ctx == VT_CTX_ONE) {
		ctx_idx = egress_context_vlan(tag);
	} else if (ctx == VT_CTX_QINQ) {
		ctx_idx = egress_context_qinq(tag, inner, ivtir);
	}
	/* 2. frame: next descriptor, tail moved past both, DMA reads the context then the frame */
	eth_tx_vtir = vtir;
	tx_len = egress_packet((unsigned char *)frame, len);
	eth_tx_vtir = TDES2_VTIR_NONE;
	if (ctx != VT_NO_CTX) {
		ctx_des3 = tx_desc[ctx_idx].des3;	// closed by the DMA before the frame: OWN = 0, CDE = 0 expected
	}
	if (tx_len != len) {
		eth_printf("  %s  TX error %d\n", id, tx_len);
		return 0;
	}
	/* 3. RDES0 (OVT / IVT, the tags seen by the MAC receiver) is only in the write-back: read it before
	 *    ingress_packet_status() rewrites the descriptor */
	wait = ETH_TEST_RX_TIMEOUT_US;
	while ((rx_desc[eth_rx_head].des3 & RDES3_OWN) && wait > 0) {
		udelay(1);
		wait--;
	}
	rdes0 = rx_desc[eth_rx_head].des0;
	rx_len = ingress_packet_status(mac_idx, vt_rxbuf, sizeof(vt_rxbuf), ETH_TEST_RX_TIMEOUT_US, NULL, &rdes3);
	if (rx_len < 0) {
		eth_printf("  %s  RX error %d\n", id, rx_len);
		return 0;
	}
	/* 4. check, piece by piece. The frame received must look like this:
	 *
	 *      received:  [ DA SA ] [ ins: the tag(s) expected ] [ EtherType + payload, as sent ]
	 *                  0    11   12        12+ins_len-1        12+ins_len .. exp_len-1
	 *      sent:      [ DA SA ] [ skip bytes: old tag(s)   ] [ EtherType + payload           ]
	 *                  0    11   12          12+skip-1        12+skip .. len-1
	 */
	len_ok  = (rx_len == exp_len);												// as many bytes as expected
	addr_ok = (memcmp(vt_rxbuf, frame, 12) == 0);								// DA + SA untouched
	tag_ok  = (ins_len == 0)													// no tag expected: nothing to check
		   || (memcmp(vt_rxbuf + 12, ins, (size_t)ins_len) == 0);				// the tag bytes at 12.. are exactly ins
	rest_ok = (memcmp(vt_rxbuf + 12 + ins_len,									// after the tag(s) in the frame received
					  frame + 12 + skip,										// after the old tag(s) in the frame sent
					  (size_t)(len - 12 - skip)) == 0);							// EtherType + payload must be the same
	ctx_ok  = (ctx == VT_NO_CTX)												// no context sent: nothing to check
		   || ((ctx_des3 & (TDES3_OWN | TDES3_CTXT_CDE)) == 0U);				// taken by the DMA (OWN 0), accepted (CDE 0)
	ok = len_ok && addr_ok && tag_ok && rest_ok && ctx_ok;
	/* 5. column header + one line: id, RX length, bytes 12..21, RDES3.LT, RDES0, context TDES3, result */
	eth_printf(VT_TABLE_HDR "\n");
	eth_printf("  %s  %3d  ", id, rx_len);
	for (k = 12; k < 22; k++) {
		if ((int)k < rx_len) {
			eth_printf("%02X ", vt_rxbuf[k]);
		} else {
			eth_printf("   ");
		}
	}
	eth_printf(" %lu   0x%08lX  ", (unsigned long)((rdes3 >> RDES3_LT_bit) & RDES3_LT_MASK), (unsigned long)rdes0);
	if (ctx != VT_NO_CTX) {
		eth_printf("0x%08lX  ", (unsigned long)ctx_des3);
	} else {
		eth_printf("    -       ");
	}
	eth_printf("| %s\n", ok ? "OK " : "BAD");
	if (!ok) {
		eth_printf("      failed check(s):%s%s%s%s%s\n",
				   len_ok ? "" : " length", addr_ok ? "" : " DA/SA", tag_ok ? "" : " tag-bytes",
				   rest_ok ? "" : " rest-of-frame", ctx_ok ? "" : " context-descriptor");
		eth_printf("      expected %d bytes:", exp_len);
		for (k = 12; k < 22; k++) {
			if ((int)k < 12 + ins_len) {
				eth_printf(" %02X", ins[k - 12]);
			} else if ((int)(k - ins_len + skip) < len) {
				eth_printf(" %02X", frame[k - ins_len + skip]);
			}
		}
		eth_printf("  (bytes 12..21)\n");
	}
	/* 6. both frames in full, split at the tags: what went in, what the MAC gave back */
	vt_frame_dump("frame sent", frame, len);
	vt_frame_dump("frame received", vt_rxbuf, rx_len);
	return ok;
}
/**
 * Case F of test 1: a context descriptor the DMA must refuse. CTXT together with FD and LD is one of the
 * three CDE causes listed in RM0399 p.3060 (the others: context in the middle of a frame, descriptor all 1s).
 * No frame is sent. Expected: write-back OWN = 0 with CDE [23] = 1, and DMA_CH0_Status.CDE [13] = 1 (+ AIS [14]).
 * The TX DMA is left in that state: RM0399 says only a software reset recovers it, and every test starts
 * with eth_reset_mac(). The DMA_CH0_Status bits are cleared (write 1) so the counters print stays readable.
 * Return 1: as expected, 0: else
 */
static int vlan_case_cde(uint8_t mac_idx, const char *id, const char *what)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t ctx_des3;
	uint32_t dmacsr;
	int ok;

	/* 0. description, setup and expected result before anything is written */
	eth_printf("\n  --- case %s: %s\n", id, what);
	eth_printf("      setup : one context descriptor, TDES3 = OWN | CTXT | FD | LD | VLTV | VT 0x%04X, tail pointer written; no frame\n",
			   VT_TAG_A);
	eth_printf("      expect: write-back OWN [31] = 0 and CDE [23] = 1; DMA_CH0_Status CDE [13] = 1, AIS [14] = 1 (TX DMA stops)\n");
	/* 1. the bad context, alone: egress_context_bad() writes the tail and waits for OWN = 0 */
	ctx_des3 = egress_context_bad(VT_TAG_A);
	dmacsr = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_STATUS);
	/* 2. check: closed (OWN 0), CDE in the descriptor, CDE in the channel status */
	ok = ((ctx_des3 & TDES3_OWN) == 0U) && ((ctx_des3 & TDES3_CTXT_CDE) != 0U) && ((dmacsr & DMA_CH0_STATUS_CDE) != 0U);
	/* 3. column header + same columns as the other lines; no frame, so len / bytes / LT / RDES0 are '-' */
	eth_printf(VT_TABLE_HDR "\n");
	eth_printf("  %s    -  (no frame, bad context alone)   -   -           0x%08lX  | %s\n",
			   id, (unsigned long)ctx_des3, ok ? "OK " : "BAD");
	eth_printf("      DMA_CH0_Status = 0x%08lX: CDE [13] = %d, AIS [14] = %d, TPS [1] = %d; ctx TDES3: OWN [31] = %d, CDE [23] = %d\n",
			   (unsigned long)dmacsr, (dmacsr & DMA_CH0_STATUS_CDE) != 0, (dmacsr & DMA_CH0_STATUS_AIS) != 0, (dmacsr & DMA_CH0_STATUS_TPS) != 0,
			   (ctx_des3 & TDES3_OWN) != 0, (ctx_des3 & TDES3_CTXT_CDE) != 0);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_STATUS, dmacsr & (DMA_CH0_STATUS_CDE | DMA_CH0_STATUS_AIS | DMA_CH0_STATUS_TPS));	// write 1 to clear
	return ok;
}
/**
 * Common start of both tests: reset, MAC 100M init, VLAN registers, loopback, DMA + MAC start, frame built.
 * MAC_VLAN_Tag is the same for both (RX side: report the tags in RDES0 / RDES3.LT); MAC_VLAN_Incl / MAC_Inner_VLAN_Incl differ.
 *
 *   Register               Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   MAC_VLAN_Incl p.3126      (macvir)                     outer tag control, see each test
 *   MAC_Inner_VLAN_Incl p.3127     (macivir)                    inner tag control, see each test
 *   MAC_VLAN_Tag p.3122      EVLRXS [24]          1       outer tag reported in RDES0 (OVT, RS0V)
 *                          EDVLP [26]           1       RX processes up to two tags (LT = double VLAN)
 *                          EIVLRXS [31]         1       inner tag reported in RDES0 [31:16] when possible
 *                          ESVL [18]            1       0x88A8 counts as VLAN on RX
 * Return 1: ready, 0: MAC reset failed
 */
static int vlan_init(uint8_t mac_idx, uint32_t macvir, uint32_t macivir)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t macvtr;
	eth_printf("Start Init: Port[%d]\n", mac_idx);

	if (eth_reset_mac(mac_idx) == 0) {
		return 0;
	}
	mac_init_100m(mac_idx);
	macvtr = MAC_VLAN_TAG_EVLRXS										// EVLRXS [24]: outer tag -> RDES0
		   | MAC_VLAN_TAG_EDVLP										// EDVLP [26]: two tags on RX
		   | MAC_VLAN_TAG_EIVLRXS										// EIVLRXS [31]: inner tag -> RDES0 [31:16]
		   | MAC_VLAN_TAG_ESVL;										// ESVL [18]: 0x88A8 is a VLAN type on RX
	AX_BASE_WRITE_REG32(mac_base, R_MAC_VLAN_INCL, macvir);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_INNER_VLAN_INCL, macivir);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_VLAN_TAG, macvtr);
	FLOW("[8] VLAN:\n");
	flow_regw("MAC_VLAN_Incl", mac_base, R_MAC_VLAN_INCL, macvir, FF_MAC_VLAN_INCL);
	flow_regw("MAC_Inner_VLAN_Incl", mac_base, R_MAC_INNER_VLAN_INCL, macivir, FF_MAC_INNER_VLAN_INCL);
	flow_regw("MAC_VLAN_Tag", mac_base, R_MAC_VLAN_TAG, macvtr, FF_MAC_VLAN_TAG);
	init_mac_internal_lb(mac_idx);
	mac_start(mac_idx);
	vlan_build_frame();
	return 1;
}
/* Title + how to read the result lines (same columns for both tests) */
static void vlan_print_header(const char *title)
{
	eth_printf("\n================ %s ================\n", title);
	eth_printf("  frame sent: %d bytes, EtherType 0x%04X at bytes 12..13. With a tag the MAC puts type + tag there and\n",
			   VT_FRAME_LEN, ETH_TEST_ETHER_TYPE);
	eth_printf("  shifts the rest by 4 bytes per tag. RDES3.LT: %lu = type, %lu = one VLAN tag, %lu = two VLAN tags\n",
			   (unsigned long)RDES3_LT_TYPE, (unsigned long)RDES3_LT_VLAN, (unsigned long)RDES3_LT_DVLAN);
	eth_printf("  RDES0 = outer tag [15:0] (inner tag [31:16] only with stripping); ctx TDES3: bit 31 OWN and bit 23 CDE must be 0\n");
	eth_printf("  Each case: description, setup, expectation -> [FLOW lines] -> result line -> frame sent / received split\n");
	eth_printf("  at DA / SA / tag(s) / type / payload.\n");
}
/**
 * Test 1: one VLAN tag through the MAC loopback (eth_run_test_ops = 4), cases A-F.
 * Same bits as eth_mac_internal_lb() (eth_functions.c) plus vlan_init() and:
 *
 *   Register / descriptor  Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   MAC_VLAN_Incl p.3126      VLTI [20]            1       outer tag taken from the TX context descriptor
 *                          VLP [18]             0       insertion decided per frame by TDES2.VTIR
 *                          CSVL [19]            0       outer type 0x8100 (C-VLAN)
 *   MAC_Inner_VLAN_Incl p.3127     all                  0       no inner tag
 *   TDES3 context p.3061   CTXT [30], VLTV [16] 1       context descriptor with a valid outer tag
 *                          VT [15:0]            tag     outer tag
 *                          FD [29], LD [28]     1       case F only: invalid with CTXT -> CDE [23] write-back
 *   TDES2 p.3050           VTIR [15:14]         2 / 0   insert the outer tag / send as it is (eth_tx_vtir)
 *   DMA_CH0_Status p.3093      CDE [13], AIS [14]   read    case F: context descriptor error reported by the channel
 * Return 1: every case as expected, 0: else
 */
uint16_t eth_mac_vlan_lb(uint8_t mac_idx)
{
	uint32_t macvir;
	uint32_t ok = 0;
	int b_ok;

	macvir = MAC_VLAN_INCL_VLTI;										// VLTI [20]: outer tag from the context descriptor
	if (vlan_init(mac_idx, macvir, 0U) == 0) {
		return 0;
	}
	vlan_print_header("VLAN INSERTION TEST (TX context descriptor, TDES2.VTIR, MAC_VLAN_Incl)");
	eth_printf("  case B prints the [10]/[11] FLOW lines (context descriptor + frame)\n");

	eth_flow_trace = 1;
	ok += vlan_case(mac_idx, "A", VT_NO_CTX,  0,        0, TDES3_CTXT_IVTIR_NONE, TDES2_VTIR_NONE,   vt_frame, VT_FRAME_LEN, 0, NULL,     0, "no context, VTIR = none. Reference: the MAC must send the frame exactly as written");
	eth_flow_trace = 1;
	b_ok = vlan_case(mac_idx, "B", VT_CTX_ONE, VT_TAG_A, 0, TDES3_CTXT_IVTIR_NONE, TDES2_VTIR_INSERT, vt_frame, VT_FRAME_LEN, 0, VT_HDR_A, 4, "context with tag A (VID 100), VTIR = insert. The MAC puts 81 00 00 64 after SA, 4 bytes more");
	eth_flow_trace = 1;
	ok += b_ok;
	ok += vlan_case(mac_idx, "C", VT_NO_CTX,  0,        0, TDES3_CTXT_IVTIR_NONE, TDES2_VTIR_INSERT, vt_frame, VT_FRAME_LEN, 0, VT_HDR_A, 4, "no new context, VTIR = insert. Tag A stays stored in the MAC after case B and is used again");
	ok += vlan_case(mac_idx, "D", VT_NO_CTX,  0,        0, TDES3_CTXT_IVTIR_NONE, TDES2_VTIR_NONE,   vt_frame, VT_FRAME_LEN, 0, NULL,     0, "no new context, VTIR = none. VTIR decides per frame: no tag although a context is stored");
	ok += vlan_case(mac_idx, "E", VT_CTX_ONE, VT_TAG_B, 0, TDES3_CTXT_IVTIR_NONE, TDES2_VTIR_INSERT, vt_frame, VT_FRAME_LEN, 0, VT_HDR_B, 4, "context with tag B (PCP 7, VID 200), VTIR = insert. The new context replaces the stored tag A");
	//eth_flow_trace = 1;
	ok += vlan_case_cde(mac_idx, "F", "a WRONG context alone, CTXT + FD + LD (RM0399 p.3060). The DMA must refuse it with CDE and stop; only a software reset recovers");

	eth_printf("\nResult: %lu/6 cases as expected.\n", (unsigned long)ok);
	if (!b_ok) {
		eth_printf("Case B BAD: RM0399 p.3061 and p.3126 disagree on VLTI. Try MAC_VLAN_Incl.VLTI = 0 with VLT = the tag\n"
				   "(the tag then comes from the register, the context descriptor only enables it).\n");
	}
	eth_tx_vtir = TDES2_VTIR_NONE;
	eth_print_mac_counters(mac_idx);
	return (ok == 6) ? 1 : 0;
}
/**
 * Test 2: QinQ, two VLAN tags through the MAC loopback (eth_run_test_ops = 5), cases A-E.
 * Same bits as eth_mac_internal_lb() (eth_functions.c) plus vlan_init() and:
 *
 *   Register / descriptor  Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   MAC_VLAN_Incl p.3126      VLTI [20]            1       outer tag taken from the TX context descriptor
 *                          VLP [18]             0       insertion decided per frame by TDES2.VTIR
 *                          CSVL [19]            1       outer type 0x88A8 (S-VLAN)
 *   MAC_Inner_VLAN_Incl p.3127     VLTI [20]            1       inner tag taken from the context (TDES2.IVT)
 *                          VLP [18], VLC [17:16] 1, 2   A: insert the inner tag in every frame; B: 0 (off)
 *                          VLP [18]             0       C, D, E: the context's IVTIR decides per descriptor
 *   TDES3 context p.3061   CTXT [30], VLTV [16] 1       context descriptor with a valid outer tag
 *                          IVLTV [17]           1       + inner tag valid
 *                          IVTIR [19:18]        0/2/3/1 A: none (VLC path), C: insert, D: replace, E: remove
 *                          VT [15:0]            tag     outer tag; TDES2.IVT [31:16] inner tag
 *   TDES2 p.3050           VTIR [15:14]         2 / 0   A, B, C: insert the outer tag; D, E: frame already tagged
 * Return 1: every case as expected, 0: else
 */
uint16_t eth_mac_qinq_lb(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t macvir;
	uint32_t macivir;
	uint32_t ok = 0;

	macvir = MAC_VLAN_INCL_VLTI										// VLTI [20]: outer tag from the context descriptor
		   | MAC_VLAN_INCL_CSVL;										// CSVL [19]: outer type 0x88A8
	macivir = MAC_INNER_VLAN_INCL_VLTI										// VLTI [20]: inner tag from the context (TDES2.IVT)
			| MAC_INNER_VLAN_INCL_VLP										// VLP [18]: VLC applies
			| (MAC_VLAN_INCL_VLC_INSERT << MAC_INNER_VLAN_INCL_VLC_bit);			// VLC [17:16]: insert
	if (vlan_init(mac_idx, macvir, macivir) == 0) {
		return 0;
	}
	vlan_print_header("QinQ TEST (TX context descriptor with outer + inner tag, MAC_VLAN_Incl.CSVL, MAC_Inner_VLAN_Incl, IVTIR)");
	eth_printf("  case A prints the [10]/[11] FLOW lines (context descriptor + frame). D, E send a frame that already\n");
	eth_printf("  carries the two tags 88 A8 07 D0 81 00 E0 C8 (%d bytes) and let the context change the inner one.\n", VT_FRAME_LEN + 8);

	/* A, B: inner tag controlled by the register (MAC_Inner_VLAN_Incl.VLP = 1, VLC = insert / off) */
	eth_flow_trace = 1;
	ok += vlan_case(mac_idx, "A", VT_CTX_QINQ, VT_TAG_S, VT_TAG_A, TDES3_CTXT_IVTIR_NONE,    TDES2_VTIR_INSERT, vt_frame,  VT_FRAME_LEN,     0, VT_HDR_QINQ, 8, "MAC_Inner_VLAN_Incl VLP = 1, VLC = insert: the register adds the inner tag to every frame. Context outer S 07D0 + inner C 0064, VTIR = insert");
	//eth_flow_trace = 0;
	macivir = 0U;												// inner insertion off
	AX_BASE_WRITE_REG32(mac_base, R_MAC_INNER_VLAN_INCL, macivir);
	ok += vlan_case(mac_idx, "B", VT_NO_CTX,   0,        0,        TDES3_CTXT_IVTIR_NONE,    TDES2_VTIR_INSERT, vt_frame,  VT_FRAME_LEN,     0, VT_HDR_S,    4, "MAC_Inner_VLAN_Incl = 0 (inner insertion off), no new context, VTIR = insert. Only the outer S tag of the stored context");
	/* C, D, E: inner tag controlled by the context descriptor (MAC_Inner_VLAN_Incl.VLP = 0, IVTIR decides) */
	macivir = MAC_INNER_VLAN_INCL_VLTI;										// VLTI [20]: tag from TDES2.IVT; VLP [18] = 0: IVTIR of the context
	AX_BASE_WRITE_REG32(mac_base, R_MAC_INNER_VLAN_INCL, macivir);
	ok += vlan_case(mac_idx, "C", VT_CTX_QINQ, VT_TAG_S, VT_TAG_A, TDES3_CTXT_IVTIR_INSERT,  TDES2_VTIR_INSERT, vt_frame,  VT_FRAME_LEN,     0, VT_HDR_QINQ, 8, "MAC_Inner_VLAN_Incl VLP = 0: the context decides. IVTIR = insert, VTIR = insert -> two tags again, now ordered by the descriptor");
	ok += vlan_case(mac_idx, "D", VT_CTX_QINQ, VT_TAG_S, VT_TAG_A, TDES3_CTXT_IVTIR_REPLACE, TDES2_VTIR_NONE,   vt_frame2, VT_FRAME_LEN + 8, 8, VT_HDR_QINQ, 8, "frame sent WITH two tags (S 07D0, C E0C8), IVTIR = replace, VTIR = none. The MAC rewrites the inner tag to C 0064");
	ok += vlan_case(mac_idx, "E", VT_CTX_QINQ, VT_TAG_S, VT_TAG_A, TDES3_CTXT_IVTIR_REMOVE,  TDES2_VTIR_NONE,   vt_frame2, VT_FRAME_LEN + 8, 8, VT_HDR_S,    4, "same two-tag frame, IVTIR = remove, VTIR = none. The MAC cuts the inner tag out, 4 bytes less");

	eth_printf("\nResult: %lu/5 cases as expected.\n", (unsigned long)ok);
	eth_tx_vtir = TDES2_VTIR_NONE;
	eth_print_mac_counters(mac_idx);
	return (ok == 5) ? 1 : 0;
}
