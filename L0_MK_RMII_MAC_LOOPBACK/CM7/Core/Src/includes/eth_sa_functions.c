#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_sa_utils.h"
#include "eth_traffic_test.h"
#include "eth_printf.h"
#include "eth_flow_utils.h"

/*
 * Frame surgery by the MAC TRANSMITTER, through the MAC loopback (eth_run_test_ops = 10): two things the
 * transmitter can do to a frame on its own, both asked for in TDES3 of the first descriptor (p.3051-3052):
 *
 *   SA insertion / replacement (RM0399 p.2952): TDES3.SAIC [25:23] per frame, or MAC_Configuration.SARC [30:28] for every
 *   frame. "Insert": the CPU gives a frame WITHOUT the 6 SA bytes (DA | type | payload), the MAC puts the content of
 *   MAC_Address0 (or MAC_Address1) after the DA, the frame grows by 6. "Replace": the CPU gives a normal frame, the MAC overwrites
 *   bytes 6..11 with MAC_Address0 (or MAC_Address1). Bit 25 (or bit 30 of SARC) picks MAC_Address1. RM p.2953 says that MAC_Address0 is used
 *   instead when MAC_Address1 is not enabled (MAC_Address1_High.AE = 0); the board does NOT do that (2026-11-18, case E): it takes
 *   the content of MAC_Address1_High/LR as it is, AE or not, so the reset value FF:FF:FF:FF:FF:FF went out as the SA.
 *   AE only matters for the RX address filter. Note the two encodings differ: SAIC [24:23] 01 insert / 10 replace,
 *   SARC [29:28] 10 insert / 11 replace (p.3113).
 *
 *   CRC and pad control: TDES3.CPC [27:26]. 00 the MAC pads a frame shorter than 60 bytes and appends the FCS
 *   (what every other test relies on); 01 FCS but no pad (a short frame goes out short: a runt); 10 neither
 *   (the CPU must supply pad and FCS itself: here it supplies garbage, so the receiver sees a CRC error);
 *   11 the MAC recomputes the FCS over the last 4 bytes of the buffer (the garbage is repaired).
 *   What the RECEIVER does with the damaged frames is decided by MTL_RxQ0_Operation_Mode (p.3106): FUP [3] forwards
 *   undersized good frames (< 64 bytes, "including pad-bytes and CRC"), FEP [4] forwards frames with an error
 *   (RDES3.ES + CE for a CRC error, p.3069); both 0 by default: the frame is dropped in the MTL RX queue.
 *
 *     CPU frame -> MAC TX: SA inserted/replaced, pad + FCS as CPC says -> loopback -> MAC RX: FCS checked,
 *     runt / CRC error -> MTL RX queue: FUP / FEP decide drop or forward -> RX descriptor + buffer
 *
 * Frames (DA 6C:1F:F7:CA:2B:FD, EtherType 0x88B5, payload 0x10, 0x11, ...):
 *   f_board 60: SA = 00:80:E1:00:00:00 (MAC_Address0)       f_other 60: SA = 02:00:00:00:00:99      f_a1 60: SA = 02:11:22:33:44:55
 *   f_nosa  54: no SA at all (DA | type | payload)   f_runt  22: 8 payload bytes               f_crc 64: f_board + DE AD BE EF
 *   f_a1reset 60: SA = FF:FF:FF:FF:FF:FF (MAC_Address1_High/LR reset value, what case E receives)
 *
 * Cases A-G: source address (CPC = 00, everything received). Expected SA in the frame received:
 *   A   SAIC = none,              f_other -> SA 02:00:..:99 kept: the MAC copies the frame as it is
 *   B   SAIC = replace MAC_Address0,     f_other -> SA = MAC_Address0, 60 bytes, bytes 12.. unchanged
 *   C   SAIC = insert MAC_Address0,      f_nosa  -> 60 bytes: MAC_Address0 appears at byte 6, type and payload pushed 6 bytes down
 *   D   SAIC = replace MAC_Address1 (bit 25), MAC_Address1 = 02:11:..:55 enabled, f_board -> SA = MAC_Address1
 *   E   same SAIC, MAC_Address1 disabled (AE = 0), f_other -> SA = FF:FF:FF:FF:FF:FF: the MAC_Address1_High/LR reset value, AE ignored
 *                                                     (board 2026-11-18; RM p.2953 says MAC_Address0, which is what the first version expected)
 *   F   SAIC = none, MAC_Configuration.SARC = replace MAC_Address0, f_other -> SA = MAC_Address0: the register works on every frame
 *   G   SARC = replace MAC_Address0 AND SAIC = replace MAC_Address1 (enabled), f_other -> SA = MAC_Address1 if the descriptor wins,
 *                                                                           MAC_Address0 if the register wins (both counted OK, the log says which)
 *                                                                           board 2026-11-18: MAC_Address0, the register SARC wins over the descriptor
 * Cases H: CRC and pad (SAIC = none). Expected at the receiver:
 *   H1a CPC = 01 (no pad), FUP = 0, f_runt (22 + FCS = 26 bytes on the wire) -> DROP: runt, MTL drops it
 *                                                                           (board 2026-11-18: dropped, but Rx_Unicast_Packets_Good still +1:
 *                                                                            the MMC counts at the MAC, before the MTL queue)
 *   H1b same, FUP = 1                                                       -> received, 22 bytes (or 26 if the FCS is kept, p.3106)
 *                                                                           (board 2026-11-18: 22, the FCS of a runt is stripped like any other)
 *   H2a CPC = 10 (no FCS), FEP = 0, f_crc (64 bytes, last 4 = garbage FCS)  -> Rx_CRC_Error_Packets +1, no good frame
 *                                                                           (board 2026-11-18: no descriptor at all, DROP, even with RSF = 0)
 *   H2b same, FEP = 1                                                       -> descriptor with RDES3.ES = 1, CE = 1, MMC +1
 *   H3  CPC = 11 (replace FCS), f_crc                                        -> received good, 60 bytes = f_board (garbage repaired), MMC +0
 *
 * RM0399: SA insertion p.2952-2953; TDES3 SAIC / CPC p.3051-3052; MAC_Configuration.SARC p.3113; MAC_Address1_High p.3155;
 * MTL_RxQ0_Operation_Mode FUP / FEP p.3106; RDES3 ES / CE p.3069; Rx_CRC_Error_Packets p.3163.
 *
 * Board 2026-11-18: 11/12, E BAD (SA = FF:FF:FF:FF:FF:FF instead of MAC_Address0). Expectation of E changed the same day to
 * the MAC_Address1 reset value; G, H1b, H2a, H1a notes above come from the same run. Expected now: 12/12.
 */

#define SA_FRAME_LEN	60			// normal frame: DA SA type + 46 payload bytes
#define SA_NOSA_LEN		54			// frame without SA: DA type + 46 payload bytes (SAIC = insert)
#define SA_RUNT_PAY		8			// payload of the runt frame
#define SA_RUNT_LEN		22			// DA SA type + 8 payload bytes, shorter than the 60-byte minimum
#define SA_FCS_LEN		4			// bytes the CPU appends in place of the FCS (CPC = no FCS / replace FCS)
#define SA_CRC_LEN		(SA_FRAME_LEN + SA_FCS_LEN)	// 64
#define SA_DROP_WAIT_US	5000		// a frame that must NOT come back: wait this long (a looped frame is back in ~200 us)
#define SA_RX_GOOD		1			// sa_case(): a good frame must come back
#define SA_RX_DROP		0			// sa_case(): nothing must come back
#define SA_RX_ERR		2			// sa_case(): a descriptor with RDES3.ES = 1 and CE = 1 must come back (FEP = 1)
#define SA_RX_ANY		3			// sa_case(): descriptor or not, only the MMC CRC counter is checked (FEP = 0)
#define SA_DUMP_PAYLOAD	16			// payload bytes shown by sa_frame_dump()

static const uint8_t SA_DA[ETH_TEST_MAC_SIZE]    = ETH_TEST_DEST_MAC;				// 6C:1F:F7:CA:2B:FD
static const uint8_t SA_BOARD[ETH_TEST_MAC_SIZE] = ETH_TEST_SRC_MAC;				// 00:80:E1:00:00:00 = MAC_Address0
static const uint8_t SA_OTHER[ETH_TEST_MAC_SIZE] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x99};	// a foreign SA
static const uint8_t SA_A1[ETH_TEST_MAC_SIZE]    = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};	// what MAC_Address1 holds when enabled
static const uint8_t SA_FCS_JUNK[SA_FCS_LEN]     = {0xDE, 0xAD, 0xBE, 0xEF};			// stands in for the FCS
static const uint8_t SA_A1_RESET[ETH_TEST_MAC_SIZE] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};	// MAC_Address1_High/LR reset value 0x0000FFFF / 0xFFFFFFFF

static uint8_t sa_f_board[MAC_100M_BUF_SIZE];	// 60: DA MAC_Address0 88B5 payload
static uint8_t sa_f_other[MAC_100M_BUF_SIZE];	// 60: DA 02:00:..:99 88B5 payload
static uint8_t sa_f_a1[MAC_100M_BUF_SIZE];		// 60: DA 02:11:..:55 88B5 payload (expected result of D)
static uint8_t sa_f_a1reset[MAC_100M_BUF_SIZE];	// 60: DA FF:FF:..:FF 88B5 payload (expected result of E, board 2026-11-18)
static uint8_t sa_f_nosa[MAC_100M_BUF_SIZE];	// 54: DA 88B5 payload (no SA)
static uint8_t sa_f_runt[MAC_100M_BUF_SIZE];	// 22: DA MAC_Address0 88B5 + 8 payload bytes
static uint8_t sa_f_crc[MAC_100M_BUF_SIZE];		// 64: f_board + DE AD BE EF
static uint8_t sa_rxbuf[MAC_100M_BUF_SIZE];		// frame received
#define SA_SUMMARY_LEN	160							// "A:OK B:BAD ..." printed before the Result line
static char sa_summary[SA_SUMMARY_LEN];
static int sa_summary_pos;
#define SA_TABLE_HDR	"  id   expect actual  len  SA (bytes 6..11)   bytes 12..15   ES CE  MMC crc | result"

/* Append "id:OK " or "id:BAD " to the summary */
static void sa_summary_add(const char *id, int ok)
{
	const char *r = ok ? ":OK " : ":BAD ";
	int n = (int)strlen(id) + (int)strlen(r);

	if (sa_summary_pos + n < SA_SUMMARY_LEN) {
		memcpy(sa_summary + sa_summary_pos, id, strlen(id));
		memcpy(sa_summary + sa_summary_pos + strlen(id), r, strlen(r));
		sa_summary_pos += n;
		sa_summary[sa_summary_pos] = 0;
	}
}
/* Build one frame: DA | SA (skipped when sa == NULL) | EtherType 0x88B5 | pay_len payload bytes 0x10, 0x11, ... Return its length */
static int sa_build(uint8_t *f, const uint8_t *sa, int pay_len)
{
	int p = 0;
	int i;

	memcpy(f, SA_DA, ETH_TEST_MAC_SIZE);
	p += ETH_TEST_MAC_SIZE;
	if (sa != NULL) {
		memcpy(f + p, sa, ETH_TEST_MAC_SIZE);
		p += ETH_TEST_MAC_SIZE;
	}
	f[p]     = (uint8_t)(ETH_TEST_ETHER_TYPE >> 8);
	f[p + 1] = (uint8_t)(ETH_TEST_ETHER_TYPE & 0xFF);
	p += 2;
	for (i = 0; i < pay_len; i++) {
		f[p + i] = (uint8_t)(0x10 + i);
	}
	return p + pay_len;
}
/* The seven frames, built once */
static void sa_build_frames(void)
{
	int n;

	sa_build(sa_f_board, SA_BOARD, SA_FRAME_LEN - ETH_TEST_HEADER_SIZE);
	sa_build(sa_f_other, SA_OTHER, SA_FRAME_LEN - ETH_TEST_HEADER_SIZE);
	sa_build(sa_f_a1,    SA_A1,    SA_FRAME_LEN - ETH_TEST_HEADER_SIZE);
	sa_build(sa_f_a1reset, SA_A1_RESET, SA_FRAME_LEN - ETH_TEST_HEADER_SIZE);
	sa_build(sa_f_nosa,  NULL,     SA_FRAME_LEN - ETH_TEST_HEADER_SIZE);		// 54 bytes
	sa_build(sa_f_runt,  SA_BOARD, SA_RUNT_PAY);								// 22 bytes
	n = sa_build(sa_f_crc, SA_BOARD, SA_FRAME_LEN - ETH_TEST_HEADER_SIZE);		// 60 + 4 junk bytes = 64
	memcpy(sa_f_crc + n, SA_FCS_JUNK, SA_FCS_LEN);
}
/* Dump of one frame: DA | SA (if has_sa) | type | payload */
static void sa_frame_dump(const char *title, const uint8_t *f, int len, int has_sa)
{
	int i, p = 0;

	eth_printf("      %s: %d bytes\n", title, len);
	eth_printf("        [ 0.. 5] DA     :");
	for (i = 0; i < ETH_TEST_MAC_SIZE; i++) {
		eth_printf(" %02X", f[i]);
	}
	p = ETH_TEST_MAC_SIZE;
	if (has_sa) {
		eth_printf("\n        [ 6..11] SA     :");
		for (i = p; i < p + ETH_TEST_MAC_SIZE; i++) {
			eth_printf(" %02X", f[i]);
		}
		p += ETH_TEST_MAC_SIZE;
	} else {
		eth_printf("\n        [ 6..11] SA     : (none: the frame is given to the MAC without a source address)");
	}
	if (p + 2 <= len) {
		eth_printf("\n        [%2d..%2d] type   : %02X %02X", p, p + 1, f[p], f[p + 1]);
		p += 2;
	}
	eth_printf("\n        [%2d..%2d] payload:", p, len - 1);
	for (i = p; i < p + SA_DUMP_PAYLOAD && i < len; i++) {
		eth_printf(" %02X", f[i]);
	}
	eth_printf("%s (%d bytes)\n", (len - p > SA_DUMP_PAYLOAD) ? " ..." : "", len - p);
}
/* Name of a SAIC value (TDES3 [25:23], p.3052): [24:23] 00 none / 01 insert / 10 replace, bit 25 MAC_Address1 */
static const char *sa_saic_name(uint32_t v)
{
	switch (v & 3U) {
	case TDES3_SAIC_INSERT:  return (v & TDES3_SAIC_MACA1) ? "insert MAC_Address1"  : "insert MAC_Address0";
	case TDES3_SAIC_REPLACE: return (v & TDES3_SAIC_MACA1) ? "replace MAC_Address1" : "replace MAC_Address0";
	case 3U:                 return "reserved";
	default:                 return "none";
	}
}
/* Name of a SARC value (MAC_Configuration [30:28], p.3113): [29:28] 10 insert / 11 replace / 0x off, bit 30 MAC_Address1. Not the SAIC encoding */
static const char *sa_sarc_name(uint32_t v)
{
	switch (v & 3U) {
	case MAC_CONFIGURATION_SARC_INSERT_A0:  return (v & MAC_CONFIGURATION_SARC_MACA1) ? "insert MAC_Address1"  : "insert MAC_Address0";
	case MAC_CONFIGURATION_SARC_REPLACE_A0: return (v & MAC_CONFIGURATION_SARC_MACA1) ? "replace MAC_Address1" : "replace MAC_Address0";
	default:                    return "none";
	}
}
/* Name of a CPC value (TDES3 [27:26], p.3051) */
static const char *sa_cpc_name(uint32_t v)
{
	switch (v) {
	case TDES3_CPC_CRC_NOPAD:   return "01 FCS, no pad";
	case TDES3_CPC_NONE:        return "10 no FCS, no pad";
	case TDES3_CPC_CRC_REPLACE: return "11 replace FCS";
	default:                    return "00 FCS + pad";
	}
}
/**
 * One case: registers -> counters -> send with SAIC / CPC in TDES3 -> receive -> check -> one result line -> frame dumps.
 *
 *   Register / descriptor   Bit [pos]         Value        Function
 *   ----------------------- ----------------- ------------ ------------------------------
 *   MAC_Configuration p.3113        SARC [30:28]      sarc         SA insertion / replacement for every frame (0 = off)
 *   MAC_Address1_High p.3155      AE [31]           a1 != NULL   MAC_Address1 enabled (also a DA filter entry, harmless with PR = 1)
 *                           ADDRHI [15:0]     a1[5:4]
 *   MAC_Address1_Low p.3154      ADDRLO [31:0]     a1[3:0]
 *   MTL_RxQ0_Operation_Mode p.3106    FUP [3]           rxq & FUP    forward undersized good frames
 *                           FEP [4]           rxq & FEP    forward frames with an error
 *   TDES3 p.3051 (this TX)  SAIC [25:23]      saic         eth_tx_saic -> egress_packet()
 *                           CPC [27:26]       cpc          eth_tx_cpc  -> egress_packet()
 *   RDES3 p.3069 (RX)       ES [15], CE [24]  read         error summary, CRC error (expect == SA_RX_ERR)
 *   MMC p.3163              RX_CRC_ERROR      read         +exp_crc expected
 *
 * @param expect     SA_RX_GOOD: a good frame, compared with exp (exp_len bytes); SA_RX_DROP: nothing;
 *                   SA_RX_ERR: a descriptor with ES + CE; SA_RX_ANY: not checked (only the MMC counter)
 * @param exp        the frame expected in the RX buffer (SA_RX_GOOD), exp_len bytes
 * @param alt_sa     second SA accepted at bytes 6..11 (case G), NULL otherwise
 * @param allow_fcs  1: exp_len + 4 bytes also accepted (FUP keeps the FCS of a runt, p.3106)
 * @param exp_crc    expected increment of Rx_CRC_Error_Packets
 * Return 1: as expected, 0: else
 */
static int sa_case(uint8_t mac_idx, const char *id, uint32_t saic, uint32_t cpc, uint32_t sarc, const uint8_t *a1,
				   uint32_t rxq, const uint8_t *frame, int len, int frame_has_sa,
				   int expect, const uint8_t *exp, int exp_len, const uint8_t *alt_sa, int allow_fcs, uint32_t exp_crc,
				   const char *what)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t maccr, rxqomr;
	uint32_t rdes2 = 0, rdes3 = 0;
	uint32_t crc0, crc1, good0, good1;
	uint32_t wait, k;
	int tx_len, rx_len;
	int got, err, alt_hit = 0;
	int ok;

	/* 0. description, setup and expectation before anything is sent */
	eth_printf("\n  --- case %s: %s\n", id, what);
	eth_printf("      setup : TDES3.SAIC = %lu (%s), TDES3.CPC = %s; MAC_Configuration.SARC = %lu (%s); MAC_Address1 %s; MTL_RxQ0_Operation_Mode FUP = %d, FEP = %d\n",
			   (unsigned long)saic, sa_saic_name(saic), sa_cpc_name(cpc), (unsigned long)sarc, sa_sarc_name(sarc),
			   a1 ? "enabled = 02:11:22:33:44:55" : "disabled (AE = 0)",
			   (rxq & MTL_RXQ0_OPERATION_MODE_FUP) != 0, (rxq & MTL_RXQ0_OPERATION_MODE_FEP) != 0);
	if (expect == SA_RX_GOOD) {
		eth_printf("      Expect: good frame, %d bytes%s, SA = %02X:%02X:%02X:%02X:%02X:%02X", exp_len,
				   allow_fcs ? " (or +4 with the FCS)" : "", exp[6], exp[7], exp[8], exp[9], exp[10], exp[11]);
		if (alt_sa != NULL) {
			eth_printf(" or %02X:%02X:%02X:%02X:%02X:%02X", alt_sa[0], alt_sa[1], alt_sa[2], alt_sa[3], alt_sa[4], alt_sa[5]);
		}
		eth_printf(", MMC crc +%lu\n", (unsigned long)exp_crc);
	} else if (expect == SA_RX_DROP) {
		eth_printf("      Expect: DROP, no RX descriptor within %d us, MMC crc +%lu\n", SA_DROP_WAIT_US, (unsigned long)exp_crc);
	} else if (expect == SA_RX_ERR) {
		eth_printf("      Expect: RX descriptor with ES = 1 and CE = 1 (CRC error forwarded), MMC crc +%lu\n", (unsigned long)exp_crc);
	} else {
		eth_printf("      Expect: MMC crc +%lu; descriptor or not is what the MTL decides with FEP = 0 (printed, not checked)\n",
				   (unsigned long)exp_crc);
	}
	/* 1. the transmitter rule for every frame (SARC), MAC_Address1, and the receiver forwarding rules (FUP / FEP) */
	maccr = AX_BASE_READ_REG32(mac_base, R_MAC_CONFIGURATION) & ~(MAC_CONFIGURATION_SARC_MASK << MAC_CONFIGURATION_SARC_bit);
	maccr |= sarc << MAC_CONFIGURATION_SARC_bit;										// SARC [30:28]: SA action for every frame
	AX_BASE_WRITE_REG32(mac_base, R_MAC_CONFIGURATION, maccr);
	if (a1 != NULL) {
		AX_BASE_WRITE_REG32(mac_base, R_MAC_ADDRESS1_HIGH, MAC_ADDRESS_HIGH_AE					// AE [31]: MAC_Address1 enabled
						  | ((uint32_t)a1[5] << 8) | a1[4]);				// ADDRHI [15:0]: byte 5:4
		AX_BASE_WRITE_REG32(mac_base, R_MAC_ADDRESS1_LOW, ((uint32_t)a1[3] << 24) | ((uint32_t)a1[2] << 16)
						  | ((uint32_t)a1[1] << 8) | a1[0]);				// ADDRLO [31:0]: byte 3:0
	} else {
		AX_BASE_WRITE_REG32(mac_base, R_MAC_ADDRESS1_HIGH, 0x0000FFFFU);				// reset value: AE = 0
		AX_BASE_WRITE_REG32(mac_base, R_MAC_ADDRESS1_LOW, 0xFFFFFFFFU);
	}
	rxqomr = AX_BASE_READ_REG32(mac_base, R_MTL_RXQ0_OPERATION_MODE) & ~(uint32_t)(MTL_RXQ0_OPERATION_MODE_FUP | MTL_RXQ0_OPERATION_MODE_FEP);
	rxqomr |= rxq & (MTL_RXQ0_OPERATION_MODE_FUP | MTL_RXQ0_OPERATION_MODE_FEP);						// FUP [3], FEP [4]
	AX_BASE_WRITE_REG32(mac_base, R_MTL_RXQ0_OPERATION_MODE, rxqomr);
	flow_regw("MAC_Configuration (SARC)", mac_base, R_MAC_CONFIGURATION, maccr, FF_MAC_CONFIGURATION);
	flow_regw("MAC_Address1_High", mac_base, R_MAC_ADDRESS1_HIGH, AX_BASE_READ_REG32(mac_base, R_MAC_ADDRESS1_HIGH), FF_NONE);
	flow_regw("MTL_RxQ0_Operation_Mode (FUP/FEP)", mac_base, R_MTL_RXQ0_OPERATION_MODE, rxqomr, FF_MTL_RXQ0_OPERATION_MODE);
	crc0  = AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_CRC_ERR);
	good0 = AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_UNICAST_GOOD);
	/* 2. the frame, with SAIC and CPC in TDES3 of its descriptor; back to plain frames right after */
	eth_tx_saic = (uint8_t)saic;
	eth_tx_cpc  = (uint8_t)cpc;
	tx_len = egress_packet((unsigned char *)frame, len);
	eth_tx_saic = TDES3_SAIC_NONE;
	eth_tx_cpc  = TDES3_CPC_CRC_PAD;
	if (tx_len != len) {
		eth_printf("  %s  TX error %d\n", id, tx_len);
		sa_summary_add(id, 0);
		return 0;
	}
	/* 3. receive: a dropped frame never gets a descriptor (timeout), an error frame gets one with ES = 1 (-3) */
	wait = (expect == SA_RX_DROP || expect == SA_RX_ANY) ? SA_DROP_WAIT_US : ETH_TEST_RX_TIMEOUT_US;
	rx_len = ingress_packet_status(mac_idx, sa_rxbuf, sizeof(sa_rxbuf), (int)wait, &rdes2, &rdes3);
	got = (rx_len >= 0);
	err = (rx_len == -3);
	crc1  = AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_CRC_ERR);
	good1 = AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_UNICAST_GOOD);
	/* 4. check */
	if (expect == SA_RX_GOOD) {
		ok = got && (rx_len == exp_len || (allow_fcs && rx_len == exp_len + SA_FCS_LEN));
		if (ok && alt_sa != NULL && memcmp(sa_rxbuf + ETH_TEST_MAC_SIZE, alt_sa, ETH_TEST_MAC_SIZE) == 0) {
			alt_hit = 1;												// G: the register won, compare the rest only
			ok = (memcmp(sa_rxbuf, exp, ETH_TEST_MAC_SIZE) == 0)
			  && (memcmp(sa_rxbuf + ETH_TEST_HEADER_SIZE - 2, exp + ETH_TEST_HEADER_SIZE - 2, (size_t)(exp_len - ETH_TEST_HEADER_SIZE + 2)) == 0);
		} else if (ok) {
			ok = (memcmp(sa_rxbuf, exp, (size_t)exp_len) == 0);
		}
	} else if (expect == SA_RX_DROP) {
		ok = !got && !err;
	} else if (expect == SA_RX_ERR) {
		ok = err && (rdes3 & RDES3_ES) && (rdes3 & RDES3_CE);
	} else {
		ok = !got;														// SA_RX_ANY: a good frame would be wrong, a dropped or error one is fine
	}
	ok = ok && ((crc1 - crc0) == exp_crc);
	/* 5. column header + one line */
	eth_printf(SA_TABLE_HDR "\n");
	eth_printf("  %-4s %-6s %-6s ", id,
			   (expect == SA_RX_GOOD) ? "GOOD" : (expect == SA_RX_DROP) ? "DROP" : (expect == SA_RX_ERR) ? "ERR" : "any",
			   got ? "GOOD" : err ? "ERR" : "DROP");
	if (got) {
		eth_printf("%3d  ", rx_len);
		for (k = ETH_TEST_MAC_SIZE; k < ETH_TEST_HEADER_SIZE - 2; k++) {
			eth_printf("%02X ", sa_rxbuf[k]);
		}
		eth_printf(" ");
		for (k = ETH_TEST_HEADER_SIZE - 2; k < ETH_TEST_HEADER_SIZE + 2; k++) {
			eth_printf("%02X ", (int)k < rx_len ? sa_rxbuf[k] : 0);
		}
		eth_printf("  %d  %d   ", (rdes3 & RDES3_ES) != 0, (rdes3 & RDES3_CE) != 0);
	} else if (err) {
		eth_printf("  -  (error frame, not copied)             %d  %d   ", (rdes3 & RDES3_ES) != 0, (rdes3 & RDES3_CE) != 0);
	} else {
		eth_printf("  -  (no frame)                            -  -   ");
	}
	eth_printf("+%-5lu | %s%s\n", (unsigned long)(crc1 - crc0), ok ? "OK " : "BAD",
			   alt_hit ? " (register SARC won over the descriptor)" : (alt_sa && ok) ? " (descriptor SAIC won over the register)" : "");
	if (!ok) {
		eth_printf("      RDES3 = 0x%08lX, RDES2 = 0x%08lX, MMC unicast good +%lu, crc +%lu (expected +%lu)\n",
				   (unsigned long)rdes3, (unsigned long)rdes2, (unsigned long)(good1 - good0),
				   (unsigned long)(crc1 - crc0), (unsigned long)exp_crc);
	}
	/* 6. both frames */
	sa_frame_dump("frame sent", frame, len, frame_has_sa);
	if (got) {
		sa_frame_dump("frame received", sa_rxbuf, rx_len, 1);
	}
	sa_summary_add(id, ok);
	return ok;
}
/**
 * SA insertion / replacement and CRC-pad control test through the MAC loopback (eth_run_test_ops = 10), cases A-H3.
 * Same bits as eth_mac_internal_lb() (eth_functions.c) plus, per case (see sa_case): MAC_Configuration.SARC, MAC_Address1_High/LR,
 * MTL_RxQ0_Operation_Mode FUP / FEP, TDES3 SAIC / CPC. Everything is put back at the end (SARC = 0, MAC_Address1 off, FUP = FEP = 0).
 * Return 1: every case as expected, 0: else
 */
uint16_t eth_mac_sa_cpc_lb(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t ok = 0;
	uint32_t reg;

	eth_printf("Start Init: Port[%d]\n", mac_idx);
	if (eth_reset_mac(mac_idx) == 0) {
		return 0;
	}
	mac_init_100m(mac_idx);
	init_mac_internal_lb(mac_idx);
	mac_start(mac_idx);
	sa_build_frames();
	sa_summary_pos = 0;
	sa_summary[0] = 0;

	eth_printf("\n================ SA INSERTION / REPLACEMENT + CRC-PAD CONTROL TEST (TDES3 SAIC / CPC, MAC_Configuration.SARC, MTL_RxQ0_Operation_Mode FUP / FEP) ================\n");
	eth_printf("  The transmitter changes the frame on its own: SA inserted or replaced with MAC_Address0 / MAC_Address1 (SAIC per frame, SARC for all),\n");
	eth_printf("  pad and FCS added, left out or recomputed (CPC). The receiver shows the result: SA at bytes 6..11, length,\n");
	eth_printf("  RDES3 ES / CE for a CRC error (p.3069), Rx_CRC_Error_Packets (p.3163). FUP / FEP (p.3106) decide whether\n");
	eth_printf("  a runt or an error frame reaches a descriptor at all. expect/actual: GOOD = good frame, ERR = descriptor with ES, DROP = none.\n");
	eth_printf("  case B prints the [10]/[11] FLOW lines.\n");

	/* A-C: SAIC in the descriptor with MAC_Address0 */
	eth_printf("=======================================================================================\n");
	ok += sa_case(mac_idx, "A",  TDES3_SAIC_NONE, TDES3_CPC_CRC_PAD, MAC_CONFIGURATION_SARC_NONE, NULL, 0, sa_f_other, SA_FRAME_LEN, 1,
				  SA_RX_GOOD, sa_f_other, SA_FRAME_LEN, NULL, 0, 0,
				  "SAIC = none: the foreign SA 02:00:00:00:00:99 goes through untouched");
	eth_printf("=======================================================================================\n");
	eth_flow_trace = 1;
	ok += sa_case(mac_idx, "B",  TDES3_SAIC_REPLACE, TDES3_CPC_CRC_PAD, MAC_CONFIGURATION_SARC_NONE, NULL, 0, sa_f_other, SA_FRAME_LEN, 1,
				  SA_RX_GOOD, sa_f_board, SA_FRAME_LEN, NULL, 0, 0,
				  "SAIC = replace MAC_Address0: bytes 6..11 become 00:80:E1:00:00:00, length unchanged");
	eth_flow_trace = 0;
	eth_printf("=======================================================================================\n");
	ok += sa_case(mac_idx, "C",  TDES3_SAIC_INSERT, TDES3_CPC_CRC_PAD, MAC_CONFIGURATION_SARC_NONE, NULL, 0, sa_f_nosa, SA_NOSA_LEN, 0,
				  SA_RX_GOOD, sa_f_board, SA_FRAME_LEN, NULL, 0, 0,
				  "SAIC = insert MAC_Address0: 54-byte frame without SA comes back as 60 bytes, MAC_Address0 at byte 6, type at byte 12");
	/* D-E: MAC_Address1 as the source, enabled or not */
	eth_printf("=======================================================================================\n");
	ok += sa_case(mac_idx, "D",  TDES3_SAIC_REPLACE | TDES3_SAIC_MACA1, TDES3_CPC_CRC_PAD, MAC_CONFIGURATION_SARC_NONE, SA_A1, 0, sa_f_board, SA_FRAME_LEN, 1,
				  SA_RX_GOOD, sa_f_a1, SA_FRAME_LEN, NULL, 0, 0,
				  "SAIC = replace MAC_Address1 (bit 25), MAC_Address1 enabled: SA becomes 02:11:22:33:44:55");
	eth_printf("=======================================================================================\n");
	ok += sa_case(mac_idx, "E",  TDES3_SAIC_REPLACE | TDES3_SAIC_MACA1, TDES3_CPC_CRC_PAD, MAC_CONFIGURATION_SARC_NONE, NULL, 0, sa_f_other, SA_FRAME_LEN, 1,
				  SA_RX_GOOD, sa_f_a1reset, SA_FRAME_LEN, NULL, 0, 0,
				  "same SAIC, MAC_Address1 disabled (AE = 0): the MAC still copies MAC_Address1_High/LR = reset value FF:FF:FF:FF:FF:FF (board 11-18; RM p.2953 says MAC_Address0)");
	/* F-G: the register MAC_Configuration.SARC, alone and against the descriptor */
	eth_printf("=======================================================================================\n");
	ok += sa_case(mac_idx, "F",  TDES3_SAIC_NONE, TDES3_CPC_CRC_PAD, MAC_CONFIGURATION_SARC_REPLACE_A0, NULL, 0, sa_f_other, SA_FRAME_LEN, 1,
				  SA_RX_GOOD, sa_f_board, SA_FRAME_LEN, NULL, 0, 0,
				  "SAIC = none but MAC_Configuration.SARC = replace MAC_Address0: the register works on every frame");
	eth_printf("=======================================================================================\n");
	ok += sa_case(mac_idx, "G",  TDES3_SAIC_REPLACE | TDES3_SAIC_MACA1, TDES3_CPC_CRC_PAD, MAC_CONFIGURATION_SARC_REPLACE_A0, SA_A1, 0, sa_f_other, SA_FRAME_LEN, 1,
				  SA_RX_GOOD, sa_f_a1, SA_FRAME_LEN, SA_BOARD, 0, 0,
				  "SARC = replace MAC_Address0 and SAIC = replace MAC_Address1: who wins? SA = MAC_Address1 (descriptor) or MAC_Address0 (register), both OK (board 11-18: MAC_Address0)");
	/* H1: no pad -> runt; FUP decides */
	eth_printf("=======================================================================================\n");
	ok += sa_case(mac_idx, "H1a", TDES3_SAIC_NONE, TDES3_CPC_CRC_NOPAD, MAC_CONFIGURATION_SARC_NONE, NULL, 0, sa_f_runt, SA_RUNT_LEN, 1,
				  SA_RX_DROP, NULL, 0, NULL, 0, 0,
				  "CPC = FCS without pad, 22-byte frame, FUP = 0: a runt, the MTL RX queue drops it");
	eth_printf("=======================================================================================\n");
	ok += sa_case(mac_idx, "H1b", TDES3_SAIC_NONE, TDES3_CPC_CRC_NOPAD, MAC_CONFIGURATION_SARC_NONE, NULL, MTL_RXQ0_OPERATION_MODE_FUP, sa_f_runt, SA_RUNT_LEN, 1,
				  SA_RX_GOOD, sa_f_runt, SA_RUNT_LEN, NULL, 1, 0,
				  "same runt, FUP = 1: forwarded, 22 bytes (26 if the FCS is kept with it, p.3106; board 11-18: 22)");
	/* H2: no FCS -> CRC error; FEP decides */
	eth_printf("=======================================================================================\n");
	ok += sa_case(mac_idx, "H2a", TDES3_SAIC_NONE, TDES3_CPC_NONE, MAC_CONFIGURATION_SARC_NONE, NULL, 0, sa_f_crc, SA_CRC_LEN, 1,
				  SA_RX_ANY, NULL, 0, NULL, 0, 1,
				  "CPC = no FCS, 64 bytes with DE AD BE EF as FCS, FEP = 0: CRC error counted, no good frame (board 11-18: no descriptor)");
	eth_printf("=======================================================================================\n");
	ok += sa_case(mac_idx, "H2b", TDES3_SAIC_NONE, TDES3_CPC_NONE, MAC_CONFIGURATION_SARC_NONE, NULL, MTL_RXQ0_OPERATION_MODE_FEP, sa_f_crc, SA_CRC_LEN, 1,
				  SA_RX_ERR, NULL, 0, NULL, 0, 1,
				  "same frame, FEP = 1: the error frame is forwarded, RDES3 ES = 1 and CE = 1");
	/* H3: the MAC repairs the FCS */
	eth_printf("=======================================================================================\n");
	ok += sa_case(mac_idx, "H3", TDES3_SAIC_NONE, TDES3_CPC_CRC_REPLACE, MAC_CONFIGURATION_SARC_NONE, NULL, 0, sa_f_crc, SA_CRC_LEN, 1,
				  SA_RX_GOOD, sa_f_board, SA_FRAME_LEN, NULL, 0, 0,
				  "CPC = replace FCS: DE AD BE EF recomputed, received good as 60 bytes (CST strips the FCS)");

	/* back to the defaults for the other tests: SARC off, MAC_Address1 off, FUP = FEP = 0 */
	reg = AX_BASE_READ_REG32(mac_base, R_MAC_CONFIGURATION) & ~(MAC_CONFIGURATION_SARC_MASK << MAC_CONFIGURATION_SARC_bit);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_CONFIGURATION, reg);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_ADDRESS1_HIGH, 0x0000FFFFU);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_ADDRESS1_LOW, 0xFFFFFFFFU);
	reg = AX_BASE_READ_REG32(mac_base, R_MTL_RXQ0_OPERATION_MODE) & ~(uint32_t)(MTL_RXQ0_OPERATION_MODE_FUP | MTL_RXQ0_OPERATION_MODE_FEP);
	AX_BASE_WRITE_REG32(mac_base, R_MTL_RXQ0_OPERATION_MODE, reg);

	eth_printf("\nCases: %s\n", sa_summary);
	eth_printf("Result: %lu/12 cases as expected.\n", (unsigned long)ok);
	eth_print_mac_counters(mac_idx);
	return (ok == 12) ? 1 : 0;
}
