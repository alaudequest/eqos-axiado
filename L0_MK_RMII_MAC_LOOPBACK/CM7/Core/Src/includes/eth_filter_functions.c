#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_filter_utils.h"
#include "eth_printf.h"
#include "eth_flow_utils.h"

/*
 * MAC address filter test (eth_run_test_ops = 1), through the MAC loopback.
 * Each case: set the filter (MAC_Packet_Filter, hash table, MAC_Address1) -> send 1 frame -> does it come back to RX?
 * The result is compared with the "expect" column; to try your own case, add a line to CASES[].
 *
 * RM0399: MAC_Packet_Filter p.3118-3120; DA/SA filter tables (Table 549/550) p.2956-2957;
 * MAC_Hash_Table_Reg0/Reg1 and the hash p.3121-3122; MAC_Address(#i)_High/Low p.3153-3155;
 * RDES2 write-back (HF, MADRM, DAF, SAF) p.3067-3068.
 */

#define FT_FRAME_LEN	22		// frame sent: 14-byte header + "FILT" + case number + 3 bytes 0
#define FT_RX_KEEP		64		// bytes of the RX frame kept for the dump (the frame comes back as 60 bytes)
#define FT_RX_WAIT_US	5000	// a frame dropped by the filter never comes back: wait 5 ms
#define MBC_BYTE(k)		(1U << (MAC_ADDRESS_HIGH_MBC_bit + (k)))	// MACAxHR.MBC: ignore address byte k (0 = first)

/* Addresses used by the cases */
static const uint8_t BOARD[6]    = {0x00, 0x80, 0xE1, 0x00, 0x00, 0x00}; // MAC_Address0 = board address
static const uint8_t BOARD_1[6]  = {0x00, 0x80, 0xE1, 0x00, 0x00, 0x01}; // board address, last byte differs
static const uint8_t PEER[6]     = {0x6C, 0x1F, 0xF7, 0xCA, 0x2B, 0xFD}; // other unicast (DA of the traffic test)
static const uint8_t BCAST[6]    = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // broadcast
static const uint8_t MDNS[6]     = {0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB}; // multicast mDNS (224.0.0.251)
static const uint8_t ALLHOST[6]  = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01}; // multicast all-hosts (224.0.0.1)
static const uint8_t ALLRTR[6]   = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x02}; // multicast all-routers (224.0.0.2)
static const uint8_t LEAK32[6]   = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x18}; // same hash value as ALLHOST
static const uint8_t LEAK48[6]   = {0x01, 0x00, 0x5E, 0x00, 0x00, 0xC9}; // same hash value as MDNS
static const uint8_t IPV6ALL[6]  = {0x33, 0x33, 0x00, 0x00, 0x00, 0x01}; // multicast IPv6 all-nodes
static const uint8_t OTHER_SA[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x99}; // other source address (SA filter)

/* One case: filter setup + frame sent + expected result */
struct filter_case {
	const char    *id;        // short name
	const char    *what;      // meaning
	uint32_t       pfr;       // value written to MAC_Packet_Filter
	const uint8_t *hash[3];   // addresses put in the hash table (NULL = end)
	const uint8_t *a1;        // address for MAC_Address1 (NULL = MAC_Address1 off)
	uint32_t       a1_flags;  // MAC_ADDRESS_HIGH_SA, MBC_BYTE(k) for MAC_Address1 (AE is added)
	const uint8_t *da;        // DA of the frame
	const uint8_t *sa;        // SA of the frame (NULL = BOARD)
	int            expect;    // 1 = PASS (frame received), 0 = DROP
};

static const struct filter_case CASES[] = {
	/* A. PR = 1: no address filter */
	{"A1", "PR=1: other DA still passes",              MAC_PACKET_FILTER_PR,   {0},             NULL,  0,           PEER,    NULL,     1},
	/* B. Perfect (HUC = 0): full 48-bit compare with MAC_Address0..3 */
	{"B1", "perfect: DA = board (MAC_Address0)",              0,           {0},             NULL,  0,           BOARD,   NULL,     1},
	{"B2", "perfect: other DA",                        0,           {0},             NULL,  0,           PEER,    NULL,     0},
	{"B3", "perfect: only the last byte differs",      0,           {0},             NULL,  0,           BOARD_1, NULL,     0},
	{"B4", "perfect: PEER added to MAC_Address1",             0,           {0},             PEER,  0,           PEER,    NULL,     1},
	{"B5", "group: MAC_Address1 = board, last byte masked",   0,           {0},             BOARD, MBC_BYTE(5), BOARD_1, NULL,     1},
	/* C. Broadcast */
	{"C1", "broadcast, DBF=0",                         0,           {0},             NULL,  0,           BCAST,   NULL,     1},
	{"C2", "broadcast, DBF=1",                         MAC_PACKET_FILTER_DBF,  {0},             NULL,  0,           BCAST,   NULL,     0},
	/* D. Multicast */
	{"D1", "multicast perfect: not in MAC_Address(#i)",          0,           {0},             NULL,  0,           MDNS,    NULL,     0},
	{"D2", "PM=1: every multicast passes",             MAC_PACKET_FILTER_PM,   {0},             NULL,  0,           MDNS,    NULL,     1},
	{"D3", "hash {mDNS, all-hosts}: send mDNS",        MAC_PACKET_FILTER_HMC,  {MDNS, ALLHOST}, NULL,  0,           MDNS,    NULL,     1},
	{"D4", "hash {mDNS, all-hosts}: send all-hosts",   MAC_PACKET_FILTER_HMC,  {MDNS, ALLHOST}, NULL,  0,           ALLHOST, NULL,     1},
	{"D5", "hash {mDNS, all-hosts}: send all-routers", MAC_PACKET_FILTER_HMC,  {MDNS, ALLHOST}, NULL,  0,           ALLRTR,  NULL,     0},
	{"D6", "hash: ..:00:18 LEAKS (= all-hosts hash)",  MAC_PACKET_FILTER_HMC,  {MDNS, ALLHOST}, NULL,  0,           LEAK32,  NULL,     1},
	{"D7", "hash: ..:00:C9 LEAKS (= mDNS hash)",       MAC_PACKET_FILTER_HMC,  {MDNS, ALLHOST}, NULL,  0,           LEAK48,  NULL,     1},
	{"D8", "hash: send IPv6 33:33:00:00:00:01",        MAC_PACKET_FILTER_HMC,  {MDNS, ALLHOST}, NULL,  0,           IPV6ALL, NULL,     0},
	/* E. Unicast hash, and HPF */
	{"E1", "HUC=1, hash {PEER}: send PEER",            MAC_PACKET_FILTER_HUC,  {PEER},          NULL,  0,           PEER,    NULL,     1},
	{"E2", "HUC=1, HPF=0: send board (hash only)",     MAC_PACKET_FILTER_HUC,  {PEER},          NULL,  0,           BOARD,   NULL,     0},
	{"E3", "HUC=1, HPF=1: send board (hash OR perf)",  MAC_PACKET_FILTER_HUC | MAC_PACKET_FILTER_HPF, {PEER}, NULL, 0,         BOARD,   NULL,     1},
	/* F. DA inverse filtering */
	{"F1", "DAIF=1: DA = board -> dropped",            MAC_PACKET_FILTER_DAIF, {0},             NULL,  0,           BOARD,   NULL,     0},
	{"F2", "DAIF=1: other DA -> passes",               MAC_PACKET_FILTER_DAIF, {0},             NULL,  0,           PEER,    NULL,     1},
	/* G. Receive All: everything passes, RDES2.DAF tells the frame should have been dropped */
	{"G1", "RA=1: other DA passes, DAF=1",             MAC_PACKET_FILTER_RA,   {0},             NULL,  0,           PEER,    NULL,     1},
	/* H. Source address filter: MAC_Address1 used as SA address */
	{"H1", "SAF=1, MAC_Address1(SA)=board: SA = board",       MAC_PACKET_FILTER_SAF,  {0},             BOARD, MAC_ADDRESS_HIGH_SA,   BOARD,   BOARD,    1},
	{"H2", "SAF=1, MAC_Address1(SA)=board: other SA",         MAC_PACKET_FILTER_SAF,  {0},             BOARD, MAC_ADDRESS_HIGH_SA,   BOARD,   OTHER_SA, 0},
};
#define N_CASES (sizeof CASES / sizeof CASES[0])

/* Result of one case */
struct case_result {
	int      tx;                 // egress_packet() return value
	int      got;                // 1 = frame came back to RX
	uint32_t ht[2];              // MAC_Hash_Table_Reg0, Reg1 written
	uint32_t rdes2;              // write-back RDES2 of the received frame
	uint32_t rdes3;              // write-back RDES3 of the received frame
	uint8_t  txf[FT_FRAME_LEN];  // frame sent (no pad/FCS)
	uint32_t rx_len;             // length of the received frame (RDES3.PL: pad kept, no FCS)
	uint8_t  rxf[FT_RX_KEEP];    // first FT_RX_KEEP bytes of the received frame
};

/* CRC-32 IEEE 802.3 (the Ethernet FCS) */
static uint32_t crc32_ieee(const uint8_t *p, uint32_t n)
{
	uint32_t c = 0xFFFFFFFFU;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; ++k)
			c = (c >> 1) ^ (0xEDB88320U & (0U - (c & 1U)));
	}
	return ~c;
}
/* Reverse the 32 bits (Cortex-M7 RBIT instruction) */
static uint32_t bit_reverse32(uint32_t v)
{
	uint32_t r = 0;
	for (int i = 0; i < 32; i++) {
		r = (r << 1) | (v & 1U);
		v >>= 1;
	}
	return r;
}
/* Hash value 0..63 of an address (p.3121): (1) CRC-32 of the 6 bytes, (2) reverse the 32 bits, (3) take the 6 upper bits */
static uint32_t hash_index(const uint8_t mac[6])
{
	return bit_reverse32(crc32_ieee(mac, 6U)) >> 26;
}
static void print_mac(const uint8_t *m)
{
	eth_printf("%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}
/* Write a filter register, then wait 1 us: the filter runs in the RX (RMII) clock domain,
 * so consecutive writes are kept a few clocks apart */
static void write_filter_reg(uint32_t mac_base, uint32_t off, uint32_t v)
{
	AX_BASE_WRITE_REG32(mac_base, off, v);
	udelay(1);
}
/**
 * Whole filter of one case: hash table, MAC_Address1, then MAC_Packet_Filter
 *
 *   Register               Bit [pos]       Value      Function
 *   ---------------------- --------------- ---------- ------------------------------
 *   MAC_Hash_Table_Reg0 p.3121     HT31T0 [31:0]   ht[0]      one bit per hash value 0..31
 *   MAC_Hash_Table_Reg1 p.3122     HT63T32 [31:0]  ht[1]      one bit per hash value 32..63
 *   MAC_Address1_High p.3155     AE [31]         1 / 0      MAC_Address1 used / not used (c->a1 NULL)
 *                          SA [30]         a1_flags   1: compare with SA, 0: with DA
 *                          MBC [29:24]     a1_flags   bit k = 1: ignore address byte k
 *                          ADDRHI [15:0]   a1[5:4]    address byte 5:4
 *   MAC_Address1_Low p.3154     ADDRLO [31:0]   a1[3:0]    address byte 3:0
 *   MAC_Packet_Filter p.3118      PR [0]          c->pfr     promiscuous: no address filter
 *                          HUC [1]                    unicast DA: hash table instead of perfect
 *                          HMC [2]                    multicast DA: hash table instead of perfect
 *                          DAIF [3]                   DA inverse: drop the frames that match
 *                          PM [4]                     pass every multicast frame
 *                          DBF [5]                    drop broadcast frames
 *                          SAF [9]                    drop frames whose SA does not match
 *                          HPF [10]                   with HUC/HMC: hash OR perfect match passes
 *                          RA [31]                    receive all (RDES2 still tells DAF/SAF)
 */
static void apply_filter(uint8_t mac_idx, const struct filter_case *c, uint32_t ht[2])
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);

	// Hash table: 1 bit per address. Bit 5 of the hash selects MAC_Hash_Table_Reg0/Reg1, the 5 low bits the bit
	ht[0] = 0U;
	ht[1] = 0U;
	for (uint32_t i = 0; i < 3U && c->hash[i] != NULL; ++i) {
		uint32_t h = hash_index(c->hash[i]);
		ht[h >> 5] |= 1U << (h & 31U);
	}
	write_filter_reg(mac_base, R_MAC_HASH_TABLE_REG0, ht[0]);
	write_filter_reg(mac_base, R_MAC_HASH_TABLE_REG1, ht[1]);

	// MAC_Address1: same byte order as MAC_Address0, HR = byte 5:4, LR = byte 3:0
	if (c->a1 != NULL) {
		write_filter_reg(mac_base, R_MAC_ADDRESS1_HIGH, MAC_ADDRESS_HIGH_AE | c->a1_flags | ((uint32_t)c->a1[5] << 8) | c->a1[4]);
		write_filter_reg(mac_base, R_MAC_ADDRESS1_LOW, ((uint32_t)c->a1[3] << 24) | ((uint32_t)c->a1[2] << 16) |
											  ((uint32_t)c->a1[1] << 8) | c->a1[0]);
	} else {
		write_filter_reg(mac_base, R_MAC_ADDRESS1_HIGH, 0x0000FFFFU);   // reset value: AE = 0 -> MAC_Address1 not used
		write_filter_reg(mac_base, R_MAC_ADDRESS1_LOW, 0xFFFFFFFFU);
	}

	write_filter_reg(mac_base, R_MAC_PACKET_FILTER, c->pfr);
}
/* One case: set the filter, send 1 frame, wait up to FT_RX_WAIT_US for it */
static void run_case(uint8_t mac_idx, uint8_t n, const struct filter_case *c, struct case_result *r)
{
	uint8_t *frame = r->txf;   // frame kept in r for the dump at the end
	unsigned char rxbuf[FT_RX_KEEP];
	uint32_t des2, des3;
	int len;

	eth_rx_flush(mac_idx);
	apply_filter(mac_idx, c, r->ht);

	// Frame: DA | SA | EtherType 0x88B5 | "FILT" + case number (to find our own frame)
	memcpy(frame, c->da, 6);
	memcpy(frame + 6, (c->sa != NULL) ? c->sa : BOARD, 6);
	frame[12] = 0x88;
	frame[13] = 0xB5;
	memcpy(frame + 14, "FILT", 4);
	frame[18] = n;
	frame[19] = frame[20] = frame[21] = 0U;

	r->got = 0;
	r->rdes2 = 0U;
	r->rdes3 = 0U;
	r->rx_len = 0U;
	r->tx = egress_packet(frame, FT_FRAME_LEN);
	if (r->tx != FT_FRAME_LEN) {
		return;
	}

	for (uint32_t tries = 0; tries <= MAC_100M_RX_DESC_CNT && !r->got; ++tries) {
		len = ingress_packet_status(mac_idx, rxbuf, sizeof(rxbuf), FT_RX_WAIT_US, &des2, &des3);
		if (len == -1) {
			break;   // nothing came back: dropped by the filter
		}
		if (len >= 19 && memcmp(rxbuf + 14, "FILT", 4) == 0 && rxbuf[18] == n) {
			r->got = 1;
			r->rdes2 = des2;
			r->rdes3 = des3;
			r->rx_len = (uint32_t)len;
			memcpy(r->rxf, rxbuf, (r->rx_len < FT_RX_KEEP) ? r->rx_len : FT_RX_KEEP);
		}
	}
}
/* n bytes in hex, 16 per line; the next lines are indented */
static void print_hex(const uint8_t *p, uint32_t n, int indent)
{
	for (uint32_t i = 0; i < n; ++i) {
		if (i != 0U && (i % 16U) == 0U)
			eth_printf("\n%*s", indent, "");
		eth_printf("%02X ", (unsigned int)p[i]);
	}
	eth_printf("\n");
}
/* End of the test: frame sent and frame received of each case */
static void print_packets(const struct case_result res[])
{
	eth_printf("\n================ PACKETS: TX sent / RX received ================\n");
	eth_printf("TX = frame given to egress_packet() (no pad/FCS).\n");
	eth_printf("RX = rx_buf written by the DMA: the MAC pads to 60 bytes, FCS removed (CST=1).\n");
	for (uint32_t i = 0; i < N_CASES; ++i) {
		const struct filter_case *c = &CASES[i];
		const struct case_result *r = &res[i];

		eth_printf("\n%-3s %s\n", c->id, c->what);
		eth_printf("  TX %2lu bytes: ", (unsigned long)FT_FRAME_LEN);
		print_hex(r->txf, FT_FRAME_LEN, 15);
		if (r->tx != FT_FRAME_LEN) {
			eth_printf("  RX: none (egress_packet() error %d)\n", r->tx);
		} else if (!r->got) {
			eth_printf("  RX: none (dropped by the filter)\n");
		} else {
			uint32_t shown = (r->rx_len < FT_RX_KEEP) ? r->rx_len : FT_RX_KEEP;
			eth_printf("  RX %2lu bytes: ", (unsigned long)r->rx_len);
			print_hex(r->rxf, shown, 15);
			eth_printf("  first %lu bytes of RX %s TX\n", (unsigned long)FT_FRAME_LEN,
					   (r->rx_len >= FT_FRAME_LEN && memcmp(r->rxf, r->txf, FT_FRAME_LEN) == 0) ? "SAME AS" : "DIFFER FROM");
		}
	}
}
/* Run every case of CASES[], print one line per case. Return the number of cases as expected */
static uint32_t filter_run_all(uint8_t mac_idx)
{
	static struct case_result res[N_CASES];   // kept for the packet dump at the end
	static const struct filter_case reset_filter = {"--", "", MAC_PACKET_FILTER_PR, {0}, NULL, 0, BOARD, NULL, 1};
	uint32_t ok = 0;
	uint32_t ht[2];

	eth_printf("\n================ MAC ADDRESS FILTER (MAC_Packet_Filter) ================\n");
	eth_printf("Board: MAC_Address0 = ");
	print_mac(BOARD);
	eth_printf(". Each line: set the filter -> send 1 frame through the MAC loopback -> back to RX?\n");
	eth_printf("hashDA   = hash value of DA computed by software (p.3121), bit 5 selects MAC_Hash_Table_Reg1/Reg0.\n");
	eth_printf("HF/MADRM = RDES2 written by the HARDWARE: HF=1 -> MADRM is the hardware hash (must = hashDA);\n");
	eth_printf("           HF=0 -> MADRM is the matching MAC_Address(#i) number. DAF/SAF = 1: DA/SA filter failed.\n\n");
	eth_printf("ID  DA sent            Pkt_Filter HT_Reg1  HT_Reg0  hashDA | expect  actual  | HF MADRM DAF SAF | result\n");

	for (uint32_t i = 0; i < N_CASES; ++i) {
		const struct filter_case *c = &CASES[i];
		struct case_result r;
		run_case(mac_idx, (uint8_t)(i + 1U), c, &r);

		uint32_t h = hash_index(c->da);
		int pass = r.got;
		int match = (r.tx == FT_FRAME_LEN) && (pass == c->expect);
		ok += (uint32_t)match;

		eth_printf("%-3s ", c->id);
		print_mac(c->da);
		eth_printf("  0x%08lX %08lX %08lX %2lu     | %-7s %-7s | ", (unsigned long)c->pfr,
				   (unsigned long)r.ht[1], (unsigned long)r.ht[0], (unsigned long)h,
				   c->expect ? "PASS" : "DROP", (r.tx != FT_FRAME_LEN) ? "TX ERR" : (pass ? "PASS" : "DROP"));
		if (pass && (r.rdes3 & RDES3_RS2V) != 0U) {
			uint32_t hf = (r.rdes2 & RDES2_HF) ? 1U : 0U;
			uint32_t madrm = (r.rdes2 >> RDES2_MADRM_bit) & RDES2_MADRM_MASK;
			eth_printf("%2lu %5lu %3lu %3lu | %s", (unsigned long)hf, (unsigned long)madrm,
					   (unsigned long)((r.rdes2 & RDES2_DAF) ? 1U : 0U), (unsigned long)((r.rdes2 & RDES2_SAF) ? 1U : 0U),
					   match ? "OK " : "BAD");
			if (hf != 0U)
				eth_printf("%s", (madrm == h) ? " (hash match)" : " (hash DIFFERS!)");
		} else {
			eth_printf(" -     -   -   - | %s", match ? "OK " : "BAD");
		}
		eth_printf("  %s\n", c->what);
		res[i] = r;
	}

	eth_printf("\nResult: %lu/%lu lines as expected.\n", (unsigned long)ok, (unsigned long)N_CASES);
	if (ok != N_CASES)
		eth_printf("BAD line: either the expected value is wrong (see Table 549/550, p.2956-2957) or the hardware differs from RM0399.\n");
	if (FLOW_LOG >= 1)
		print_packets(res);

	// Filter back to a simple state: PR = 1, empty hash table, MAC_Address1 off
	apply_filter(mac_idx, &reset_filter, ht);
	return ok;
}
/**
 * MAC address filter test through the MAC loopback (eth_run_test_ops = 1)
 * Init MAC, enable the loopback, start DMA and MAC, run CASES[]
 * Same bits as eth_mac_internal_lb() (eth_functions.c); then for each case apply_filter() writes
 * MAC_Hash_Table_Reg0/Reg1, MAC_Address1_High/LR and MAC_Packet_Filter (see the note on top of apply_filter).
 * Return 1: every case as expected, 0: else
 */
uint16_t eth_mac_filter_lb(uint8_t mac_idx)
{
	uint32_t ok;
	eth_printf("Start Init: Port[%d]\n", mac_idx);

	if (eth_reset_mac(mac_idx) == 0) {
		return 0;
	}
	mac_init_100m(mac_idx);
	init_mac_internal_lb(mac_idx);
	mac_start(mac_idx);

	ok = filter_run_all(mac_idx);
	eth_print_mac_counters(mac_idx);
	return (ok == N_CASES) ? 1 : 0;
}
