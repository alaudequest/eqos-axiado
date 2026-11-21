#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_coe_utils.h"
#include "eth_traffic_test.h"
#include "eth_printf.h"
#include "eth_flow_utils.h"

/*
 * Checksum offload test (eth_run_test_ops = 7), through the MAC loopback: the Checksum Offload Engine
 * (COE, RM0399 §61.5.5 p.2985-2990) on both sides of the MAC.
 *
 *   TX side: TDES3.CIC [17:16] (p.3052) tells the MAC, frame by frame, which checksums to compute and
 *            write into the frame before it goes out:
 *              00  none: the frame goes out as it is
 *              01  IPv4 header checksum only
 *              10  IPv4 header + TCP/UDP checksum; the CPU puts the pseudo-header sum in the checksum field
 *              11  IPv4 header + TCP/UDP checksum, pseudo header computed by the MAC too (what TSO forces)
 *            The write-back TDES3 (p.3055) reports IHE [0] (IP header error) and PCE [12] (payload
 *            checksum error) when the MAC could not do the job.
 *   RX side: MAC_Configuration.IPC [27] (p.3112) turns the receive COE on. For every IPv4/IPv6 frame the MAC
 *            recomputes both checksums and writes the result in RDES1 (p.3066, valid when RDES3.RS1V = 1):
 *              IPV4 [4]  IPv4 header found         PT [2:0]  payload type: 1 UDP, 2 TCP, 3 ICMP, 0 not processed
 *              IPHE [3]  IP header checksum wrong   IPCE [7]  TCP/UDP checksum wrong   IPCB [6]  COE bypassed
 *            The frame is still delivered: init_mac_internal_lb() sets MTL_RxQ0_Operation_Mode.DIS_TCP_EF = 1
 *            (p.3106); without it the MTL drops the frames with a checksum error.
 *
 * Every case sends the same TCP/IP (or UDP/IP) frame of 100 payload bytes; only the two checksum fields
 * and TDES3.CIC change. The software then verifies both checksums of the frame that came back (RFC 1071),
 * so every case has two independent witnesses: the bytes on the wire and the RDES1 bits.
 *   A  CIC 00, IP checksum 0, TCP correct   -> wire unchanged: IPHE = 1; PT = 2 and IPCE = 0 (payload still checked)
 *   B  CIC 00, IP correct, TCP 0            -> IPHE = 0, IPCE = 1, PT = 2
 *   C  CIC 00, both correct (CPU work)      -> IPHE = 0, IPCE = 0: reference, software and MAC agree
 *   D  CIC 01, both 0                       -> the MAC fixes the IP header only: IPHE = 0, IPCE = 1
 *   E  CIC 11, both 0                       -> the MAC fixes both: IPHE = 0, IPCE = 0, both verify
 *   F  CIC 10, IP 0, TCP = pseudo-header sum -> the MAC adds header + payload: both verify
 *   G  CIC 11, UDP, both 0                  -> as E with UDP: PT = 1
 *   H  CIC 00, both 0                       -> IPHE = 1 and IPCE = 1 together
 * Board result 2026-11-08 (case A): with only the IP checksum wrong the RX COE still reports PT = 2 and
 * checks the TCP checksum. The RM0399 p.3067 sentence "PT = 0 when the COE does not process the payload
 * because of an IP header error" covers the structural errors of p.2986 (version, header length, frame
 * shorter than the IP length), not a checksum mismatch.
 * Limit (p.2986): the TX FIFO stores the whole frame before sending (store-and-forward), so a frame with
 * CIC != 0 must be < TQS - (PBL + 7) x 4 = 2048 - 60 bytes. 154 bytes here.
 *
 * Byte offsets in the frame (big-endian fields):
 *    0..5  DA        6..11 SA       12..13 EtherType 0x0800
 *   14     IP ver/IHL 0x45   15 TOS   16..17 total length   18..19 ID   20..21 flags/frag   22 TTL
 *   23     protocol 6 TCP / 17 UDP    24..25 IP checksum    26..29 source IP    30..33 destination IP
 *   TCP:  34..35 source port  36..37 dest port  38..41 seq  42..45 ack  46 offset 0x50  47 flags
 *         48..49 window  50..51 TCP checksum  52..53 urgent  54.. payload
 *   UDP:  34..35 source port  36..37 dest port  38..39 UDP length  40..41 UDP checksum  42.. payload
 */

#define CO_PAYLOAD		100
#define CO_TCP_HDR		54		// Ethernet 14 + IPv4 20 + TCP 20
#define CO_UDP_HDR		42		// Ethernet 14 + IPv4 20 + UDP 8
#define CO_PROTO_TCP	6		// IP protocol number (byte 23)
#define CO_PROTO_UDP	17
#define CO_IP_ID		0x4321
#define CO_SEND_ZERO	0		// checksum field sent as 0: wrong on the wire unless the MAC fixes it
#define CO_SEND_OK		1		// checksum computed by the CPU
#define CO_SEND_PSEUDO	2		// L4 field = pseudo-header sum only, what CIC = 10 expects (p.2987)
#define CO_FRAME_MAX	(CO_TCP_HDR + CO_PAYLOAD)
/* Column header of the result table; co_case() prints its line with the same widths */
#define CO_TABLE_HDR	"  id  CIC  IP.sent L4.sent | ip  l4  | IPV4 PT IPHE IPCE IPCB | IHE PCE | result"

static uint8_t co_frame[CO_FRAME_MAX];		// the frame sent (egress_packet copies it to tx_buf)
static uint8_t co_rxbuf[MAC_100M_BUF_SIZE];	// the frame received

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
/* pseudo header of TCP/UDP (RFC 793 / 768): source IP, destination IP, 0, protocol, L4 length. Not sent
 * on the wire, only added into the L4 checksum, so a frame delivered to the wrong host fails the check */
static uint32_t csum_pseudo(const uint8_t *f, int proto, int l4_len)
{
	uint8_t pseudo[12];

	memcpy(pseudo, f + 26, 8);				// source IP, destination IP
	pseudo[8] = 0;
	pseudo[9] = (uint8_t)proto;
	pseudo[10] = (l4_len >> 8) & 0xFF;
	pseudo[11] = l4_len & 0xFF;
	return csum_add(0, pseudo, 12);
}
/* 1: the IPv4 header checksum of frame f verifies (bytes 14..33) */
static int ip_csum_ok(const uint8_t *f)
{
	return csum_fold(csum_add(0, f + 14, 20)) == 0xFFFF;
}
/* 1: the TCP/UDP checksum verifies: pseudo header + L4 header + payload (bytes 34..34+l4_len) */
static int l4_csum_ok(const uint8_t *f, int proto, int l4_len)
{
	return csum_fold(csum_add(csum_pseudo(f, proto, l4_len), f + 34, l4_len)) == 0xFFFF;
}
/* Build co_frame: DA | SA | 0x0800 | IPv4 | TCP or UDP | payload bytes 0, 1, 2, ... Both checksums 0.
 * Return the frame length (154 TCP, 142 UDP) */
static int co_build(int proto)
{
	unsigned char da[ETH_TEST_MAC_SIZE] = ETH_TEST_DEST_MAC;
	unsigned char sa[ETH_TEST_MAC_SIZE] = ETH_TEST_SRC_MAC;
	int hdr = (proto == CO_PROTO_TCP) ? CO_TCP_HDR : CO_UDP_HDR;
	int ip_len = hdr - 14 + CO_PAYLOAD;			// IP total length: IP header + L4 header + payload
	int i;

	memcpy(co_frame, da, ETH_TEST_MAC_SIZE);
	memcpy(co_frame + ETH_TEST_MAC_SIZE, sa, ETH_TEST_MAC_SIZE);
	co_frame[12] = 0x08;						// EtherType 0x0800 = IPv4: the COE only looks at IPv4/IPv6 frames
	co_frame[13] = 0x00;
	/* IPv4 header */
	co_frame[14] = 0x45;						// version 4, header 5 words
	co_frame[15] = 0x00;						// TOS
	co_frame[16] = (ip_len >> 8) & 0xFF;		// total length
	co_frame[17] = ip_len & 0xFF;
	co_frame[18] = (CO_IP_ID >> 8) & 0xFF;		// identification
	co_frame[19] = CO_IP_ID & 0xFF;
	co_frame[20] = 0x40;						// flags DF, fragment offset 0 (a fragment is not checked, Table 566)
	co_frame[21] = 0x00;
	co_frame[22] = 64;							// TTL
	co_frame[23] = (uint8_t)proto;				// protocol: 6 TCP, 17 UDP
	co_frame[24] = 0x00;						// header checksum: set by co_set_csums()
	co_frame[25] = 0x00;
	co_frame[26] = 192;							// source 192.168.1.10
	co_frame[27] = 168;
	co_frame[28] = 1;
	co_frame[29] = 10;
	co_frame[30] = 192;							// destination 192.168.1.20
	co_frame[31] = 168;
	co_frame[32] = 1;
	co_frame[33] = 20;
	/* L4 header */
	co_frame[34] = 0x9C;						// source port 40000
	co_frame[35] = 0x40;
	co_frame[36] = 0x00;						// destination port 80
	co_frame[37] = 0x50;
	if (proto == CO_PROTO_TCP) {
		co_frame[38] = 0x10;					// sequence number 0x10000000
		co_frame[39] = 0x00;
		co_frame[40] = 0x00;
		co_frame[41] = 0x00;
		co_frame[42] = 0x00;					// ack number 0
		co_frame[43] = 0x00;
		co_frame[44] = 0x00;
		co_frame[45] = 0x00;
		co_frame[46] = 0x50;					// data offset 5 words, no options
		co_frame[47] = 0x18;					// flags PSH | ACK
		co_frame[48] = 0x20;					// window 8192
		co_frame[49] = 0x00;
		co_frame[50] = 0x00;					// TCP checksum: set by co_set_csums()
		co_frame[51] = 0x00;
		co_frame[52] = 0x00;					// urgent pointer
		co_frame[53] = 0x00;
	} else {
		co_frame[38] = ((8 + CO_PAYLOAD) >> 8) & 0xFF;	// UDP length: header 8 + payload
		co_frame[39] = (8 + CO_PAYLOAD) & 0xFF;
		co_frame[40] = 0x00;					// UDP checksum: set by co_set_csums()
		co_frame[41] = 0x00;
	}
	/* payload */
	for (i = 0; i < CO_PAYLOAD; i++) {
		co_frame[hdr + i] = (uint8_t)i;
	}
	return hdr + CO_PAYLOAD;
}
/* Fill the two checksum fields of co_frame: 0, the correct value, or (L4 only) the pseudo-header sum.
 * The correct value is the one's complement of the folded sum (RFC 1071); for CIC = 10 the MAC wants the
 * folded pseudo-header sum NOT complemented in the field, it adds the L4 bytes to it and complements */
static void co_set_csums(int proto, int len, int ip_mode, int l4_mode)
{
	int l4_off = (proto == CO_PROTO_TCP) ? 50 : 40;	// where the TCP / UDP checksum field is
	int l4_len = len - 34;							// L4 header + payload
	uint16_t v;

	co_frame[24] = 0;
	co_frame[25] = 0;
	co_frame[l4_off] = 0;
	co_frame[l4_off + 1] = 0;
	if (ip_mode == CO_SEND_OK) {
		v = (uint16_t)~csum_fold(csum_add(0, co_frame + 14, 20));
		co_frame[24] = (v >> 8) & 0xFF;
		co_frame[25] = v & 0xFF;
	}
	if (l4_mode == CO_SEND_OK) {
		v = (uint16_t)~csum_fold(csum_add(csum_pseudo(co_frame, proto, l4_len), co_frame + 34, l4_len));
		co_frame[l4_off] = (v >> 8) & 0xFF;
		co_frame[l4_off + 1] = v & 0xFF;
	} else if (l4_mode == CO_SEND_PSEUDO) {
		v = csum_fold(csum_pseudo(co_frame, proto, l4_len));
		co_frame[l4_off] = (v >> 8) & 0xFF;
		co_frame[l4_off + 1] = v & 0xFF;
	}
}
/* 1: every byte of rx equals the frame sent, except the two checksum fields (the MAC may rewrite them) */
static int co_same_but_csums(const uint8_t *rx, int len, int proto)
{
	int l4_off = (proto == CO_PROTO_TCP) ? 50 : 40;
	int i;

	for (i = 0; i < len; i++) {
		if (i == 24 || i == 25 || i == l4_off || i == l4_off + 1) {
			continue;
		}
		if (rx[i] != co_frame[i]) {
			return 0;
		}
	}
	return 1;
}
static const char *co_mode_name(int mode)
{
	return (mode == CO_SEND_OK) ? "ok" : (mode == CO_SEND_PSEUDO) ? "pseudo" : "zero";
}
/* One byte of the dump: the two bytes of the checksum field at csum_off are printed as [XX XX] */
static void co_dump_byte(const uint8_t *f, int i, int csum_off)
{
	if (i == csum_off) {
		eth_printf(" [%02X", f[i]);
	} else if (i == csum_off + 1) {
		eth_printf(" %02X]", f[i]);
	} else {
		eth_printf(" %02X", f[i]);
	}
}
/* Hex dump of frame f by field: Ethernet | IP header | TCP/UDP header | first 16 payload bytes.
 * The two checksum fields are printed as [XX XX] and repeated on the title line */
static void co_frame_dump(const char *title, const uint8_t *f, int len, int proto)
{
	const char *l4 = (proto == CO_PROTO_TCP) ? "TCP" : "UDP";
	int l4_end = (proto == CO_PROTO_TCP) ? CO_TCP_HDR : CO_UDP_HDR;	// 54 / 42: first payload byte
	int l4_off = (proto == CO_PROTO_TCP) ? 50 : 40;					// where the TCP / UDP checksum field is
	int i;

	eth_printf("      %s: %d bytes, IP csum [24..25] = %02X %02X, %s csum [%d..%d] = %02X %02X\n",
			   title, len, f[24], f[25], l4, l4_off, l4_off + 1, f[l4_off], f[l4_off + 1]);
	eth_printf("        [  0.. 13] Ethernet:");
	for (i = 0; i < 14; i++) {
		eth_printf(" %02X", f[i]);
	}
	eth_printf("\n        [ 14.. 33] IP hdr  :");
	for (i = 14; i < 34; i++) {
		co_dump_byte(f, i, 24);				// [24..25] IP header checksum
	}
	eth_printf("\n        [ 34..%3d] %s hdr :", l4_end - 1, l4);
	for (i = 34; i < l4_end; i++) {
		co_dump_byte(f, i, l4_off);			// [50..51] TCP / [40..41] UDP checksum
	}
	eth_printf("\n        [%3d..%3d] payload :", l4_end, len - 1);
	for (i = l4_end; i < l4_end + 16 && i < len; i++) {
		eth_printf(" %02X", f[i]);
	}
	eth_printf(" ... (%d bytes)\n", len - l4_end);
}

/**
 * One case: build the frame, choose what goes in the checksum fields, send it with TDES3.CIC = cic,
 * read it back, verify the checksums in software and compare with what RDES1 says.
 *
 * What is expected is derived from the inputs, not listed per case:
 *   wire IP ok   = the CPU wrote it (ip_mode ok)  OR the MAC inserted it (cic >= 01)
 *   wire L4 ok   = the CPU wrote it (l4_mode ok)  OR the MAC inserted it (cic >= 10)
 *   RDES1.IPHE   = NOT wire IP ok              RDES1.PT   = 2 TCP / 1 UDP (protocol byte, also when IPHE = 1)
 *   RDES1.IPCE   = NOT wire L4 ok (also when IPHE = 1: a wrong IP checksum does not stop the payload check)
 *   TX write-back IHE / PCE = 0 when the MAC was asked to insert (cic != 00)
 *
 * @param cic      TDES3.CIC value (TDES3_CIC_NONE / IP / IP_PAYLOAD / FULL)
 * @param proto    CO_PROTO_TCP / CO_PROTO_UDP
 * @param ip_mode  CO_SEND_ZERO / CO_SEND_OK: IP header checksum field sent
 * @param l4_mode  CO_SEND_ZERO / CO_SEND_OK / CO_SEND_PSEUDO: TCP/UDP checksum field sent
 * Return 1: as expected, 0: else
 */
static int co_case(uint8_t mac_idx, const char *id, uint32_t cic, int proto, int ip_mode, int l4_mode, const char *what)
{
	uint32_t tdes3_wb;							// TX write-back TDES3 of the frame (IHE, PCE)
	uint32_t rdes3 = 0;							// RX write-back RDES3 (PL, ES)
	uint32_t rdes1;								// RX write-back RDES1 (COE status)
	int len, tx, rx_len;
	int ip_ok, l4_ok, exp_ip, exp_l4, exp_pt;
	int ipv4, ipcb, iphe, ipce, pt, ihe, pce;
	int ok_wire, ok_rdes1, ok_tx, ok;

	/* 1. the frame: same bytes every case, only the checksum fields differ */
	len = co_build(proto);
	co_set_csums(proto, len, ip_mode, l4_mode);
	/* 2. send with CIC: egress_packet() puts eth_tx_cic in TDES3 [17:16] of this frame */
	eth_tx_cic = (uint8_t)cic;
	tx = egress_packet(co_frame, len);
	tdes3_wb = tx_desc[(eth_tx_head - 1) & (MAC_100M_TX_DESC_CNT - 1)].des3;	// descriptor just used
	if (tx < 0) {
		eth_printf("  %s  TX error %d\n", id, tx);	// -2 busy, -3 timeout, -4 TDES3.ES
		return 0;
	}
	/* 3. receive: ingress_packet_status() copies the frame and keeps RDES1 in eth_rx_rdes1 */
	rx_len = ingress_packet_status(mac_idx, co_rxbuf, sizeof(co_rxbuf), ETH_TEST_RX_TIMEOUT_US, NULL, &rdes3);
	rdes1 = eth_rx_rdes1;
	if (rx_len < 0) {
		eth_printf("  %s  RX error %d (RDES3=0x%08lX)\n", id, rx_len, (unsigned long)rdes3);	// -1 nothing came, -3 ES
		return 0;
	}
	/* 4. the wire: what the software finds in the frame that came back */
	ip_ok = ip_csum_ok(co_rxbuf);
	l4_ok = (rx_len == len) && l4_csum_ok(co_rxbuf, proto, rx_len - 34);
	exp_ip = (ip_mode == CO_SEND_OK) || (cic >= TDES3_CIC_IP);
	exp_l4 = (l4_mode == CO_SEND_OK) || (cic >= TDES3_CIC_IP_PAYLOAD);
	ok_wire = (rx_len == len) && co_same_but_csums(co_rxbuf, len, proto) && (ip_ok == exp_ip) && (l4_ok == exp_l4);
	/* 5. RDES1: what the RX COE found (p.3066) */
	ipv4 = (rdes1 & RDES1_IPV4) != 0;
	ipcb = (rdes1 & RDES1_IPCB) != 0;
	iphe = (rdes1 & RDES1_IPHE) != 0;
	ipce = (rdes1 & RDES1_IPCE) != 0;
	pt = (int)(rdes1 & RDES1_PT_MASK);
	exp_pt = (proto == CO_PROTO_TCP) ? RDES1_PT_TCP : RDES1_PT_UDP;	// from the protocol byte, even with IPHE = 1
	ok_rdes1 = ((rdes3 & RDES3_RS1V) != 0) && ipv4 && !ipcb && (iphe == !exp_ip) && (pt == exp_pt) && (ipce == !exp_l4);
	/* 6. TX write-back: the TX COE must not have failed when it was asked to insert (p.3055) */
	ihe = (tdes3_wb & TDES3_IHE) != 0;
	pce = (tdes3_wb & TDES3_PCE) != 0;
	ok_tx = (cic == TDES3_CIC_NONE) || (!ihe && !pce);

	ok = ok_wire && ok_rdes1 && ok_tx;
	/* 7. print: column header + table line (same widths), then the details of this case indented under them */
	eth_printf("\n" CO_TABLE_HDR "\n");
	eth_printf("  %-2s  %lu%lu   %-7s %-7s | %-3s %-3s | %-4d %-2d %-4d %-4d %-4d | %-3d %-3d | %s\n",
			   id, (unsigned long)(cic >> 1), (unsigned long)(cic & 1U), co_mode_name(ip_mode), co_mode_name(l4_mode),
			   ip_ok ? "ok" : "BAD", l4_ok ? "ok" : "BAD", ipv4, pt, iphe, ipce, ipcb, ihe, pce, ok ? "OK" : "BAD");
	eth_printf("      %s\n", what);
	co_frame_dump("frame sent (after co_set_csums)", co_frame, len, proto);
	co_frame_dump("frame received (after the MAC)", co_rxbuf, rx_len, proto);
	if (!ok) {
		eth_printf("      expected: wire ip %s l4 %s, IPHE=%d IPCE=%d PT=%d, IHE=PCE=0 -> wire %s, RDES1 %s, TX %s\n",
				   exp_ip ? "ok" : "BAD", exp_l4 ? "ok" : "BAD", !exp_ip, !exp_l4, exp_pt,
				   ok_wire ? "OK" : "BAD", ok_rdes1 ? "OK" : "BAD", ok_tx ? "OK" : "BAD");
		eth_printf("      raw: RDES1=0x%08lX RDES3=0x%08lX TDES3=0x%08lX\n",
				   (unsigned long)rdes1, (unsigned long)rdes3, (unsigned long)tdes3_wb);
	}
	return ok;
}

/**
 * Checksum offload test through the MAC loopback (eth_run_test_ops = 7).
 *
 *   Register / descriptor      Bit [pos]       Value          Function
 *   -------------------------- --------------- -------------- -----------------------------------------
 *   MAC_HW_Feature0 p.3142 (read) TXCOESEL [14]   1              the core has the TX checksum engine
 *                              RXCOESEL [16]   1              the core has the RX checksum engine
 *   MAC_Configuration p.3112           IPC [27]        1              RX COE on: RDES1 IPHE / IPCE / PT written
 *   MTL_RxQ0_Operation_Mode p.3106       DIS_TCP_EF [6]  1              keep the frames with a checksum error (init_mac_internal_lb)
 *   TDES3 (each TX, p.3052)    CIC [17:16]     per case       what the TX COE inserts (eth_tx_cic)
 *   TDES3 write-back p.3055    IHE [0], PCE [12] 0            TX COE could do it
 *   RDES1 write-back p.3066    IPV4 [4] PT [2:0] IPHE [3] IPCE [7] IPCB [6]   RX COE result
 * Return 1: all cases as expected, 0: else
 */
uint16_t eth_mac_coe_lb(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t machwf0r;
	uint32_t maccr;
	uint32_t ok = 0;
	eth_printf("Start Init: Port[%d]\n", mac_idx);

	if (eth_reset_mac(mac_idx) == 0) {
		return 0;
	}
	mac_init_100m(mac_idx);
	/* both engines must exist on this core: MAC_HW_Feature0.TXCOESEL / RXCOESEL (p.3142) */
	machwf0r = AX_BASE_READ_REG32(mac_base, R_MAC_HW_FEATURE0);
	eth_printf("MAC_HW_Feature0 = 0x%08lX: TXCOESEL [14] = %d, RXCOESEL [16] = %d\n", (unsigned long)machwf0r,
			   (machwf0r & MAC_HW_FEATURE0_TXCOESEL) != 0, (machwf0r & MAC_HW_FEATURE0_RXCOESEL) != 0);
	if ((machwf0r & (MAC_HW_FEATURE0_TXCOESEL | MAC_HW_FEATURE0_RXCOESEL)) != (MAC_HW_FEATURE0_TXCOESEL | MAC_HW_FEATURE0_RXCOESEL)) {
		eth_printf("checksum offload is not available on this core, test stopped\n");
		return 0;
	}
	/* RX COE on, while the receiver is still off (mac_start() sets RE) */
	maccr = AX_BASE_READ_REG32(mac_base, R_MAC_CONFIGURATION)
		  | MAC_CONFIGURATION_IPC;												// IPC [27]: check IP / TCP / UDP checksums on RX
	AX_BASE_WRITE_REG32(mac_base, R_MAC_CONFIGURATION, maccr);
	FLOW("[8] COE:\n");
	flow_regw("MAC_Configuration (IPC)", mac_base, R_MAC_CONFIGURATION, maccr, FF_MAC_CONFIGURATION);
	init_mac_internal_lb(mac_idx);
	mac_start(mac_idx);

	eth_printf("\n================ CHECKSUM OFFLOAD TEST (TDES3.CIC, MAC_Configuration.IPC, RDES1) ================\n");
	eth_printf("  same %d-byte TCP/IP frame every case (G: UDP, 142 bytes), checksum fields sent as 0 / correct / pseudo-header sum.\n", CO_TCP_HDR + CO_PAYLOAD);
	eth_printf("  'wire' = checksums verified in software on the frame received; 'RDES1' = what the RX COE reported.\n");
	eth_printf("  Case D prints the [10]/[11] FLOW lines.\n");
	eth_printf("  columns: IP.sent L4.sent = what the CPU wrote | ip l4 = wire | IPV4..IPCB = RDES1 (RX COE) | IHE PCE = TDES3\n");

/*
Case  CIC  CPU writes to co_frame      MAC inserts in TX FIFO                         Received on the wire
A     00   IP = 0, TCP correct         none                                           IP wrong, TCP correct
B     00   IP correct, TCP = 0         none                                           IP correct, TCP wrong
C     00   IP correct, TCP correct     none                                           both correct (reference)
D     01   IP = 0, TCP = 0             IP                                             IP correct, TCP wrong
E     11   IP = 0, TCP = 0             IP + TCP, MAC builds pseudo hZeader itself      both correct
F     10   IP = 0, TCP = fold(pseudo)  IP + TCP, MAC continues from CPU's pseudo sum  both correct
G     11   IP = 0, UDP = 0             IP + UDP                                       both correct, PT = 1
H     00   IP = 0, TCP = 0             none                                           both wrong
*/
	eth_flow_trace = 0;
	ok += co_case(mac_idx, "A", TDES3_CIC_NONE,       CO_PROTO_TCP, CO_SEND_ZERO, CO_SEND_OK,     "no insertion, IP checksum wrong: IPHE = 1, payload still checked (PT = 2, IPCE = 0)");
	eth_flow_trace = 0;
	ok += co_case(mac_idx, "B", TDES3_CIC_NONE,       CO_PROTO_TCP, CO_SEND_OK,   CO_SEND_ZERO,   "no insertion, TCP checksum wrong: RX COE must flag IPCE");
	eth_flow_trace = 0;
	ok += co_case(mac_idx, "C", TDES3_CIC_NONE,       CO_PROTO_TCP, CO_SEND_OK,   CO_SEND_OK,     "no insertion, both correct by the CPU: reference");
	eth_flow_trace = 1;
	ok += co_case(mac_idx, "D", TDES3_CIC_IP,         CO_PROTO_TCP, CO_SEND_ZERO, CO_SEND_ZERO,   "CIC 01: the MAC inserts the IP checksum only");
	eth_flow_trace = 0;
	ok += co_case(mac_idx, "E", TDES3_CIC_FULL,       CO_PROTO_TCP, CO_SEND_ZERO, CO_SEND_ZERO,   "CIC 11: the MAC inserts both, pseudo header included");
	eth_flow_trace = 0;
	ok += co_case(mac_idx, "F", TDES3_CIC_IP_PAYLOAD, CO_PROTO_TCP, CO_SEND_ZERO, CO_SEND_PSEUDO, "CIC 10: CPU gives the pseudo-header sum, the MAC adds the rest");
	eth_flow_trace = 0;
	ok += co_case(mac_idx, "G", TDES3_CIC_FULL,       CO_PROTO_UDP, CO_SEND_ZERO, CO_SEND_ZERO,   "CIC 11 with UDP: PT = 1");
	eth_flow_trace = 0;
	ok += co_case(mac_idx, "H", TDES3_CIC_NONE,       CO_PROTO_TCP, CO_SEND_ZERO, CO_SEND_ZERO,   "no insertion, both wrong: IPHE = 1 and IPCE = 1 together");
	eth_tx_cic = TDES3_CIC_NONE;				// back to plain frames for the other tests

	eth_printf("\nResult: %lu/8 cases as expected.\n", (unsigned long)ok);
	eth_print_mac_counters(mac_idx);
	return (ok == 8) ? 1 : 0;
}
