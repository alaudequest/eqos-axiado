#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_addr_utils.h"
#include "eth_traffic_test.h"
#include "eth_printf.h"
#include "eth_flow_utils.h"

/*
 * MAC address filter of the RECEIVER through the MAC loopback (eth_run_test_ops = 11): who gets in, who is dropped.
 * Every other test runs with MAC_Packet_Filter.PR = 1 (promiscuous: every DA accepted). Here PR = 0 for the whole test and
 * the frame's DA / SA is compared with the 4 address slots MAC_Address0..3 and the 64-bit hash table (RM0399 p.2955-2956,
 * MAC_Packet_Filter p.3118-3120, MAC_Address(#i)_High p.3153/3155, MAC_Hash_Table_Reg0/Reg1 p.3121):
 *
 *     CPU frame (DA, SA chosen per case) -> MAC TX (untouched) -> loopback -> MAC RX address filter (AFM)
 *        broadcast  : pass, unless DBF = 1
 *        multicast  : pass if PM = 1; else perfect compare with the enabled slots (HMC = 0) or hash table (HMC = 1)
 *        unicast    : perfect compare with MAC_Address0 + enabled slots (HUC = 0) or hash table (HUC = 1); HPF = 1: either
 *        SA         : slot with its SA bit = 1 compares the SA instead; SAF = 1 drops a frame that fails it
 *        DAIF / SAIF: the result is inverted
 *        pass -> RX descriptor, RDES2 (with RS2V = 1): MADRM [26:19] = slot that matched, HF [18] = hash passed (then MADRM = hash value),
 *                DAF [17] / SAF [16] = failed (RA = 1 only)
 *        fail -> dropped, no RX descriptor
 *
 * Address slots (MAC_Address(#i)_High/Low): MAC_Address0 is always on (AE reads 1). MAC_Address1..3 have AE [31], SA [30] (compare the SA field
 * instead of the DA), MBC [29:24] (bit 24 + k: do not compare address byte k; LR holds bytes 0..3, HR bytes 4..5).
 * Hash table: index = top 6 bits of bitrev(~crc32(DA)) (p.3121, the way the Linux stmmac driver does it); bit 5 picks
 * MAC_Hash_Table_Reg1, bits [4:0] the bit. Mode 1 (eth_filter_functions.c, board 2026-10-16, 24/24) verified this recipe: 5 frames
 * passed through the hash with RDES2.HF = 1 and MADRM = the computed index. Case K1 prints both again.
 *
 * Addresses (DA unless said; all frames 60 bytes, EtherType 0x88B5, payload 0x10, 0x11, ...):
 *   MAC_Address0   00:80:E1:00:00:00 (board)      uni_x   02:00:00:00:00:99 (unknown unicast)   bcast FF:FF:FF:FF:FF:FF
 *   a1      02:11:22:33:44:55 (MAC_Address1)      a1_near 02:11:22:33:44:56 (last byte differs)  a1_grp 02:11:22:AA:BB:CC (same first 3 bytes)
 *   mc_ptp  01:1B:19:00:00:00 (MAC_Address2)      mc_x    01:00:5E:01:02:03 (unknown multicast)
 *   sa_ok   02:AA:BB:CC:DD:EE (MAC_Address3, SA)  sa_x    02:AA:BB:CC:DD:EF
 *   Hash index (af_hash_index): MAC_Address0 = 13, uni_x = 4, a1 = 14, mc_x = 6 (no two test addresses share a bit)
 *
 * Cases, PR = 0 everywhere, slots 1..3 off and hash table 0 unless said (DA = MAC_Address0 and SA = uni_x unless said):
 *   A   MAC_Packet_Filter = 0                                      -> PASS  MAC_Address0 is always on; MADRM = 0
 *   B   DA = uni_x                                      -> DROP  no slot matches
 *   C   DA = bcast                                      -> PASS  broadcast is never filtered (DBF = 0)
 *   D   DBF = 1, DA = bcast                             -> DROP  "overrides all other filter settings" (p.3119)
 *   E   MAC_Address1 = a1 (AE = 1), DA = a1                    -> PASS  second unicast address; MADRM = 1
 *   F   same, DA = a1_near                              -> DROP  48-bit compare
 *   G   MAC_Address1 = a1 with MBC = bytes 3..5 masked, DA = a1_grp -> PASS  group filter on the first 3 bytes (OUI)
 *   H   MAC_Address1 = a1 written with AE = 0, DA = a1         -> DROP  the slot holds the address but is off
 *   I   MAC_Address2 = mc_ptp (AE = 1), PM = 0, DA = mc_ptp    -> PASS  multicast through a perfect slot (HMC = 0); MADRM = 2
 *   J1  PM = 1, DA = mc_x                               -> PASS  all multicast
 *   J2  PM = 0, DA = mc_x                               -> DROP  unknown multicast, no slot
 *   K1  HUC = 1, hash bit of uni_x set, DA = uni_x      -> PASS  unicast through the hash table; HF = 1, MADRM = hash value
 *   K2  HUC = 1, HPF = 0, DA = MAC_Address0                    -> DROP  hash mode only: the perfect slots are ignored (p.3119 HPF)
 *   K3  HUC = 1, HPF = 1, DA = MAC_Address0                    -> PASS  hash OR perfect
 *   L   MAC_Address3 = sa_ok with SA = 1, SAF = 1, SA = sa_ok  -> PASS  source address filter
 *   M   same, SA = sa_x                                 -> DROP  SAF = 1 drops a frame whose SA fails
 *   N1  DAIF = 1, DA = MAC_Address0                            -> DROP  inverse DA filter
 *   N2  DAIF = 1, DA = uni_x                            -> PASS
 * A, E, I, K1 print RDES2 (RS2V, SAF, DAF, HF, MADRM): informational. Mode 1 showed RS2V = 1 only for a MAC_Address1 match (HF = 0,
 * MADRM = 1), a hash match (HF = 1, MADRM = index) and RA = 1 (DAF = 1); a MAC_Address0 / broadcast / PM / SA match leaves RS2V = 0.
 * MMC: Rx_Unicast_Packets_Good counts before the filter (mode 8 board 2026-11-12) and only unicast frames.
 *
 * Mode 1 (eth_run_test_ops = 1, CASES[] table, 24 cases) tested the same registers and passed 24/24 on 2026-10-16. This mode
 * adds MAC_Address2 / MAC_Address3, MBC on 3 bytes, AE = 0 with an address loaded, the controls J2 K2 N2 and a RDES2 column per case.
 *
 * RM0399: filtering p.2954-2957 (Table 549 DA filter, Table 550 SA filter); MAC_Packet_Filter p.3118-3120; MAC_Address0_High p.3153,
 * MAC_Address(#i)_High p.3155, MAC_Address(#i)_Low p.3154; MAC_Hash_Table_Reg0/Reg1 p.3121-3122; RDES2 write-back p.3068.
 */

#define AF_FRAME_LEN	60			// DA SA type + 46 payload bytes
#define AF_DROP_WAIT_US	5000		// a frame that must NOT come back: wait this long (a looped frame is back in ~200 us)
#define AF_PASS			1			// af_case(): the frame must be received
#define AF_DROP			0			// af_case(): the frame must be dropped
#define AF_SLOT_NONE	0			// af_case(): no slot written (MAC_Address1..3 stay off)
#define AF_SLOT_CNT		4			// MAC_Address0..3
#define AF_SLOT_STRIDE	8			// MAC_Address(#i)_High = 0x0300 + 8 * x
#define AF_HASH_NONE	-1			// af_case(): hash table left at 0
#define AF_HASH_BITS	64			// MAC_Hash_Table_Reg0 + MAC_Hash_Table_Reg1
#define AF_HASH_SHIFT	26			// top 6 bits of the reversed CRC-32
#define AF_CRC32_POLY	0xEDB88320UL	// CRC-32 (IEEE 802.3), reflected
#define AF_MBC_BYTE(k)	(1U << (MAC_ADDRESS_HIGH_MBC_bit + (k)))	// MBC bit for address byte k (0..5)
#define AF_MBC_OUI		(AF_MBC_BYTE(3) | AF_MBC_BYTE(4) | AF_MBC_BYTE(5))	// compare bytes 0..2 only
#define AF_DUMP_PAYLOAD	16			// payload bytes shown by af_frame_dump()
#define AF_RDES2_NONE	-1			// af_case(): RDES2 not printed
#define AF_SUMMARY_LEN	200			// "A:OK B:BAD ..." printed before the Result line
#define AF_TABLE_HDR	"  id  expect actual  len  DA (bytes 0..5)    SA (bytes 6..11)   RS2V SAF DAF HF MADRM  MMC rx | result"

static const uint8_t AF_MACA0[ETH_TEST_MAC_SIZE]  = ETH_TEST_SRC_MAC;					// 00:80:E1:00:00:00
static const uint8_t AF_UNI_X[ETH_TEST_MAC_SIZE]  = {0x02, 0x00, 0x00, 0x00, 0x00, 0x99};
static const uint8_t AF_BCAST[ETH_TEST_MAC_SIZE]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t AF_A1[ETH_TEST_MAC_SIZE]     = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t AF_A1_NEAR[ETH_TEST_MAC_SIZE] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x56};
static const uint8_t AF_A1_GRP[ETH_TEST_MAC_SIZE] = {0x02, 0x11, 0x22, 0xAA, 0xBB, 0xCC};
static const uint8_t AF_MC_PTP[ETH_TEST_MAC_SIZE] = {0x01, 0x1B, 0x19, 0x00, 0x00, 0x00};
static const uint8_t AF_MC_X[ETH_TEST_MAC_SIZE]   = {0x01, 0x00, 0x5E, 0x01, 0x02, 0x03};
static const uint8_t AF_SA_OK[ETH_TEST_MAC_SIZE]  = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
static const uint8_t AF_SA_X[ETH_TEST_MAC_SIZE]   = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEF};

static uint8_t af_txbuf[MAC_100M_BUF_SIZE];		// the frame of the current case
static uint8_t af_rxbuf[MAC_100M_BUF_SIZE];		// frame received
static char af_summary[AF_SUMMARY_LEN];
static int af_summary_pos;

/* Append "id:OK " or "id:BAD " to the summary */
static void af_summary_add(const char *id, int ok)
{
	const char *r = ok ? ":OK " : ":BAD ";
	int n = (int)strlen(id) + (int)strlen(r);

	if (af_summary_pos + n < AF_SUMMARY_LEN) {
		memcpy(af_summary + af_summary_pos, id, strlen(id));
		memcpy(af_summary + af_summary_pos + strlen(id), r, strlen(r));
		af_summary_pos += n;
		af_summary[af_summary_pos] = 0;
	}
}
/* Build the frame: DA | SA | EtherType 0x88B5 | 46 payload bytes 0x10, 0x11, ... Return its length */
static int af_build(uint8_t *f, const uint8_t *da, const uint8_t *sa)
{
	int i;

	memcpy(f, da, ETH_TEST_MAC_SIZE);
	memcpy(f + ETH_TEST_MAC_SIZE, sa, ETH_TEST_MAC_SIZE);
	f[ETH_TEST_HEADER_SIZE - 2] = (uint8_t)(ETH_TEST_ETHER_TYPE >> 8);
	f[ETH_TEST_HEADER_SIZE - 1] = (uint8_t)(ETH_TEST_ETHER_TYPE & 0xFF);
	for (i = 0; i < AF_FRAME_LEN - ETH_TEST_HEADER_SIZE; i++) {
		f[ETH_TEST_HEADER_SIZE + i] = (uint8_t)(0x10 + i);
	}
	return AF_FRAME_LEN;
}
/* "02:11:22:33:44:55" */
static void af_print_addr(const uint8_t *a)
{
	eth_printf("%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}
/* Dump of one frame: DA | SA | type | payload */
static void af_frame_dump(const char *title, const uint8_t *f, int len)
{
	int i;

	eth_printf("      %s: %d bytes\n", title, len);
	eth_printf("        [ 0.. 5] DA     :");
	for (i = 0; i < ETH_TEST_MAC_SIZE; i++) {
		eth_printf(" %02X", f[i]);
	}
	eth_printf("\n        [ 6..11] SA     :");
	for (i = ETH_TEST_MAC_SIZE; i < 2 * ETH_TEST_MAC_SIZE; i++) {
		eth_printf(" %02X", f[i]);
	}
	eth_printf("\n        [12..13] type   : %02X %02X", f[ETH_TEST_HEADER_SIZE - 2], f[ETH_TEST_HEADER_SIZE - 1]);
	eth_printf("\n        [%2d..%2d] payload:", ETH_TEST_HEADER_SIZE, len - 1);
	for (i = ETH_TEST_HEADER_SIZE; i < ETH_TEST_HEADER_SIZE + AF_DUMP_PAYLOAD && i < len; i++) {
		eth_printf(" %02X", f[i]);
	}
	eth_printf("%s (%d bytes)\n", (len - ETH_TEST_HEADER_SIZE > AF_DUMP_PAYLOAD) ? " ..." : "", len - ETH_TEST_HEADER_SIZE);
}
/* Bit reversal of a 32-bit word (bit 0 <-> bit 31) */
static uint32_t af_bitrev32(uint32_t x)
{
	uint32_t r = 0;
	int i;

	for (i = 0; i < 32; i++) {
		r = (r << 1) | ((x >> i) & 1U);
	}
	return r;
}
/**
 * Index (0..63) of a MAC address in the 64-bit hash table (p.3121): CRC-32 of the 6 bytes (reflected, init all 1s,
 * bit 0 of byte 0 first, as on the wire), complemented, bit-reversed, top 6 bits. Same recipe as the Linux stmmac
 * driver (bitrev32(~crc32_le(~0, addr, 6)) >> 26); mode 1 verified it on the board (RDES2.MADRM = this index when HF = 1).
 */
static int af_hash_index(const uint8_t *addr)
{
	uint32_t crc = 0xFFFFFFFFUL;
	uint32_t bit;
	int i, k;

	for (k = 0; k < ETH_TEST_MAC_SIZE; k++) {
		for (i = 0; i < 8; i++) {
			bit = ((uint32_t)addr[k] >> i) & 1U;
			if (((crc & 1U) ^ bit) != 0U) {
				crc = (crc >> 1) ^ AF_CRC32_POLY;
			} else {
				crc >>= 1;
			}
		}
	}
	return (int)(af_bitrev32(~crc) >> AF_HASH_SHIFT);
}
/* Slots 1..3 off (reset values, p.3155/3154), hash table empty: the state every case starts from */
static void af_slots_reset(uint32_t mac_base)
{
	int x;

	for (x = 1; x < AF_SLOT_CNT; x++) {
		AX_BASE_WRITE_REG32(mac_base, R_MAC_ADDRESS1_LOW + AF_SLOT_STRIDE * (x - 1), 0xFFFFFFFFU);	// ADDRLO reset
		AX_BASE_WRITE_REG32(mac_base, R_MAC_ADDRESS1_HIGH + AF_SLOT_STRIDE * (x - 1), 0x0000FFFFU);	// AE = 0, SA = 0, MBC = 0
	}
	AX_BASE_WRITE_REG32(mac_base, R_MAC_HASH_TABLE_REG0, 0);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_HASH_TABLE_REG1, 0);
}
/**
 * One case: MAC_Packet_Filter + one slot + one hash bit -> send -> receive (or not) -> check -> one result line -> frame dumps.
 *
 *   Register / descriptor   Bit [pos]         Value         Function
 *   ----------------------- ----------------- ------------- ------------------------------
 *   MAC_Packet_Filter p.3118       PR [0]            0             address filter on for the whole test
 *                           HUC [1] HMC [2]   pfr           hash table for unicast / multicast
 *                           DAIF [3] PM [4]   pfr           inverse DA result / pass all multicast
 *                           DBF [5] SAF [9]   pfr           drop broadcast / drop on SA filter fail
 *                           HPF [10]          pfr           hash OR perfect
 *   MAC_Address(#i)_High p.3155      AE [31] SA [30]   slot_hr       slot x (1..3) on, compares the SA instead of the DA
 *                           MBC [29:24]       slot_hr       bytes not compared (group filter)
 *                           ADDRHI [15:0]     slot_addr[5:4]
 *   MAC_Address(#i)_Low p.3154      ADDRLO [31:0]     slot_addr[3:0]
 *   MAC_Hash_Table_Reg0/Reg1 p.3121   bit (h & 31)      hash_addr     the hash bit of hash_addr set, h = af_hash_index()
 *   RDES2 p.3068 (RX)       MADRM [26:19]     read          slot matched (HF = 0) or hash value (HF = 1)
 *                           HF [18] DAF [17] SAF [16]       hash passed / DA failed / SA failed
 *   MMC p.3164              RX_UNICAST_GOOD   read          +1 for a unicast frame, dropped or not
 *
 * @param pfr        MAC_Packet_Filter bits other than PR (PR is written 0)
 * @param slot       1..3: write slot_addr into MAC_Address(#i) with the flags slot_hr (AE / SA / MBC); AF_SLOT_NONE: no slot
 * @param hash_addr  address whose hash bit is set in MAC_Hash_Table_Reg0/Reg1, NULL: table left at 0
 * @param exp_madrm  RDES2.MADRM expected (printed only, not checked), AF_RDES2_NONE: RDES2 not printed
 * Return 1: as expected, 0: else
 */
static int af_case(uint8_t mac_idx, const char *id, uint32_t pfr, int slot, const uint8_t *slot_addr, uint32_t slot_hr,
				   const uint8_t *hash_addr, const uint8_t *da, const uint8_t *sa, int expect, int exp_madrm, const char *what)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t macpfr, hr, lr, ht;
	uint32_t rdes2 = 0, rdes3 = 0;
	uint32_t rx_good0, rx_good1;
	uint32_t wait, k;
	int h = AF_HASH_NONE;
	int len, tx_len, rx_len;
	int got, ok;

	/* 0. description, setup and expectation before anything is sent */
	eth_printf("\n  --- case %s: %s\n", id, what);
	eth_printf("      setup : MAC_Packet_Filter PR = 0%s%s%s%s%s%s%s", (pfr & MAC_PACKET_FILTER_HUC) ? " HUC" : "", (pfr & MAC_PACKET_FILTER_HMC) ? " HMC" : "",
			   (pfr & MAC_PACKET_FILTER_DAIF) ? " DAIF" : "", (pfr & MAC_PACKET_FILTER_PM) ? " PM" : "", (pfr & MAC_PACKET_FILTER_DBF) ? " DBF" : "",
			   (pfr & MAC_PACKET_FILTER_SAF) ? " SAF" : "", (pfr & MAC_PACKET_FILTER_HPF) ? " HPF" : "");
	if (slot != AF_SLOT_NONE) {
		eth_printf("; MAC_Address%d = ", slot);
		af_print_addr(slot_addr);
		eth_printf(" (AE = %d, SA = %d, MBC = 0x%02lX)", (slot_hr & MAC_ADDRESS_HIGH_AE) != 0, (slot_hr & MAC_ADDRESS_HIGH_SA) != 0,
				   (unsigned long)((slot_hr >> MAC_ADDRESS_HIGH_MBC_bit) & 0x3FU));
	} else {
		eth_printf("; MAC_Address1..3 off");
	}
	if (hash_addr != NULL) {
		h = af_hash_index(hash_addr);
		eth_printf("; hash bit of ");
		af_print_addr(hash_addr);
		eth_printf(" = index %d -> MAC_Hash_Table_Reg%d bit %d", h, h >> 5, h & 31);
	}
	eth_printf("\n      frame : DA = ");
	af_print_addr(da);
	eth_printf(", SA = ");
	af_print_addr(sa);
	if (expect == AF_PASS) {
		eth_printf("\n      Expect: PASS, %d bytes back unchanged", AF_FRAME_LEN);
		if (exp_madrm != AF_RDES2_NONE) {
			eth_printf(", RDES2.MADRM = %d if RS2V = 1 (printed, not checked)", exp_madrm);
		}
		eth_printf("\n");
	} else {
		eth_printf("\n      Expect: DROP, no RX descriptor within %d us\n", AF_DROP_WAIT_US);
	}
	/* 1. the receiver's rules: MAC_Packet_Filter (PR = 0 + the case bits, VTFE and the rest off), slots, hash table */
	af_slots_reset(mac_base);
	macpfr = pfr;														// PR [0] = 0: the address filter decides
	AX_BASE_WRITE_REG32(mac_base, R_MAC_PACKET_FILTER, macpfr);
	flow_regw("MAC_Packet_Filter", mac_base, R_MAC_PACKET_FILTER, macpfr, FF_MAC_PACKET_FILTER);
	if (slot != AF_SLOT_NONE) {
		lr = ((uint32_t)slot_addr[3] << 24) | ((uint32_t)slot_addr[2] << 16)	// ADDRLO [31:0]: bytes 3:0
		   | ((uint32_t)slot_addr[1] << 8) | slot_addr[0];
		hr = slot_hr													// AE [31], SA [30], MBC [29:24]
		   | ((uint32_t)slot_addr[5] << 8) | slot_addr[4];				// ADDRHI [15:0]: bytes 5:4
		AX_BASE_WRITE_REG32(mac_base, R_MAC_ADDRESS1_LOW + AF_SLOT_STRIDE * (slot - 1), lr);	// LR first, HR (with AE) last
		AX_BASE_WRITE_REG32(mac_base, R_MAC_ADDRESS1_HIGH + AF_SLOT_STRIDE * (slot - 1), hr);
		flow_regw("MAC_Address(#i)_Low (bytes 3:0)", mac_base, R_MAC_ADDRESS1_LOW + AF_SLOT_STRIDE * (slot - 1), lr, FF_NONE);
		flow_regw("MAC_Address(#i)_High (AE SA MBC, bytes 5:4)", mac_base, R_MAC_ADDRESS1_HIGH + AF_SLOT_STRIDE * (slot - 1), hr, FF_NONE);
	}
	if (h != AF_HASH_NONE) {
		ht = 1UL << (h & 31);
		if (h < 32) {
			AX_BASE_WRITE_REG32(mac_base, R_MAC_HASH_TABLE_REG0, ht);
			flow_regw("MAC_Hash_Table_Reg0", mac_base, R_MAC_HASH_TABLE_REG0, ht, FF_NONE);
		} else {
			AX_BASE_WRITE_REG32(mac_base, R_MAC_HASH_TABLE_REG1, ht);
			flow_regw("MAC_Hash_Table_Reg1", mac_base, R_MAC_HASH_TABLE_REG1, ht, FF_NONE);
		}
	}
	rx_good0 = AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_UNICAST_GOOD);
	/* 2. the frame, as it is */
	len = af_build(af_txbuf, da, sa);
	tx_len = egress_packet((unsigned char *)af_txbuf, len);
	if (tx_len != len) {
		eth_printf("  %s  TX error %d\n", id, tx_len);
		af_summary_add(id, 0);
		return 0;
	}
	/* 3. receive: a dropped frame never gets a descriptor (timeout, -1) */
	wait = (expect == AF_PASS) ? ETH_TEST_RX_TIMEOUT_US : AF_DROP_WAIT_US;
	rx_len = ingress_packet_status(mac_idx, af_rxbuf, sizeof(af_rxbuf), (int)wait, &rdes2, &rdes3);
	got = (rx_len >= 0);
	rx_good1 = AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_UNICAST_GOOD);
	/* 4. check: PASS = same 60 bytes back, DROP = nothing */
	if (expect == AF_PASS) {
		ok = got && (rx_len == len) && (memcmp(af_rxbuf, af_txbuf, (size_t)len) == 0);
	} else {
		ok = !got;
	}
	/* 5. column header + one line */
	eth_printf(AF_TABLE_HDR "\n");
	eth_printf("  %-3s %-6s %-6s ", id, (expect == AF_PASS) ? "PASS" : "DROP", got ? "PASS" : "DROP");
	if (got) {
		eth_printf("%3d  ", rx_len);
		for (k = 0; k < 2 * ETH_TEST_MAC_SIZE; k++) {
			eth_printf("%02X%s", af_rxbuf[k], (k == ETH_TEST_MAC_SIZE - 1) ? "   " : " ");
		}
		if (rdes3 & RDES3_RS2V) {
			eth_printf("  1    %d   %d   %d  %-5lu  ", (rdes2 & RDES2_SAF) != 0, (rdes2 & RDES2_DAF) != 0, (rdes2 & RDES2_HF) != 0,
					   (unsigned long)((rdes2 >> RDES2_MADRM_bit) & RDES2_MADRM_MASK));
		} else {
			eth_printf("  0    -   -   -  -      ");
		}
	} else {
		eth_printf("  -  (no frame)                              -    -   -   -  -      ");
	}
	eth_printf("+%-5lu | %s\n", (unsigned long)(rx_good1 - rx_good0), ok ? "OK " : "BAD");
	if (!ok) {
		if (expect == AF_PASS) {
			eth_printf("      failed: %s (RDES3 = 0x%08lX, RDES2 = 0x%08lX)\n",
					   got ? "frame back but changed" : "the frame was dropped although the filter should pass it",
					   (unsigned long)rdes3, (unsigned long)rdes2);
		} else {
			eth_printf("      failed: the frame came back although the filter should have dropped it (RDES2 = 0x%08lX)\n",
					   (unsigned long)rdes2);
		}
	}
	/* 6. both frames */
	af_frame_dump("frame sent", af_txbuf, len);
	if (got) {
		af_frame_dump("frame received", af_rxbuf, rx_len);
	}
	af_summary_add(id, ok);
	return ok;
}
/**
 * MAC address filter test through the MAC loopback (eth_run_test_ops = 11), cases A-N2. Same bits as
 * eth_mac_internal_lb() (eth_functions.c) except MAC_Packet_Filter.PR = 0, plus per case (see af_case) the other
 * MAC_Packet_Filter bits, one of MAC_Address1..3_High/Low, one bit of MAC_Hash_Table_Reg0/Reg1. Everything is put back at the end
 * (PR = 1, slots off, hash table 0).
 * Return 1: every case as expected, 0: else
 */
uint16_t eth_mac_addr_filter_lb(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t ok = 0;

	eth_printf("Start Init: Port[%d]\n", mac_idx);
	if (eth_reset_mac(mac_idx) == 0) {
		return 0;
	}
	mac_init_100m(mac_idx);
	init_mac_internal_lb(mac_idx);
	mac_start(mac_idx);
	af_summary_pos = 0;
	af_summary[0] = 0;

	eth_printf("\n================ MAC ADDRESS FILTER TEST (MAC_Packet_Filter PR = 0, MAC_Address1..3_High/Low, MAC_Hash_Table_Reg0/Reg1, RDES2 MADRM / HF) ================\n");
	eth_printf("  The receiver compares the DA (or SA) of every frame with the 4 address slots and the 64-bit hash table.\n");
	eth_printf("  expect/actual: PASS = the frame came back, DROP = none within %d us. RS2V (RDES3): 1 = RDES2 written; then\n", AF_DROP_WAIT_US);
	eth_printf("  SAF / DAF = SA / DA filter failed (RA = 1 only), HF = passed the hash filter, MADRM = slot that matched (HF = 0)\n");
	eth_printf("  or the hash value (HF = 1) (p.3068). MMC rx = Rx_Unicast_Packets_Good: +1 for a unicast frame even when dropped,\n");
	eth_printf("  0 for broadcast / multicast. Each case: description, setup, expectation -> [FLOW lines] -> result line -> frames.\n");
	eth_printf("  Case A prints the [10]/[11] FLOW lines.\n");

	/* A-D: MAC_Address0 and broadcast, nothing else configured */
	eth_printf("=======================================================================================\n");
	eth_flow_trace = 1;
	ok += af_case(mac_idx, "A",  0, AF_SLOT_NONE, NULL, 0, NULL, AF_MACA0, AF_UNI_X, AF_PASS, 0,
				  "PR = 0, nothing else: DA = MAC_Address0 (the board address) passes, MAC_Address0 is always enabled");
	eth_flow_trace = 0;
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "B",  0, AF_SLOT_NONE, NULL, 0, NULL, AF_UNI_X, AF_MACA0, AF_DROP, AF_RDES2_NONE,
				  "DA = 02:00:00:00:00:99, no slot holds it: dropped (this is what PR = 1 hid in every other test)");
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "C",  0, AF_SLOT_NONE, NULL, 0, NULL, AF_BCAST, AF_UNI_X, AF_PASS, AF_RDES2_NONE,
				  "DA = broadcast: passes, broadcast is never filtered while DBF = 0");
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "D",  MAC_PACKET_FILTER_DBF, AF_SLOT_NONE, NULL, 0, NULL, AF_BCAST, AF_UNI_X, AF_DROP, AF_RDES2_NONE,
				  "DBF = 1, DA = broadcast: dropped");
	/* E-H: a second unicast address in MAC_Address1 */
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "E",  0, 1, AF_A1, MAC_ADDRESS_HIGH_AE, NULL, AF_A1, AF_UNI_X, AF_PASS, 1,
				  "MAC_Address1 = 02:11:22:33:44:55 enabled, DA = that: passes through slot 1");
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "F",  0, 1, AF_A1, MAC_ADDRESS_HIGH_AE, NULL, AF_A1_NEAR, AF_UNI_X, AF_DROP, AF_RDES2_NONE,
				  "same slot, DA = 02:11:22:33:44:56: one byte off, dropped (48-bit compare)");
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "G",  0, 1, AF_A1, MAC_ADDRESS_HIGH_AE | AF_MBC_OUI, NULL, AF_A1_GRP, AF_UNI_X, AF_PASS, 1,
				  "MAC_Address1 with MBC masking bytes 3..5, DA = 02:11:22:AA:BB:CC: passes, only the first 3 bytes are compared");
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "H",  0, 1, AF_A1, 0, NULL, AF_A1, AF_UNI_X, AF_DROP, AF_RDES2_NONE,
				  "MAC_Address1 holds 02:11:22:33:44:55 but AE = 0, DA = that: dropped, the slot is off");
	/* I-J: multicast */
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "I",  0, 2, AF_MC_PTP, MAC_ADDRESS_HIGH_AE, NULL, AF_MC_PTP, AF_UNI_X, AF_PASS, 2,
				  "MAC_Address2 = 01:1B:19:00:00:00 (PTP multicast), PM = 0, HMC = 0, DA = that: multicast through a perfect slot");
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "J1", MAC_PACKET_FILTER_PM, AF_SLOT_NONE, NULL, 0, NULL, AF_MC_X, AF_UNI_X, AF_PASS, AF_RDES2_NONE,
				  "PM = 1, DA = 01:00:5E:01:02:03: every multicast passes");
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "J2", 0, AF_SLOT_NONE, NULL, 0, NULL, AF_MC_X, AF_UNI_X, AF_DROP, AF_RDES2_NONE,
				  "PM = 0, same multicast DA, no slot: dropped");
	/* K: the hash table for unicast, with and without the perfect slots */
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "K1", MAC_PACKET_FILTER_HUC, AF_SLOT_NONE, NULL, 0, AF_UNI_X, AF_UNI_X, AF_MACA0, AF_PASS, af_hash_index(AF_UNI_X),
				  "HUC = 1, hash bit of 02:00:00:00:00:99 set, DA = that: passes through the hash table (mode 1 E1 board 10-16: HF = 1, MADRM = index)");
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "K2", MAC_PACKET_FILTER_HUC, AF_SLOT_NONE, NULL, 0, AF_UNI_X, AF_MACA0, AF_UNI_X, AF_DROP, AF_RDES2_NONE,
				  "HUC = 1, HPF = 0, DA = MAC_Address0: dropped, in hash mode the perfect slots are not looked at (p.3119)");
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "K3", MAC_PACKET_FILTER_HUC | MAC_PACKET_FILTER_HPF, AF_SLOT_NONE, NULL, 0, AF_UNI_X, AF_MACA0, AF_UNI_X, AF_PASS, 0,
				  "HUC = 1, HPF = 1, DA = MAC_Address0: passes, hash OR perfect");
	/* L-M: the source address filter in MAC_Address3 */
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "L",  MAC_PACKET_FILTER_SAF, 3, AF_SA_OK, MAC_ADDRESS_HIGH_AE | MAC_ADDRESS_HIGH_SA, NULL, AF_MACA0, AF_SA_OK, AF_PASS, 0,
				  "MAC_Address3 = 02:AA:BB:CC:DD:EE with SA = 1, SAF = 1, frame SA = that: passes both filters");
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "M",  MAC_PACKET_FILTER_SAF, 3, AF_SA_OK, MAC_ADDRESS_HIGH_AE | MAC_ADDRESS_HIGH_SA, NULL, AF_MACA0, AF_SA_X, AF_DROP, AF_RDES2_NONE,
				  "same, frame SA = 02:AA:BB:CC:DD:EF: DA passes but the SA filter fails, SAF = 1 drops it");
	/* N: inverse DA filter */
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "N1", MAC_PACKET_FILTER_DAIF, AF_SLOT_NONE, NULL, 0, NULL, AF_MACA0, AF_UNI_X, AF_DROP, AF_RDES2_NONE,
				  "DAIF = 1, DA = MAC_Address0: the match is inverted, dropped");
	eth_printf("=======================================================================================\n");
	ok += af_case(mac_idx, "N2", MAC_PACKET_FILTER_DAIF, AF_SLOT_NONE, NULL, 0, NULL, AF_UNI_X, AF_MACA0, AF_PASS, AF_RDES2_NONE,
				  "DAIF = 1, DA = 02:00:00:00:00:99: no match, inverted to pass");

	/* back to the state the other tests expect: promiscuous, slots off, hash table empty */
	af_slots_reset(mac_base);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_PACKET_FILTER, MAC_PACKET_FILTER_PR);

	eth_printf("\nCases: %s\n", af_summary);
	eth_printf("Result: %lu/18 cases as expected.\n", (unsigned long)ok);
	eth_print_mac_counters(mac_idx);
	return (ok == 18) ? 1 : 0;
}
