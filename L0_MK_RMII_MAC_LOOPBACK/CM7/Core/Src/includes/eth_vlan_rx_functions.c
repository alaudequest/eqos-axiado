#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_vlan_rx_utils.h"
#include "eth_traffic_test.h"
#include "eth_printf.h"
#include "eth_flow_utils.h"

/*
 * VLAN tests of the MAC RECEIVER through the MAC loopback: what MAC_VLAN_Tag does with a tag that comes in.
 * Modes 4 and 5 look at the transmitter (it adds tags). Here the CPU writes the tag(s) into the frame itself,
 * sends it with TDES2.VTIR = none and no context, so the transmitter copies the bytes as they are and
 * everything that happens, happens on the RX side:
 *
 *     CPU frame with tag -> MAC TX (untouched) -> loopback -> MAC RX compares the tag with MAC_VLAN_Tag.VL
 *                                                      |-- match:    received, RDES2.VF = 1, (tag stripped if EVLS says so)
 *                                                      '-- no match: MAC_Packet_Filter.VTFE = 1 -> dropped, no RX descriptor
 *                                                                    MAC_Packet_Filter.VTFE = 0 -> received, RDES2 = 0, RS2V = 0
 *                                                      (board 2026-11-16: the MAC never writes VF = 0; a failed or not applied
 *                                                       filter leaves RDES2 at 0 with RDES3.RS2V = 0, the log prints VF as -)
 *
 * Test 1 (eth_run_test_ops = 8, eth_mac_vlan_filter_lb): the filter, VTFE = 1. VL = VID 100 unless said:
 *   A   no tag                         -> PASS   the VLAN filter only looks at tagged frames
 *   B   C-tag VID 100                  -> PASS   VF = 1, RDES0 = 0x0064
 *   C   C-tag VID 200                  -> DROP   no descriptor, RX timeout
 *   D   C-tag PCP 7 VID 100, ETV = 1   -> PASS   12-bit compare: PCP is not part of it
 *   E   same frame, ETV = 0            -> DROP   16-bit compare: 0xE064 != 0x0064
 *   F1  VTIM = 1, VID 100              -> DROP   inverse match
 *   F2  VTIM = 1, VID 200              -> PASS
 *   G   VL = 0, VID 200                -> PASS   VL = 0 switches the compare off (p.3124)
 *   H1  VTHM, hash table bit of VID 200, VL = 999 (matches nothing), VID 200 -> PASS   hash filter (MAC_VLAN_Hash_Table, p.3124)
 *   H2  same, VID 300                  -> DROP   (VID 100 would pass: it shares the hash index of 200)
 *       (VL = 0 must not be used here: G shows it makes every tag match, hash or not)
 *   I   S 2000 + C 100, VL 2000, ERSVLM = 1  -> PASS   match on the OUTER S-tag
 *   J   S 2000 + C 100, VL 100, ERIVLT = 1   -> PASS   match on the INNER C-tag (needs EDVLP)
 *   K1  S 2000 + C 100, VL 2000, ERSVLM = 0  -> PASS   outer tag is S type, the filter wants C type: the frame is
 *                                                      NOT filtered at all, RS2V = 0 (board 2026-11-12, 11-16; RM p.3123 only says "not matched")
 *   K2  same + DOVLTC = 1              -> PASS   type check off: the value 2000 alone decides
 * Board 2026-11-12: Rx_Unicast_Packets_Good counts every frame, dropped ones too (MMC sits before the filter).
 * Test 2 (eth_run_test_ops = 9, eth_mac_vlan_strip_lb): stripping, VTFE = 0 (nothing is dropped):
 *   A   EVLS = always,         VID 100  -> 60 bytes, tag gone, RDES0.OVT = 0x0064, VF = 1 (VL = 0: every tag matches)
 *   B   EVLS = if pass, VL 100, VID 100 -> 60 bytes, stripped, VF = 1
 *   C   EVLS = if pass, VL 100, VID 200 -> 64 bytes, tag KEPT, RS2V = 0 (VF -), still received (VTFE = 0)
 *   D1  EVLS = if fail, VL 100, VID 200 -> 60 bytes, stripped because it failed, RS2V = 0: the compare ran, VF is just not reported
 *   D2  EVLS = if fail, VL 100, VID 100 -> 64 bytes, kept because it passed
 *   E   EVLS + EIVLS = always, S 2000 + C 100 -> 60 bytes, both gone, RDES0 = IVT 0x0064 << 16 | OVT 0x07D0, RS2V = 0 (S-tag, ERSVLM = 0)
 *   F   EVLS = always, EIVLS = none, S 2000 + C 100 -> 64 bytes, inner C-tag now at byte 12, RDES0 still = 0x006407D0 (IVT reported
 *                                                      by EIVLRXS even without inner stripping), LT = 5, RS2V = 0
 * Board 2026-11-16: 5/7, C and D1 failed only on VF (expected 0, got RS2V = 0); 7/7 with VR_VF_NONE.
 *
 * RM0399: MAC_VLAN_Tag p.3122-3124; MAC_VLAN_Hash_Table p.3124; MAC_Packet_Filter.VTFE p.3118; RDES0 OVT/IVT p.3065;
 * RDES2.VF p.3068; RDES3.LT / RS0V / RS2V p.3069.
 */

#define VR_FRAME_LEN	60			// frame without tag; + 4 per tag written by the CPU
#define VR_TPID_C		0x8100		// C-VLAN type (802.1Q)
#define VR_TPID_S		0x88A8		// S-VLAN type (802.1ad)
#define VR_TAG_100		0x0064		// PCP 0, VID 100
#define VR_TAG_200		0x00C8		// PCP 0, VID 200
#define VR_TAG_300		0x012C		// PCP 0, VID 300: another hash index than VID 200 (H2); 100 and 200 share one
#define VR_TAG_100P7	0xE064		// PCP 7, VID 100: same VID as VR_TAG_100, different 16-bit value
#define VR_TAG_S		0x07D0		// PCP 0, VID 2000 (outer, operator)
#define VR_VL_NONE		0x03E7		// VID 999: in VL while the hash table is tested, so the direct compare never matches
#define VR_DROP_WAIT_US	5000		// a frame that must NOT come back: wait this long (a looped frame is back in ~200 us)
#define VR_PASS			1			// vr_case(): the frame must be received
#define VR_DROP			0			// vr_case(): the frame must be dropped
#define VR_VF_ANY		-2			// vr_case(): RDES2.VF not checked
#define VR_VF_NONE		-1			// vr_case(): RDES3.RS2V = 0 expected (RDES2 not written: filter failed or not applied), log shows -
#define VR_RDES0_OVT	0x0000FFFFUL	// vr_case(): compare RDES0 [15:0] only (outer tag)
#define VR_RDES0_BOTH	0xFFFFFFFFUL	// vr_case(): compare the whole RDES0 (inner [31:16] + outer [15:0])
#define VR_RDES0_NONE	0x00000000UL	// vr_case(): RDES0 not checked
#define VR_DUMP_PAYLOAD	16			// payload bytes shown by vr_frame_dump()
#define VR_HASH_BITS	12			// VLAN hash with ETV = 1: CRC over the 12 VID bits
#define VR_CRC32_POLY	0xEDB88320UL	// CRC-32 (IEEE 802.3), reflected, the one used by the MAC hash filters

static uint8_t vr_f0[MAC_100M_BUF_SIZE];		// 60: DA SA 88B5 payload
static uint8_t vr_f100[MAC_100M_BUF_SIZE];		// 64: DA SA 81 00 00 64 88B5 payload
static uint8_t vr_f200[MAC_100M_BUF_SIZE];		// 64: DA SA 81 00 00 C8 88B5 payload
static uint8_t vr_f300[MAC_100M_BUF_SIZE];		// 64: DA SA 81 00 01 2C 88B5 payload
static uint8_t vr_f100p7[MAC_100M_BUF_SIZE];	// 64: DA SA 81 00 E0 64 88B5 payload
static uint8_t vr_fq[MAC_100M_BUF_SIZE];		// 68: DA SA 88 A8 07 D0 81 00 00 64 88B5 payload
static uint8_t vr_rxbuf[MAC_100M_BUF_SIZE];		// frame received
#define VR_SUMMARY_LEN	160							// "A:OK B:BAD ..." of one test, printed before the Result line
static char vr_summary[VR_SUMMARY_LEN];
static int vr_summary_pos;

/* Append "id:OK " or "id:BAD " to the summary of the running test */
static void vr_summary_add(const char *id, int ok)
{
	const char *r = ok ? ":OK " : ":BAD ";
	int n = (int)strlen(id) + (int)strlen(r);

	if (vr_summary_pos + n < VR_SUMMARY_LEN) {
		memcpy(vr_summary + vr_summary_pos, id, strlen(id));
		memcpy(vr_summary + vr_summary_pos + strlen(id), r, strlen(r));
		vr_summary_pos += n;
		vr_summary[vr_summary_pos] = 0;
	}
}

/* Column header of the result table; vr_case() prints its line with the same widths */
#define VR_TABLE_HDR	"  id  expect actual  len  bytes 12..21                     LT  VF  RDES0       MMC rx | result"

/* Write one 4-byte tag at f[at..at+3]: type (TPID) then value (TCI), both big-endian */
static void vr_put_tag(uint8_t *f, int at, uint16_t tpid, uint16_t tci)
{
	f[at]     = (uint8_t)(tpid >> 8);
	f[at + 1] = (uint8_t)(tpid & 0xFF);
	f[at + 2] = (uint8_t)(tci >> 8);
	f[at + 3] = (uint8_t)(tci & 0xFF);
}
/* Build one frame: DA | SA | ntag tags (outer first) | EtherType 0x88B5 | payload 0x10, 0x11, ... Return its length */
static int vr_build(uint8_t *f, int ntag, uint16_t tpid1, uint16_t tci1, uint16_t tpid2, uint16_t tci2)
{
	unsigned char da[ETH_TEST_MAC_SIZE] = ETH_TEST_DEST_MAC;
	unsigned char sa[ETH_TEST_MAC_SIZE] = ETH_TEST_SRC_MAC;
	int p = 12;
	int len = VR_FRAME_LEN + 4 * ntag;
	int i;

	memcpy(f, da, ETH_TEST_MAC_SIZE);
	memcpy(f + ETH_TEST_MAC_SIZE, sa, ETH_TEST_MAC_SIZE);
	if (ntag >= 1) {
		vr_put_tag(f, p, tpid1, tci1);
		p += 4;
	}
	if (ntag >= 2) {
		vr_put_tag(f, p, tpid2, tci2);
		p += 4;
	}
	f[p]     = (uint8_t)(ETH_TEST_ETHER_TYPE >> 8);
	f[p + 1] = (uint8_t)(ETH_TEST_ETHER_TYPE & 0xFF);
	for (i = p + 2; i < len; i++) {
		f[i] = (uint8_t)(0x10 + (i - p - 2));
	}
	return len;
}
/* The five frames, built once */
static void vr_build_frames(void)
{
	vr_build(vr_f0,     0, 0, 0, 0, 0);
	vr_build(vr_f100,   1, VR_TPID_C, VR_TAG_100,   0, 0);
	vr_build(vr_f200,   1, VR_TPID_C, VR_TAG_200,   0, 0);
	vr_build(vr_f300,   1, VR_TPID_C, VR_TAG_300,   0, 0);
	vr_build(vr_f100p7, 1, VR_TPID_C, VR_TAG_100P7, 0, 0);
	vr_build(vr_fq,     2, VR_TPID_S, VR_TAG_S, VR_TPID_C, VR_TAG_100);
}
/* Dump of one frame split at the VLAN tags: DA | SA | tag(s) | EtherType | payload (same layout as mode 4/5) */
static void vr_frame_dump(const char *title, const uint8_t *f, int len)
{
	int i, p = 12;	// first byte after DA + SA
	uint16_t tpid;

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
	while (p + 4 <= len) {
		tpid = (uint16_t)(((uint16_t)f[p] << 8) | f[p + 1]);
		if (tpid != VR_TPID_S && tpid != VR_TPID_C) {
			break;
		}
		eth_printf("        [%2d..%2d] %s  : %02X %02X %02X %02X   (PCP %d, VID %d)\n", p, p + 3,
				   (tpid == VR_TPID_S) ? "S-tag" : "C-tag", f[p], f[p + 1], f[p + 2], f[p + 3],
				   f[p + 2] >> 5, ((f[p + 2] & 0x0F) << 8) | f[p + 3]);
		p += 4;
	}
	if (p + 2 <= len) {
		eth_printf("        [%2d..%2d] type   : %02X %02X\n", p, p + 1, f[p], f[p + 1]);
		p += 2;
	}
	eth_printf("        [%2d..%2d] payload:", p, len - 1);
	for (i = p; i < p + VR_DUMP_PAYLOAD && i < len; i++) {
		eth_printf(" %02X", f[i]);
	}
	eth_printf("%s (%d bytes)\n", (len - p > VR_DUMP_PAYLOAD) ? " ..." : "", len - p);
}
/* Bit reversal of a 32-bit word (bit 0 <-> bit 31) */
static uint32_t vr_bitrev32(uint32_t x)
{
	uint32_t r = 0;
	int i;

	for (i = 0; i < 32; i++) {
		r = (r << 1) | ((x >> i) & 1U);
	}
	return r;
}
/**
 * Index (0..15) of a VID in the 16-bit VLAN hash table MAC_VLAN_Hash_Table (p.3124): "the most significant four
 * bits of the CRC of the VLAN tag"; with ETV = 1 the CRC runs over the 12-bit VID. Same CRC-32 as the
 * MAC address hash (mode 1): reflected, init all 1s, bit by bit from VID bit 0, then the MAC takes the
 * complement and reads the top 4 bits in reverse order (the way the Linux stmmac driver computes it).
 * Unverified on this silicon: case H prints the index so the log can confirm or correct it.
 */
static int vr_vid_hash_index(uint16_t vid)
{
	uint32_t crc = 0xFFFFFFFFUL;
	uint32_t bit;
	int i;

	for (i = 0; i < VR_HASH_BITS; i++) {
		bit = ((uint32_t)vid >> i) & 1U;
		if (((crc & 1U) ^ bit) != 0U) {
			crc = (crc >> 1) ^ VR_CRC32_POLY;
		} else {
			crc >>= 1;
		}
	}
	return (int)(vr_bitrev32(~crc) >> 28);
}
/* Name of an EVLS / EIVLS value (p.3123): 00 no strip, 01 strip if the filter passes, 10 if it fails, 11 always */
static const char *vr_strip_name(uint32_t v)
{
	switch (v) {
	case MAC_VLAN_TAG_EVLS_PASS:   return "if pass";
	case MAC_VLAN_TAG_EVLS_FAIL:   return "if fail";
	case MAC_VLAN_TAG_EVLS_ALWAYS: return "always";
	default:                 return "none";
	}
}
/* The setup line: every MAC_VLAN_Tag bit that matters here, and MAC_Packet_Filter.VTFE */
static void vr_print_setup(uint32_t macvtr, int vtfe)
{
	eth_printf("      setup : MAC_Packet_Filter.VTFE = %d (%s); MAC_VLAN_Tag: VL = 0x%04lX (VID %lu), ETV = %d (%s), VTIM = %d, VTHM = %d,\n",
			   vtfe, vtfe ? "drop if no match" : "never drop, only report VF",
			   (unsigned long)(macvtr & MAC_VLAN_TAG_VL_MASK), (unsigned long)(macvtr & 0x0FFF),
			   (macvtr & MAC_VLAN_TAG_ETV) != 0, (macvtr & MAC_VLAN_TAG_ETV) ? "12-bit VID" : "16-bit tag",
			   (macvtr & MAC_VLAN_TAG_VTIM) != 0, (macvtr & MAC_VLAN_TAG_VTHM) != 0);
	eth_printf("              ERSVLM = %d (match %s-tag), ERIVLT = %d (%s tag), DOVLTC = %d, EDVLP = %d, EVLS = %s, EIVLS = %s\n",
			   (macvtr & MAC_VLAN_TAG_ERSVLM) != 0, (macvtr & MAC_VLAN_TAG_ERSVLM) ? "S" : "C",
			   (macvtr & MAC_VLAN_TAG_ERIVLT) != 0, (macvtr & MAC_VLAN_TAG_ERIVLT) ? "inner" : "outer",
			   (macvtr & MAC_VLAN_TAG_DOVLTC) != 0, (macvtr & MAC_VLAN_TAG_EDVLP) != 0,
			   vr_strip_name((macvtr >> MAC_VLAN_TAG_EVLS_bit) & 3U), vr_strip_name((macvtr >> MAC_VLAN_TAG_EIVLS_bit) & 3U));
}
/**
 * One case: program the RX VLAN registers -> send one frame (tags written by the CPU) -> receive or time out
 * -> compare with what the receiver should have done.
 *
 * The frame received (when expect_rx = VR_PASS) must be: frame[0..11] | frame[12 + skip .. len-1]
 *   skip = 0: nothing stripped;  4: one tag stripped;  8: both tags stripped
 *
 * @param macvtr      value written to MAC_VLAN_Tag (VL, ETV, VTIM, VTHM, ERSVLM, ERIVLT, DOVLTC, EVLS, EIVLS...)
 * @param vtfe        MAC_Packet_Filter.VTFE: 1 = drop a tagged frame that does not match
 * @param expect_rx   VR_PASS or VR_DROP
 * @param exp_rdes0   RDES0 write-back expected, compared under rdes0_mask (VR_RDES0_OVT / BOTH / NONE)
 * @param exp_vf      1 (RS2V = 1, VF = 1), VR_VF_NONE (RS2V = 0, no VF) or VR_VF_ANY. 0 is never reported by this MAC (board 2026-11-16)
 * Return 1: as expected, 0: else
 */
static int vr_case(uint8_t mac_idx, const char *id, uint32_t macvtr, int vtfe, const uint8_t *frame, int len,
				   int expect_rx, int skip, uint32_t exp_rdes0, uint32_t rdes0_mask, int exp_vf, const char *what)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t macpfr;
	uint32_t rdes0 = 0, rdes2 = 0, rdes3 = 0;
	uint32_t rx_good0, rx_good1;
	uint32_t wait, k;
	int exp_len = len - skip;
	int tx_len, rx_len;
	int got, vf, lt;
	int len_ok, addr_ok, rest_ok, rdes0_ok, vf_ok, ok;

	/* 0. description, setup and expectation before anything is sent */
	eth_printf("\n  --- case %s: %s\n", id, what);
	vr_print_setup(macvtr, vtfe);
	if (expect_rx == VR_PASS) {
		eth_printf("      Expect: PASS, %d bytes%s, bytes 12..13 = %02X %02X", exp_len,
				   skip ? " (tag bytes stripped)" : "", frame[12 + skip], frame[13 + skip]);
		if (rdes0_mask != VR_RDES0_NONE) {
			eth_printf(", RDES0 %s= 0x%08lX", (rdes0_mask == VR_RDES0_OVT) ? "[15:0] " : "", (unsigned long)exp_rdes0);
		}
		if (exp_vf == VR_VF_NONE) {
			eth_printf(", VF = - (RS2V = 0)");
		} else if (exp_vf != VR_VF_ANY) {
			eth_printf(", VF = %d", exp_vf);
		}
		eth_printf("\n");
	} else {
		eth_printf("      expect: DROP, no RX descriptor within %d us\n", VR_DROP_WAIT_US);
	}
	/* 1. the receiver's rules: VTFE in MAC_Packet_Filter (keep the other filter bits), the rest in MAC_VLAN_Tag */
	macpfr = AX_BASE_READ_REG32(mac_base, R_MAC_PACKET_FILTER) & ~(uint32_t)MAC_PACKET_FILTER_VTFE;
	if (vtfe) {
		macpfr |= MAC_PACKET_FILTER_VTFE;										// VTFE [16]: drop tagged frames that fail
	}
	AX_BASE_WRITE_REG32(mac_base, R_MAC_PACKET_FILTER, macpfr);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_VLAN_TAG, macvtr);
	flow_regw("MAC_Packet_Filter (VTFE)", mac_base, R_MAC_PACKET_FILTER, macpfr, FF_MAC_PACKET_FILTER);
	flow_regw("MAC_VLAN_Tag", mac_base, R_MAC_VLAN_TAG, macvtr, FF_MAC_VLAN_TAG);
	rx_good0 = AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_UNICAST_GOOD);
	/* 2. the frame, as it is: no context, VTIR = none, the transmitter copies the tag bytes */
	eth_tx_vtir = TDES2_VTIR_NONE;
	tx_len = egress_packet((unsigned char *)frame, len);
	if (tx_len != len) {
		eth_printf("  %s  TX error %d\n", id, tx_len);
		return 0;
	}
	/* 3. RDES0 is only in the write-back: poll OWN, read it, then let ingress_packet_status() take the frame.
	 *    A dropped frame never gets a descriptor: the poll times out and the ingress call returns -1 */
	wait = (expect_rx == VR_PASS) ? ETH_TEST_RX_TIMEOUT_US : VR_DROP_WAIT_US;
	while ((rx_desc[eth_rx_head].des3 & RDES3_OWN) && wait > 0) {
		udelay(1);
		wait--;
	}
	rdes0 = rx_desc[eth_rx_head].des0;
	rx_len = ingress_packet_status(mac_idx, vr_rxbuf, sizeof(vr_rxbuf), (wait > 0) ? ETH_TEST_RX_TIMEOUT_US : 1,
								   &rdes2, &rdes3);
	got = (rx_len >= 0);
	rx_good1 = AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_UNICAST_GOOD);
	vf = (rdes3 & RDES3_RS2V) ? ((rdes2 & RDES2_VF) != 0) : -1;	// VF only valid with RS2V
	lt = (int)((rdes3 >> RDES3_LT_bit) & RDES3_LT_MASK);
	/* 4. check */
	if (expect_rx == VR_PASS) {
		len_ok   = got && (rx_len == exp_len);
		addr_ok  = got && (memcmp(vr_rxbuf, frame, 12) == 0);
		rest_ok  = got && (memcmp(vr_rxbuf + 12, frame + 12 + skip, (size_t)(len - 12 - skip)) == 0);
		rdes0_ok = (rdes0_mask == VR_RDES0_NONE) || ((rdes0 & rdes0_mask) == (exp_rdes0 & rdes0_mask));
		vf_ok    = (exp_vf == VR_VF_ANY) || (vf == exp_vf);		// vf = -1 when RS2V = 0, which is VR_VF_NONE
		ok = len_ok && addr_ok && rest_ok && rdes0_ok && vf_ok;
	} else {
		len_ok = addr_ok = rest_ok = rdes0_ok = vf_ok = 1;
		ok = !got;
	}
	/* 5. column header + one line */
	eth_printf(VR_TABLE_HDR "\n");
	eth_printf("  %-3s %-6s %-6s ", id, (expect_rx == VR_PASS) ? "PASS" : "DROP", got ? "PASS" : "DROP");
	if (got) {
		eth_printf("%3d  ", rx_len);
		for (k = 12; k < 22; k++) {
			if ((int)k < rx_len) {
				eth_printf("%02X ", vr_rxbuf[k]);
			} else {
				eth_printf("   ");
			}
		}
		eth_printf(" %d   ", lt);
		if (vf >= 0) {
			eth_printf("%d   ", vf);
		} else {
			eth_printf("-   ");
		}
		eth_printf("0x%08lX  ", (unsigned long)rdes0);
	} else {
		eth_printf("  -  (no frame)                       -   -   -           ");
	}
	eth_printf("+%-5lu | %s\n", (unsigned long)(rx_good1 - rx_good0), ok ? "OK " : "BAD");
	if (!ok) {
		if (expect_rx == VR_PASS) {
			eth_printf("      failed check(s):%s%s%s%s%s\n",
					   len_ok ? "" : " length", addr_ok ? "" : " DA/SA", rest_ok ? "" : " bytes-after-SA",
					   rdes0_ok ? "" : " RDES0", vf_ok ? "" : " VF");
		} else {
			eth_printf("      failed: the frame came back although the filter should have dropped it\n");
		}
	}
	/* 6. both frames, split at the tags */
	vr_frame_dump("frame sent", frame, len);
	if (got) {
		vr_frame_dump("frame received", vr_rxbuf, rx_len);
	}
	vr_summary_add(id, ok);
	return ok;
}
/**
 * Common start of both tests: reset, MAC 100M init, loopback, DMA + MAC start, frames built. No TX insertion:
 * MAC_VLAN_Incl = MAC_Inner_VLAN_Incl = 0. MAC_VLAN_Tag is written per case (vr_case). Return 1: ready, 0: reset failed
 */
static int vr_init(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	eth_printf("Start Init: Port[%d]\n", mac_idx);

	if (eth_reset_mac(mac_idx) == 0) {
		return 0;
	}
	mac_init_100m(mac_idx);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_VLAN_INCL, 0);					// TX adds nothing: the tags are in the frames
	AX_BASE_WRITE_REG32(mac_base, R_MAC_INNER_VLAN_INCL, 0);
	init_mac_internal_lb(mac_idx);
	mac_start(mac_idx);
	vr_build_frames();
	vr_summary_pos = 0;
	vr_summary[0] = 0;
	return 1;
}
/* Title + how to read the result lines (same columns for both tests) */
static void vr_print_header(const char *title)
{
	eth_printf("\n================ %s ================\n", title);
	eth_printf("  The CPU writes the tag(s) into the frame (VTIR = none, no context): the transmitter copies them, the receiver\n");
	eth_printf("  compares them with MAC_VLAN_Tag. expect/actual: PASS = a frame came back, DROP = none within %d us.\n", VR_DROP_WAIT_US);
	eth_printf("  LT (RDES3): %lu = type, %lu = one tag, %lu = two tags. VF (RDES2, p.3068): 1 = passed the VLAN filter, - = RS2V = 0 (failed or not applied).\n",
			   (unsigned long)RDES3_LT_TYPE, (unsigned long)RDES3_LT_VLAN, (unsigned long)RDES3_LT_DVLAN);
	eth_printf("  RDES0: outer tag [15:0], inner tag [31:16] (only with stripping). MMC rx = Rx_Unicast_Packets_Good\n");
	eth_printf("  increment: +1 even for a dropped frame, the MMC counts before the VLAN filter (board 2026-11-12).\n");
	eth_printf("  Each case: description, setup, expectation -> [FLOW lines] -> result line -> frame sent / received.\n");
}
/**
 * Test 1: VLAN filter through the MAC loopback (eth_run_test_ops = 8), cases A-K2.
 * Same bits as eth_mac_internal_lb() (eth_functions.c) plus, per case (see vr_case):
 *
 *   Register               Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   MAC_Packet_Filter p.3118      VTFE [16]            1       drop tagged frames that fail the VLAN filter
 *   MAC_VLAN_Tag p.3122      VL [15:0]            tag     the tag (or VID) to match
 *                          ETV [16]             1/0     compare 12-bit VID / 16-bit tag
 *                          VTIM [17]            0/1     inverse match (F)
 *                          ESVL [18]            1       0x88A8 frames are VLAN frames
 *                          ERSVLM [19]          0/1     the filter looks at C-tags / S-tags (I, K)
 *                          DOVLTC [20]          0/1     type check off (K2)
 *                          EVLRXS [24]          1       outer tag -> RDES0
 *                          VTHM [25]            0/1     hash table instead of VL (H)
 *                          EDVLP [26]           1       two tags processed
 *                          ERIVLT [27]          0/1     the filter looks at the outer / inner tag (J)
 *                          EIVLRXS [31]         1       inner tag -> RDES0 [31:16] when possible
 *   MAC_VLAN_Hash_Table p.3124     VLHT [15:0]          1 << i  hash table, bit i = hash index of the VID (H)
 * Return 1: every case as expected, 0: else
 */
uint16_t eth_mac_vlan_filter_lb(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t base = MAC_VLAN_TAG_ESVL										// ESVL [18]: 0x88A8 counts as VLAN
				  | MAC_VLAN_TAG_EVLRXS									// EVLRXS [24]: outer tag -> RDES0
				  | MAC_VLAN_TAG_EIVLRXS									// EIVLRXS [31]: inner tag -> RDES0 [31:16]
				  | MAC_VLAN_TAG_EDVLP;									// EDVLP [26]: two tags processed
	uint32_t vl100 = base | MAC_VLAN_TAG_ETV | VR_TAG_100;				// ETV [16] + VL = VID 100: the usual rule
	uint32_t ok = 0;
	int h200 = vr_vid_hash_index(VR_TAG_200);
	int h300 = vr_vid_hash_index(VR_TAG_300);

	if (vr_init(mac_idx) == 0) {
		return 0;
	}
	vr_print_header("VLAN FILTER TEST (MAC_VLAN_Tag VL / ETV / VTIM / VTHM / ERSVLM / ERIVLT / DOVLTC, MAC_Packet_Filter.VTFE)");
	eth_printf("  case B prints the [10]/[11] FLOW lines. VLAN hash (case H): software index of VID 200 = %d, VID 300 = %d\n", h200, h300);

	/* A-C: the basic rule, VID 100 with ETV */
	eth_printf("=======================================================================================\n");
	eth_flow_trace = 1;
	ok += vr_case(mac_idx, "A",  vl100, 1, vr_f0,     VR_FRAME_LEN,     VR_PASS, 0, 0, VR_RDES0_NONE, VR_VF_ANY,
				  "no tag: the VLAN filter ignores untagged frames, DA filter only");
	eth_printf("=======================================================================================\n");
	ok += vr_case(mac_idx, "B",  vl100, 1, vr_f100,   VR_FRAME_LEN + 4, VR_PASS, 0, VR_TAG_100, VR_RDES0_OVT, 1,
				  "C-tag VID 100 = VL: passes, VF = 1, RDES0 reports the tag");
	eth_printf("=======================================================================================\n");
	//eth_flow_trace = 0;
	ok += vr_case(mac_idx, "C",  vl100, 1, vr_f200,   VR_FRAME_LEN + 4, VR_DROP, 0, 0, VR_RDES0_NONE, VR_VF_ANY,
				  "C-tag VID 200 != VL: dropped by the receiver, no descriptor (if it comes back: try MAC_Packet_Filter.PR = 0)");
	eth_printf("=======================================================================================\n");
	/* D, E: 12-bit against 16-bit compare, same frame PCP 7 VID 100 */
	ok += vr_case(mac_idx, "D",  vl100, 1, vr_f100p7, VR_FRAME_LEN + 4, VR_PASS, 0, VR_TAG_100P7, VR_RDES0_OVT, 1,
				  "PCP 7 VID 100, ETV = 1: only the 12 VID bits are compared, PCP does not matter");
	eth_printf("=======================================================================================\n");
	ok += vr_case(mac_idx, "E",  base | VR_TAG_100, 1, vr_f100p7, VR_FRAME_LEN + 4, VR_DROP, 0, 0, VR_RDES0_NONE, VR_VF_ANY,
				  "same frame, ETV = 0: all 16 bits compared, 0xE064 != 0x0064, dropped");
	eth_printf("=======================================================================================\n");
	/* F: inverse match */
	ok += vr_case(mac_idx, "F1", vl100 | MAC_VLAN_TAG_VTIM, 1, vr_f100, VR_FRAME_LEN + 4, VR_DROP, 0, 0, VR_RDES0_NONE, VR_VF_ANY,
				  "VTIM = 1: the matching VID 100 is now the one dropped");
	eth_printf("=======================================================================================\n");
	ok += vr_case(mac_idx, "F2", vl100 | MAC_VLAN_TAG_VTIM, 1, vr_f200, VR_FRAME_LEN + 4, VR_PASS, 0, VR_TAG_200, VR_RDES0_OVT, 1,
				  "VTIM = 1: VID 200 does not match VL, so it passes");
	eth_printf("=======================================================================================\n");
	/* G: VL = 0 */
	ok += vr_case(mac_idx, "G",  base | MAC_VLAN_TAG_ETV, 1, vr_f200, VR_FRAME_LEN + 4, VR_PASS, 0, VR_TAG_200, VR_RDES0_OVT, 1,
				  "VL = 0: no compare at all, every tagged frame is a VLAN match (p.3124)");
	eth_printf("=======================================================================================\n");
	
	eth_printf("h200= %x\n", h200);
	eth_printf("h300= %x\n", h300);
	/* H: hash table with one bit, the one of VID 200 */
	AX_BASE_WRITE_REG32(mac_base, R_MAC_VLAN_HASH_TABLE, (1UL << h200) & 0xFFFFUL);
	flow_regw("MAC_VLAN_Hash_Table", mac_base, R_MAC_VLAN_HASH_TABLE, (1UL << h200) & 0xFFFFUL, FF_NONE);
	ok += vr_case(mac_idx, "H1", base | MAC_VLAN_TAG_ETV | MAC_VLAN_TAG_VTHM | VR_VL_NONE, 1, vr_f200, VR_FRAME_LEN + 4, VR_PASS, 0, VR_TAG_200, VR_RDES0_OVT, 1,
				  "VTHM = 1, VL = 999 (no direct match), MAC_VLAN_Hash_Table has the bit of VID 200: hash match (if DROP, the software hash index is wrong)");
	eth_printf("=======================================================================================\n");
	ok += vr_case(mac_idx, "H2", base | MAC_VLAN_TAG_ETV | MAC_VLAN_TAG_VTHM | VR_VL_NONE, 1, vr_f300, VR_FRAME_LEN + 4,
				  (h300 == h200) ? VR_PASS : VR_DROP, 0, 0, VR_RDES0_NONE, VR_VF_ANY,
				  (h300 == h200) ? "VID 300 shares the hash index with 200: passes too (hash is not exact)"
								 : "VID 300 has another hash index: dropped (VID 100 would pass: same index as 200)");
	eth_printf("=======================================================================================\n");
	AX_BASE_WRITE_REG32(mac_base, R_MAC_VLAN_HASH_TABLE, 0);
	/* I-K: the two-tag frame S 2000 + C 100, which tag and which type the filter looks at */
	ok += vr_case(mac_idx, "I",  base | MAC_VLAN_TAG_ETV | MAC_VLAN_TAG_ERSVLM | VR_TAG_S, 1, vr_fq, VR_FRAME_LEN + 8, VR_PASS, 0, VR_TAG_S, VR_RDES0_OVT, 1,
				  "VL 2000, ERSVLM = 1 (S-tags): the OUTER S-tag 2000 matches");
	eth_printf("=======================================================================================\n");
	ok += vr_case(mac_idx, "J",  vl100 | MAC_VLAN_TAG_ERIVLT, 1, vr_fq, VR_FRAME_LEN + 8, VR_PASS, 0, VR_TAG_S, VR_RDES0_OVT, 1,
				  "VL 100, ERIVLT = 1: the filter looks at the INNER C-tag 100, matches (RDES0 still reports the outer tag)");
	eth_printf("=======================================================================================\n");
	ok += vr_case(mac_idx, "K1", base | MAC_VLAN_TAG_ETV | VR_TAG_S, 1, vr_fq, VR_FRAME_LEN + 8, VR_PASS, 0, VR_TAG_S, VR_RDES0_OVT, VR_VF_NONE,
				  "VL 2000, ERSVLM = 0 (C-tags): the outer tag has the S type, so the filter does not apply, frame passes, RS2V = 0");
	eth_printf("=======================================================================================\n");
	ok += vr_case(mac_idx, "K2", base | MAC_VLAN_TAG_ETV | MAC_VLAN_TAG_DOVLTC | VR_TAG_S, 1, vr_fq, VR_FRAME_LEN + 8, VR_PASS, 0, VR_TAG_S, VR_RDES0_OVT, 1,
				  "same + DOVLTC = 1: type check off, the value 2000 alone decides");

	eth_printf("\nCases: %s\n", vr_summary);
	eth_printf("Result: %lu/14 cases as expected.\n", (unsigned long)ok);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_PACKET_FILTER, AX_BASE_READ_REG32(mac_base, R_MAC_PACKET_FILTER) & ~(uint32_t)MAC_PACKET_FILTER_VTFE);
	eth_print_mac_counters(mac_idx);
	return (ok == 14) ? 1 : 0;
}
/**
 * Test 2: VLAN tag stripping through the MAC loopback (eth_run_test_ops = 9), cases A-F. VTFE = 0 everywhere:
 * the filter result only steers the stripping (EVLS = if pass / if fail) and RDES2.VF.
 *
 *   Register               Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   MAC_Packet_Filter p.3118      VTFE [16]            0       nothing dropped
 *   MAC_VLAN_Tag p.3123      EVLS [22:21]         0..3    outer tag: no strip / if pass / if fail / always
 *                          EIVLS [29:28]        0..3    inner tag, same coding
 *                          VL, ETV              100     the rule EVLS "if pass / if fail" refers to
 *                          EVLRXS, EIVLRXS, EDVLP, ESVL 1  tags reported in RDES0, two tags processed
 * Return 1: every case as expected, 0: else
 */
uint16_t eth_mac_vlan_strip_lb(uint8_t mac_idx)
{
	uint32_t base = MAC_VLAN_TAG_ESVL | MAC_VLAN_TAG_EVLRXS | MAC_VLAN_TAG_EIVLRXS | MAC_VLAN_TAG_EDVLP;
	uint32_t vl100 = base | MAC_VLAN_TAG_ETV | VR_TAG_100;
	uint32_t ok = 0;

	if (vr_init(mac_idx) == 0) {
		return 0;
	}
	vr_print_header("VLAN STRIP TEST (MAC_VLAN_Tag EVLS / EIVLS, RDES0 OVT / IVT, RDES2.VF; VTFE = 0)");
	eth_printf("  case A prints the [10]/[11] FLOW lines.\n");

	eth_flow_trace = 1;
	ok += vr_case(mac_idx, "A",  base | (MAC_VLAN_TAG_EVLS_ALWAYS << MAC_VLAN_TAG_EVLS_bit), 0, vr_f100, VR_FRAME_LEN + 4, VR_PASS, 4, VR_TAG_100, VR_RDES0_OVT, 1,
				  "EVLS = always: the tag is removed, the frame is 4 bytes shorter, RDES0 keeps the tag value; VL = 0 matches every tag, VF = 1");
	eth_flow_trace = 1;
	ok += vr_case(mac_idx, "B",  vl100 | (MAC_VLAN_TAG_EVLS_PASS << MAC_VLAN_TAG_EVLS_bit), 0, vr_f100, VR_FRAME_LEN + 4, VR_PASS, 4, VR_TAG_100, VR_RDES0_OVT, 1,
				  "EVLS = if pass, VL 100, VID 100: filter passes -> stripped, VF = 1");
	ok += vr_case(mac_idx, "C",  vl100 | (MAC_VLAN_TAG_EVLS_PASS << MAC_VLAN_TAG_EVLS_bit), 0, vr_f200, VR_FRAME_LEN + 4, VR_PASS, 0, VR_TAG_200, VR_RDES0_OVT, VR_VF_NONE,
				  "EVLS = if pass, VID 200: filter fails -> tag kept, RS2V = 0 (no VF), still received because VTFE = 0");
	ok += vr_case(mac_idx, "D1", vl100 | (MAC_VLAN_TAG_EVLS_FAIL << MAC_VLAN_TAG_EVLS_bit), 0, vr_f200, VR_FRAME_LEN + 4, VR_PASS, 4, VR_TAG_200, VR_RDES0_OVT, VR_VF_NONE,
				  "EVLS = if fail, VID 200: filter fails -> stripped, RS2V = 0 (the strip proves the compare ran)");
	ok += vr_case(mac_idx, "D2", vl100 | (MAC_VLAN_TAG_EVLS_FAIL << MAC_VLAN_TAG_EVLS_bit), 0, vr_f100, VR_FRAME_LEN + 4, VR_PASS, 0, VR_TAG_100, VR_RDES0_OVT, 1,
				  "EVLS = if fail, VID 100: filter passes -> tag kept");
	ok += vr_case(mac_idx, "E",  base | (MAC_VLAN_TAG_EVLS_ALWAYS << MAC_VLAN_TAG_EVLS_bit) | (MAC_VLAN_TAG_EVLS_ALWAYS << MAC_VLAN_TAG_EIVLS_bit), 0,
				  vr_fq, VR_FRAME_LEN + 8, VR_PASS, 8, ((uint32_t)VR_TAG_100 << RDES0_IVT_bit) | VR_TAG_S, VR_RDES0_BOTH, VR_VF_NONE,
				  "EVLS + EIVLS = always, S 2000 + C 100: both tags removed, RDES0 = inner [31:16] | outer [15:0], RS2V = 0 (S-tag, ERSVLM = 0)");
	ok += vr_case(mac_idx, "F",  base | (MAC_VLAN_TAG_EVLS_ALWAYS << MAC_VLAN_TAG_EVLS_bit), 0, vr_fq, VR_FRAME_LEN + 8, VR_PASS, 4, VR_TAG_S, VR_RDES0_OVT, VR_VF_NONE,
				  "EVLS = always, EIVLS = none, S 2000 + C 100: only the outer tag removed, C-tag now at byte 12, RS2V = 0");

	eth_printf("\nCases: %s\n", vr_summary);
	eth_printf("Result: %lu/7 cases as expected.\n", (unsigned long)ok);
	eth_print_mac_counters(mac_idx);
	return (ok == 7) ? 1 : 0;
}
