#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_istat_utils.h"
#include "eth_traffic_test.h"
#include "eth_printf.h"
#include "eth_flow_utils.h"

/*
 * Interrupt STATUS test (eth_run_test_ops = 12), through the MAC loopback.
 * No interrupt is enabled: DMA_CH0_Interrupt_Enable = 0 (mac_dma_init), MAC_Interrupt_Enable = 0 and the enable bits of MTL_Q0_Interrupt_Control_Status = 0
 * (reset values), the NVIC is not touched. The test makes events happen and checks that the STATUS registers
 * report them anyway, that the summary bits AIS / NIS stay 0 (p.3094: only enabled bits feed them), and that
 * the bits clear the way the RM says (rc_w1: write 1 to clear; counters clear on read).
 *
 *   DMA_CH0_Status p.3093-3095   TI [0] TPS [1] TBU [2] RI [6] RBU [7] RPS [8] RWT [9] ETI [10] ERI [11] FBE [12] CDE [13]
 *                            AIS [14] NIS [15] TEB [18:16] REB [21:19]
 *   DMA_Interrupt_Status p.3077        DC0IS [0] = a DMA channel bit is set, MTLIS [16], MACIS [17]: summaries, read-only
 *   DMA_Debug_Status0 p.3078        TPS0 [15:12] / RPS0 [11:8]: DMA process state (0 stopped, 3 waiting, 4 / 6 suspended)
 *   MTL_Interrupt_Status p.3101        Q0IS [0]; MTL_Q0_Interrupt_Control_Status p.3104: RXOVFIS [16], TXUNFIS [0] (rc_w1), RXOIE [24], TXUIE [8] = 0
 *   MTL_RxQ0_Missed_Packet_Overflow_Cnt p.3108   OVFPKTCNT [10:0], MISPKTCNT [26:16] (clear on read); MTL_RxQ0_Debug p.3109: PRXQ [29:16]
 *   MAC_Interrupt_Status p.3131        RXSTSIS [14] (set by MAC_Rx_Tx_Status.RWT [8], p.3134); MAC_Interrupt_Enable p.3133 = 0
 *   MAC_WD_JB_Timeout p.3120        PWE [8], WTO [3:0]: RX watchdog length (case G)
 *   DMA_CH0_Miss_Frame_Cnt p.3096      MFC [10:0]: frames dropped by the DMA (clear on read)
 *   TDES2.IOC [31] p.3050, RDES3.IOC [30] p.3064: TI / RI are set only for descriptors that ask for it
 *
 * Cases (every frame 60 bytes, EtherType 0x88B5, unless said):
 *   A   after init + start, nothing sent             -> status printed, then one write clears it: DMA_CH0_Status = 0
 *   B   1 frame, TDES2.IOC = 0, RDES3.IOC = 0        -> TBU ETI ERI = 1; TI RI = 0 (no IOC); AIS NIS = 0 (no enable)
 *   C   1 frame, IOC = 1 on both descriptors         -> TI RI = 1 as well; AIS NIS still 0. The RX ring is re-armed with
 *                                                       IOC by 8 frames first (the DMA already holds the next descriptor);
 *                                                       RDES3.IOC stays 1 after C, so RI shows up in later cases (not checked)
 *   D   write 1 to TI only                           -> TI = 0, the other bits stay; write 0 changes nothing; write all -> 0
 *   E   9 frames, none read: the 8 RX descriptors run out -> RBU = 1, RPS0 = 4 (suspended), frame 9 waits in the MTL FIFO
 *                                                       (PRXQ = 1, nothing dropped); reading 8 frames restarts the DMA, 9/9 back
 *   F   10 frames of 1514 bytes, none read            -> RBU; the FIFO (2048 bytes) holds 1 frame, the next one overflows:
 *                                                       MTL_Q0_Interrupt_Control_Status.RXOVFIS = 1, OVFPKTCNT = 1; 9/10 come back (RSF = 0,
 *                                                       threshold mode: a partial frame handed over would carry RDES3.OE)
 *   G   MAC_WD_JB_Timeout PWE = 1, WTO = 2 KB, 2100-byte frame -> RX cut: RDES3 ES + RWT, DMA_CH0_Status.RWT = 1, MAC_Rx_Tx_Status.RWT = 1,
 *                                                       MAC_Interrupt_Status.RXSTSIS = 1 (printed); how RWT clears (write 1 or write 0)
 *   H1  DMA_CH0_Tx_Control.ST = 0                          -> TPS = 1, TPS0 = 0 (stopped); ST = 1 again, a frame still goes through
 *   H2  DMA_CH0_Rx_Control.SR = 0                          -> RPS = 1, RPS0 = 0; SR = 1 again, a frame still comes back
 *   I   TX buffer address in DTCM (0x20000000, not reachable by the Ethernet DMA, RM §2.1.7 p.113)
 *                                                    -> FBE = 1, TEB = read error on a data buffer, TPS = 1, DMA dead:
 *                                                       last case, the next test starts with eth_reset_mac()
 * Each case: description -> action -> one status line (DMA_CH0_Status hex + the names of the bits set, DMA_Interrupt_Status, TPS0/RPS0,
 * MTL_Interrupt_Status, MTL_Q0_Interrupt_Control_Status, MAC_Interrupt_Status) -> expect line -> OK / BAD. Case B prints the [10]/[11] FLOW lines.
 * Board 2026-11-07 (mode 4 case F): DMA_CH0_Status = 0x2C04 = TBU ETI ERI CDE with AIS = 0 and DMA_CH0_Interrupt_Enable = 0: the summary
 * rule of p.3094 already seen once.
 */

#define IS_FRAME_LEN		60
#define IS_BIG_LEN			1514	// case F: 1514 + 4 FCS = 1518 bytes in a 2048-byte RX FIFO
#define IS_RWT_LEN			2100	// case G: longer than the 2 KB watchdog (MAC_WD_JB_Timeout.WTO = 0), shorter than the 2304-byte buffer
#define IS_RING_FILL		(MAC_100M_RX_DESC_CNT + 1)		// case E: one more frame than RX descriptors
#define IS_OVF_FRAMES		(MAC_100M_RX_DESC_CNT + 2)		// case F: ring full + 1 in the FIFO + 1 that overflows
#define IS_SETTLE_US		200		// after a frame / a stop command: time for the status bits to land
#define IS_FBE_WAIT_US		10000	// case I: wait for the bus error
#define IS_DTCM_ADDR		0x20000000UL	// DTCM: CPU and MDMA only, the Ethernet DMA gets an AHB error (RM §2.1.7 p.113)
#define IS_N_CASES			10
#define IS_TEB_READ			(1U << 0)	// TEB bit 0: error during a read transfer
#define IS_TEB_DESC			(1U << 1)	// TEB bit 1: error on a descriptor access (0 = data buffer)
#define IS_TEB_TXDATA		(1U << 2)	// TEB bit 2: error during data transfer by the TX DMA
/* every rc_w1 bit of DMA_CH0_Status (p.3094 access column) except RWT [9], whose column says rw: case G tries it */
#define IS_DMA_CH0_STATUS_W1C		(DMA_CH0_STATUS_TI | DMA_CH0_STATUS_TPS | DMA_CH0_STATUS_TBU | DMA_CH0_STATUS_RI | DMA_CH0_STATUS_RBU | DMA_CH0_STATUS_RPS | DMA_CH0_STATUS_ETI | \
							 DMA_CH0_STATUS_ERI | DMA_CH0_STATUS_FBE | DMA_CH0_STATUS_CDE | DMA_CH0_STATUS_AIS | DMA_CH0_STATUS_NIS)

/* The status registers read together, in this order (MAC_Interrupt_Status last: reading it clears PHYIS only) */
struct is_snap {
	uint32_t dmacsr;
	uint32_t dmaisr;
	uint32_t dmadsr;
	uint32_t mtlisr;
	uint32_t mtlqicsr;
	uint32_t macisr;
};

static const struct {
	uint32_t    mask;
	const char *name;
} IS_DMA_CH0_STATUS_BITS[] = {
	{DMA_CH0_STATUS_TI, "TI"}, {DMA_CH0_STATUS_TPS, "TPS"}, {DMA_CH0_STATUS_TBU, "TBU"}, {DMA_CH0_STATUS_RI, "RI"}, {DMA_CH0_STATUS_RBU, "RBU"},
	{DMA_CH0_STATUS_RPS, "RPS"}, {DMA_CH0_STATUS_RWT, "RWT"}, {DMA_CH0_STATUS_ETI, "ETI"}, {DMA_CH0_STATUS_ERI, "ERI"}, {DMA_CH0_STATUS_FBE, "FBE"},
	{DMA_CH0_STATUS_CDE, "CDE"}, {DMA_CH0_STATUS_AIS, "AIS"}, {DMA_CH0_STATUS_NIS, "NIS"},
};
#define IS_N_BITS (sizeof IS_DMA_CH0_STATUS_BITS / sizeof IS_DMA_CH0_STATUS_BITS[0])

static uint8_t is_frame[MAC_100M_BUF_SIZE];
static uint8_t is_rxbuf[MAC_100M_BUF_SIZE];
static char    is_summary[128];		// "Cases: A:OK B:BAD ..."

static void is_summary_add(const char *id, int ok)
{
	size_t n = strlen(is_summary);

	if (n + strlen(id) + 6 < sizeof is_summary) {
		snprintf(is_summary + n, sizeof is_summary - n, "%s:%s ", id, ok ? "OK" : "BAD");
	}
}
/* Build the test frame: DA | SA | 88 B5 | payload 0x10, 0x11, ... (len bytes, no FCS) */
static void is_build(int len)
{
	unsigned char da[ETH_TEST_MAC_SIZE] = ETH_TEST_DEST_MAC;
	unsigned char sa[ETH_TEST_MAC_SIZE] = ETH_TEST_SRC_MAC;
	int i;

	memcpy(is_frame, da, ETH_TEST_MAC_SIZE);
	memcpy(is_frame + ETH_TEST_MAC_SIZE, sa, ETH_TEST_MAC_SIZE);
	is_frame[12] = (ETH_TEST_ETHER_TYPE >> 8) & 0xFF;
	is_frame[13] = ETH_TEST_ETHER_TYPE & 0xFF;
	for (i = ETH_TEST_HEADER_SIZE; i < len; i++) {
		is_frame[i] = (uint8_t)(0x10 + (i % 0x60));
	}
}
/* Send one frame of len bytes; 1 = sent (egress_packet returned len) */
static int is_send(int len)
{
	int tx;

	is_build(len);
	tx = egress_packet(is_frame, len);
	if (tx != len) {
		eth_printf("      TX error %d (frame %d bytes)\n", tx, len);
		return 0;
	}
	return 1;
}
/* Receive one frame into is_rxbuf: ingress_packet_status() value (length, -1 nothing, -3 error), RDES3 in *rdes3 */
static int is_recv(uint8_t mac_idx, int timeout_us, uint32_t *rdes3)
{
	*rdes3 = 0;
	return ingress_packet_status(mac_idx, is_rxbuf, sizeof(is_rxbuf), timeout_us, NULL, rdes3);
}
/* Read the 6 status registers */
static void is_read(uint32_t mac_base, struct is_snap *s)
{
	s->dmacsr = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_STATUS);
	s->dmaisr = AX_BASE_READ_REG32(mac_base, R_DMA_INTERRUPT_STATUS);
	s->dmadsr = AX_BASE_READ_REG32(mac_base, R_DMA_DEBUG_STATUS0);
	s->mtlisr = AX_BASE_READ_REG32(mac_base, R_MTL_INTERRUPT_STATUS);
	s->mtlqicsr = AX_BASE_READ_REG32(mac_base, R_MTL_Q0_INTERRUPT_CONTROL_STATUS);
	s->macisr = AX_BASE_READ_REG32(mac_base, R_MAC_INTERRUPT_STATUS);
}
/* Write 1 to every rc_w1 bit of DMA_CH0_Status (not RWT) and to the two status bits of MTL_Q0_Interrupt_Control_Status */
static void is_clear(uint32_t mac_base)
{
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_STATUS, IS_DMA_CH0_STATUS_W1C);
	AX_BASE_WRITE_REG32(mac_base, R_MTL_Q0_INTERRUPT_CONTROL_STATUS, MTL_Q0_INTERRUPT_CONTROL_STATUS_RXOVFIS | MTL_Q0_INTERRUPT_CONTROL_STATUS_TXUNFIS);		// enables RXOIE / TXUIE stay 0
}
/* Names of the DMA_CH0_Status bits set in v, e.g. "TBU ETI ERI" ("-" when none) */
static void is_print_bits(uint32_t v)
{
	uint32_t i, n = 0;

	for (i = 0; i < IS_N_BITS; i++) {
		if (v & IS_DMA_CH0_STATUS_BITS[i].mask) {
			eth_printf("%s%s", n ? " " : "", IS_DMA_CH0_STATUS_BITS[i].name);
			n++;
		}
	}
	if (n == 0) {
		eth_printf("-");
	}
}
/* The status line of a case */
static void is_print_snap(const struct is_snap *s)
{
	eth_printf("      DMA_CH0_Status = 0x%08lX [", (unsigned long)s->dmacsr);
	is_print_bits(s->dmacsr);
	eth_printf("]  TEB = %lu REB = %lu | DMA_Interrupt_Status = 0x%08lX (DC0IS %d MTLIS %d MACIS %d) | TPS0 = %lu RPS0 = %lu\n",
			   (unsigned long)((s->dmacsr >> DMA_CH0_STATUS_TEB_bit) & DMA_CH0_STATUS_EB_MASK),
			   (unsigned long)((s->dmacsr >> DMA_CH0_STATUS_REB_bit) & DMA_CH0_STATUS_EB_MASK), (unsigned long)s->dmaisr,
			   (s->dmaisr & DMA_INTERRUPT_STATUS_DC0IS) != 0, (s->dmaisr & DMA_INTERRUPT_STATUS_MTLIS) != 0, (s->dmaisr & DMA_INTERRUPT_STATUS_MACIS) != 0,
			   (unsigned long)((s->dmadsr >> DMA_DEBUG_STATUS0_TPS0_bit) & DMA_DEBUG_STATUS0_PS_MASK),
			   (unsigned long)((s->dmadsr >> DMA_DEBUG_STATUS0_RPS0_bit) & DMA_DEBUG_STATUS0_PS_MASK));
	eth_printf("      MTL_Interrupt_Status = 0x%08lX (Q0IS %d) | MTL_Q0_Interrupt_Control_Status = 0x%08lX (RXOVFIS %d TXUNFIS %d) | MAC_Interrupt_Status = 0x%08lX (RXSTSIS %d TXSTSIS %d TSIS %d)\n",
			   (unsigned long)s->mtlisr, (s->mtlisr & MTL_INTERRUPT_STATUS_Q0IS) != 0, (unsigned long)s->mtlqicsr,
			   (s->mtlqicsr & MTL_Q0_INTERRUPT_CONTROL_STATUS_RXOVFIS) != 0, (s->mtlqicsr & MTL_Q0_INTERRUPT_CONTROL_STATUS_TXUNFIS) != 0, (unsigned long)s->macisr,
			   (s->macisr & MAC_INTERRUPT_STATUS_RXSTSIS) != 0, (s->macisr & MAC_INTERRUPT_STATUS_TXSTSIS) != 0, (s->macisr & MAC_INTERRUPT_STATUS_TSIS) != 0);
}
/**
 * Check DMA_CH0_Status of a snapshot: every bit of must1 set, every bit of must0 clear. Prints the snapshot, the
 * expectation and the verdict (extra_ok: result of the checks the case made itself, 1 when none).
 * Return 1: OK, 0: BAD
 */
static int is_check(const char *id, const struct is_snap *s, uint32_t must1, uint32_t must0, int extra_ok)
{
	int ok = ((s->dmacsr & must1) == must1) && ((s->dmacsr & must0) == 0U) && extra_ok;

	is_print_snap(s);
	eth_printf("      expect DMA_CH0_Status: 1 = [");
	is_print_bits(must1);
	eth_printf("], 0 = [");
	is_print_bits(must0);
	eth_printf("]%s -> %s\n", extra_ok ? "" : ", plus a check of the case that failed", ok ? "OK" : "BAD");
	is_summary_add(id, ok);
	return ok;
}
static void is_title(const char *id, const char *what)
{
	eth_printf("=======================================================================================\n");
	eth_printf("  %s  %s\n", id, what);
}
/* Case B / C (and H1 / H2): one frame out and back. ioc = 1: TDES2.IOC on the frame and, from now on, RDES3.IOC on every
 * RX descriptor given back. The 8 descriptors of the ring were armed without IOC and the DMA holds the next one already
 * (RM: never touch a descriptor the DMA owns), so C first sends MAC_100M_RX_DESC_CNT frames to re-arm the whole ring
 * with IOC, then the frame that is checked. Return 1: frame back and same */
static int is_one_frame(uint8_t mac_idx, uint32_t mac_base, int ioc)
{
	uint32_t rdes3, i;
	int rx;

	eth_tx_ioc = (uint8_t)ioc;
	if (ioc) {
		eth_rx_ioc = 1;		// stays 1 for the rest of the test: RI is set by every frame after C (checked in C only)
		for (i = 0; i < MAC_100M_RX_DESC_CNT; i++) {
			if (!is_send(IS_FRAME_LEN) || is_recv(mac_idx, ETH_TEST_RX_TIMEOUT_US, &rdes3) != IS_FRAME_LEN) {
				return 0;
			}
		}
	}
	is_clear(mac_base);
	if (!is_send(IS_FRAME_LEN)) {
		return 0;
	}
	rx = is_recv(mac_idx, ETH_TEST_RX_TIMEOUT_US, &rdes3);
	udelay(IS_SETTLE_US);
	eth_tx_ioc = 0;
	if (rx != IS_FRAME_LEN || memcmp(is_rxbuf, is_frame, IS_FRAME_LEN) != 0) {
		eth_printf("      RX: %d (expected %d bytes, same)\n", rx, IS_FRAME_LEN);
		return 0;
	}
	return 1;
}
/* Case E / F: send n frames of len bytes without reading any. Return the number sent */
static uint32_t is_send_n(uint32_t n, int len)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (!is_send(len)) {
			break;
		}
	}
	return i;
}
/* Case E / F: read up to n frames, count those identical to the frame sent (same length). Return the count */
static uint32_t is_drain(uint8_t mac_idx, uint32_t n, int len)
{
	uint32_t got = 0, i;
	uint32_t rdes3;
	int rx;

	is_build(len);
	for (i = 0; i < n; i++) {
		rx = is_recv(mac_idx, ETH_TEST_RX_TIMEOUT_US, &rdes3);
		if (rx == -1) {
			eth_printf("      frame %lu: nothing more (timeout)\n", (unsigned long)(i + 1));
			break;
		}
		if (rx == len && memcmp(is_rxbuf, is_frame, len) == 0) {
			got++;
		} else {
			eth_printf("      frame %lu: %d bytes / error, RDES3 = 0x%08lX (ES %d OE %d)\n", (unsigned long)(i + 1), rx,
					   (unsigned long)rdes3, (rdes3 & RDES3_ES) != 0, (rdes3 & RDES3_OE) != 0);
		}
	}
	return got;
}
/* Cases H1 / H2: one bit of a DMA control register off, status read, bit on again, then one frame to prove it still works */
static int is_stop_case(uint8_t mac_idx, uint32_t mac_base, const char *id, uint32_t reg, uint32_t bit, uint32_t must1,
						uint32_t ps_bit, const char *what)
{
	struct is_snap s;
	uint32_t v, ps;
	int ok;

	is_title(id, what);
	is_clear(mac_base);
	v = AX_BASE_READ_REG32(mac_base, reg) & ~bit;
	AX_BASE_WRITE_REG32(mac_base, reg, v);				// ST / SR = 0: stop command
	udelay(IS_SETTLE_US);
	is_read(mac_base, &s);
	ps = (s.dmadsr >> ps_bit) & DMA_DEBUG_STATUS0_PS_MASK;
	eth_printf("      stop bit written: register = 0x%08lX, process state = %lu (0 = stopped)\n", (unsigned long)v, (unsigned long)ps);
	AX_BASE_WRITE_REG32(mac_base, reg, v | bit);		// ST / SR = 1: start again
	udelay(IS_SETTLE_US);
	is_clear(mac_base);
	ok = is_one_frame(mac_idx, mac_base, 0);
	eth_printf("      started again: frame out and back %s\n", ok ? "SAME" : "FAILED");
	return is_check(id, &s, must1, DMA_CH0_STATUS_AIS | DMA_CH0_STATUS_NIS, (ps == DMA_DEBUG_STATUS0_PS_STOPPED) && ok);
}
/**
 * Interrupt status test through the MAC loopback (eth_run_test_ops = 12)
 * Init MAC, enable the loopback, start DMA and MAC (same bits as eth_mac_internal_lb(), DMA_CH0_Interrupt_Enable = 0), then the
 * cases on top of the file. Registers touched by the cases:
 *
 *   Register / descriptor   Bit [pos]            Value        Function
 *   ----------------------- -------------------- ------------ ------------------------------
 *   DMA_CH0_Status 0x1160       rc_w1 bits           write 1      clear (is_clear, case D)
 *   MTL_Q0_Interrupt_Control_Status p.3104     RXOVFIS [16] TXUNFIS [0] write 1  clear; RXOIE [24] TXUIE [8] stay 0
 *   TDES2 p.3050            IOC [31]             eth_tx_ioc   case C: TI for this frame
 *   RDES3 p.3064            IOC [30]             eth_rx_ioc   case C: RI for this descriptor
 *   MAC_WD_JB_Timeout p.3120       PWE [8], WTO [3:0]   1, 0         case G: RX watchdog at 2 KB; back to 0 after
 *   DMA_CH0_Tx_Control p.3079     ST [0]               0 then 1     case H1
 *   DMA_CH0_Rx_Control p.3081     SR [0]               0 then 1     case H2
 *   TDES0                   BUF1AP [31:0]        0x20000000   case I: buffer in DTCM -> bus error
 * Return 1: every case as expected, 0: else. After case I the DMA is stopped by the bus error (eth_reset_mac() next test).
 */
uint16_t eth_mac_int_status_lb(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	struct is_snap s;
	uint32_t ok = 0;
	uint32_t v, v1, v2, v3, idx, n, cnt, missed, ovf;
	uint32_t rdes3;
	int rx, extra;
	eth_printf("Start Init: Port[%d]\n", mac_idx);

	if (eth_reset_mac(mac_idx) == 0) {
		return 0;
	}
	mac_init_100m(mac_idx);
	init_mac_internal_lb(mac_idx);
	mac_start(mac_idx);
	is_summary[0] = 0;
	eth_tx_ioc = 0;
	eth_rx_ioc = 0;

	eth_printf("\n================ INTERRUPT STATUS TEST (DMA_CH0_Status / DMA_Interrupt_Status / MTL_Q0_Interrupt_Control_Status / MAC_Interrupt_Status, no interrupt enabled) ================\n");
	eth_printf("  DMA_CH0_Interrupt_Enable = 0x%08lX, MAC_Interrupt_Enable = 0x%08lX, MTL_Q0_Interrupt_Control_Status = 0x%08lX: nothing enabled, NVIC untouched.\n",
			   (unsigned long)AX_BASE_READ_REG32(mac_base, R_DMA_CH0_INTERRUPT_ENABLE), (unsigned long)AX_BASE_READ_REG32(mac_base, R_MAC_INTERRUPT_ENABLE),
			   (unsigned long)AX_BASE_READ_REG32(mac_base, R_MTL_Q0_INTERRUPT_CONTROL_STATUS));
	eth_printf("  Each case: event -> status registers -> expect -> OK / BAD. [..] = names of the DMA_CH0_Status bits set.\n");
	eth_printf("  TPS0 / RPS0 = DMA_Debug_Status0 process state: 0 stopped, 1 fetching, 3 waiting, 4 RX suspended, 6 TX suspended, 7 closing.\n");
	eth_printf("  Case B prints the [10]/[11] FLOW lines.\n");

	/* A: what the start leaves behind, and one clearing write */
	is_title("A", "after init + start, no frame: status as found, then write 1 to every rc_w1 bit");
	udelay(IS_SETTLE_US);
	is_read(mac_base, &s);
	eth_printf("      as found (information: TBU = 1 means the TX DMA already met tx_desc[0] owned by the CPU):\n");
	is_print_snap(&s);
	is_clear(mac_base);
	is_read(mac_base, &s);
	eth_printf("      after the clearing write:\n");
	ok += is_check("A", &s, 0U, IS_DMA_CH0_STATUS_W1C, 1);

	/* B: one frame without IOC */
	is_title("B", "1 frame, TDES2.IOC = 0 and RDES3.IOC = 0: TBU ETI ERI set, TI RI not, AIS NIS 0 (nothing enabled)");
	eth_flow_trace = 1;
	extra = is_one_frame(mac_idx, mac_base, 0);
	eth_flow_trace = 0;
	is_read(mac_base, &s);
	ok += is_check("B", &s, DMA_CH0_STATUS_TBU | DMA_CH0_STATUS_ETI | DMA_CH0_STATUS_ERI, DMA_CH0_STATUS_TI | DMA_CH0_STATUS_RI | DMA_CH0_STATUS_RBU | DMA_CH0_STATUS_AIS | DMA_CH0_STATUS_NIS, extra);

	/* C: one frame with IOC on both sides */
	is_title("C", "1 frame, TDES2.IOC = 1 and RDES3.IOC = 1: TI and RI appear, AIS NIS still 0");
	extra = is_one_frame(mac_idx, mac_base, 1);
	is_read(mac_base, &s);
	ok += is_check("C", &s, DMA_CH0_STATUS_TI | DMA_CH0_STATUS_RI | DMA_CH0_STATUS_TBU | DMA_CH0_STATUS_ETI | DMA_CH0_STATUS_ERI, DMA_CH0_STATUS_AIS | DMA_CH0_STATUS_NIS, extra);

	/* D: write-1-to-clear, bit by bit */
	is_title("D", "DMA_CH0_Status is rc_w1: write TI alone clears TI only; writing 0s changes nothing; writing all clears all");
	v = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_STATUS);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_STATUS, DMA_CH0_STATUS_TI);
	v1 = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_STATUS);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_STATUS, 0U);
	v2 = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_STATUS);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_STATUS, IS_DMA_CH0_STATUS_W1C);
	v3 = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_STATUS);
	eth_printf("      before 0x%08lX -> write TI: 0x%08lX -> write 0: 0x%08lX -> write all: 0x%08lX\n",
			   (unsigned long)v, (unsigned long)v1, (unsigned long)v2, (unsigned long)v3);
	extra = ((v & DMA_CH0_STATUS_TI) != 0U) && (v1 == (v & ~DMA_CH0_STATUS_TI)) && (v2 == v1);
	is_read(mac_base, &s);
	ok += is_check("D", &s, 0U, IS_DMA_CH0_STATUS_W1C, extra);

	/* E: receive buffer unavailable, frames kept in the MTL FIFO */
	is_title("E", "9 frames sent, none read: 8 RX descriptors used up -> RBU, RX DMA suspended, frame 9 waits in the FIFO");
	eth_rx_flush(mac_idx);
	is_clear(mac_base);
	(void)AX_BASE_READ_REG32(mac_base, R_MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT);		// counters clear on read: start from 0
	(void)AX_BASE_READ_REG32(mac_base, R_DMA_CH0_MISS_FRAME_CNT);
	n = is_send_n(IS_RING_FILL, IS_FRAME_LEN);
	udelay(IS_SETTLE_US);
	is_read(mac_base, &s);
	v = AX_BASE_READ_REG32(mac_base, R_MTL_RXQ0_DEBUG);
	cnt = (v >> MTL_RXQ0_DEBUG_PRXQ_bit) & MTL_RXQ0_DEBUG_PRXQ_MASK;
	v = AX_BASE_READ_REG32(mac_base, R_MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT);
	missed = (v >> MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT_MISPKTCNT_bit) & MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT_CNT_MASK;
	ovf = v & MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT_CNT_MASK;
	v = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_MISS_FRAME_CNT) & DMA_CH0_MISS_FRAME_CNT_MFC_MASK;
	eth_printf("      sent %lu; MTL RX FIFO holds PRXQ = %lu frame(s); MTL_RxQ0_Missed_Packet_Overflow_Cnt missed = %lu overflow = %lu; DMA_CH0_Miss_Frame_Cnt = %lu\n",
			   (unsigned long)n, (unsigned long)cnt, (unsigned long)missed, (unsigned long)ovf, (unsigned long)v);
	cnt = is_drain(mac_idx, IS_RING_FILL, IS_FRAME_LEN);
	eth_printf("      read back: %lu/%lu frames identical (the first give-back writes the RX tail: the DMA resumes)\n",
			   (unsigned long)cnt, (unsigned long)IS_RING_FILL);
	extra = (n == IS_RING_FILL) && (cnt == IS_RING_FILL) && (missed == 0U) && (ovf == 0U)
		 && (((s.dmadsr >> DMA_DEBUG_STATUS0_RPS0_bit) & DMA_DEBUG_STATUS0_PS_MASK) == DMA_DEBUG_STATUS0_PS_RX_SUSPENDED);
	ok += is_check("E", &s, DMA_CH0_STATUS_RBU, DMA_CH0_STATUS_AIS | DMA_CH0_STATUS_NIS | DMA_CH0_STATUS_FBE, extra);
	is_clear(mac_base);
	v = AX_BASE_READ_REG32(mac_base, R_DMA_DEBUG_STATUS0);
	eth_printf("      after the drain: RPS0 = %lu (3 = waiting for a packet again)\n",
			   (unsigned long)((v >> DMA_DEBUG_STATUS0_RPS0_bit) & DMA_DEBUG_STATUS0_PS_MASK));

	/* F: the same with big frames: the FIFO overflows */
	is_title("F", "10 frames of 1514 bytes, none read: ring full + 1 frame in the 2048-byte FIFO, the next overflows (RXOVFIS)");
	is_clear(mac_base);
	(void)AX_BASE_READ_REG32(mac_base, R_MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT);
	n = is_send_n(IS_OVF_FRAMES, IS_BIG_LEN);
	udelay(IS_SETTLE_US);
	is_read(mac_base, &s);
	v = AX_BASE_READ_REG32(mac_base, R_MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT);
	missed = (v >> MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT_MISPKTCNT_bit) & MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT_CNT_MASK;
	ovf = v & MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT_CNT_MASK;
	eth_printf("      sent %lu; MTL_RxQ0_Missed_Packet_Overflow_Cnt missed = %lu overflow = %lu (information: MTL_Interrupt_Status.Q0IS and DMA_Interrupt_Status.MTLIS with nothing enabled)\n",
			   (unsigned long)n, (unsigned long)missed, (unsigned long)ovf);
	cnt = is_drain(mac_idx, IS_OVF_FRAMES, IS_BIG_LEN);
	eth_printf("      read back: %lu/%lu frames identical (expected %lu: the overflowed one is gone)\n",
			   (unsigned long)cnt, (unsigned long)IS_OVF_FRAMES, (unsigned long)(IS_OVF_FRAMES - 1U));
	extra = (n == IS_OVF_FRAMES) && (cnt == IS_OVF_FRAMES - 1U) && ((s.mtlqicsr & MTL_Q0_INTERRUPT_CONTROL_STATUS_RXOVFIS) != 0U) && (ovf >= 1U);
	ok += is_check("F", &s, DMA_CH0_STATUS_RBU, DMA_CH0_STATUS_AIS | DMA_CH0_STATUS_NIS | DMA_CH0_STATUS_FBE, extra);
	is_clear(mac_base);
	v = AX_BASE_READ_REG32(mac_base, R_MTL_Q0_INTERRUPT_CONTROL_STATUS);
	eth_printf("      MTL_Q0_Interrupt_Control_Status after write 1 to RXOVFIS: 0x%08lX (RXOVFIS %d)\n", (unsigned long)v, (v & MTL_Q0_INTERRUPT_CONTROL_STATUS_RXOVFIS) != 0);

	/* G: receive watchdog */
	is_title("G", "MAC_WD_JB_Timeout PWE = 1, WTO = 2 KB, then a 2100-byte frame: the receiver cuts it (RDES3 RWT), DMA_CH0_Status.RWT, MAC_Rx_Tx_Status.RWT");
	is_clear(mac_base);
	AX_BASE_WRITE_REG32(mac_base, R_MAC_WD_JB_TIMEOUT, MAC_WD_JB_TIMEOUT_PWE | (MAC_WD_JB_TIMEOUT_WTO_2KB << MAC_WD_JB_TIMEOUT_WTO_bit));	// PWE [8]: use WTO; WTO [3:0]: 2 KB
	extra = is_send(IS_RWT_LEN);
	rx = is_recv(mac_idx, ETH_TEST_RX_TIMEOUT_US, &rdes3);
	udelay(IS_SETTLE_US);
	is_read(mac_base, &s);
	v = AX_BASE_READ_REG32(mac_base, R_MAC_RX_TX_STATUS);			// read last: clears RWT [8] and so MAC_Interrupt_Status.RXSTSIS
	eth_printf("      RX = %d (-3 = error summary), RDES3 = 0x%08lX (ES %d RWT %d GP %d PL %lu); MAC_Rx_Tx_Status = 0x%08lX (RWT %d)\n",
			   rx, (unsigned long)rdes3, (rdes3 & RDES3_ES) != 0, (rdes3 & RDES3_RWT) != 0, (rdes3 & RDES3_GP) != 0,
			   (unsigned long)(rdes3 & RDES3_PL_MASK), (unsigned long)v, (v & MAC_RX_TX_STATUS_RWT) != 0);
	extra = extra && (rx == -3) && ((rdes3 & RDES3_RWT) != 0U) && ((v & MAC_RX_TX_STATUS_RWT) != 0U);
	ok += is_check("G", &s, DMA_CH0_STATUS_RWT, DMA_CH0_STATUS_AIS | DMA_CH0_STATUS_NIS, extra);
	/* how does RWT clear? p.3094 lists it as rw, every other status bit as rc_w1 */
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_STATUS, DMA_CH0_STATUS_RWT);
	v1 = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_STATUS);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_STATUS, 0U);
	v2 = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_STATUS);
	eth_printf("      RWT clearing: write 1 -> 0x%08lX (RWT %d), then write 0 -> 0x%08lX (RWT %d): %s\n",
			   (unsigned long)v1, (v1 & DMA_CH0_STATUS_RWT) != 0, (unsigned long)v2, (v2 & DMA_CH0_STATUS_RWT) != 0,
			   ((v1 & DMA_CH0_STATUS_RWT) == 0U) ? "rc_w1 like the others" : ((v2 & DMA_CH0_STATUS_RWT) == 0U) ? "rw: cleared by writing 0" : "stuck");
	AX_BASE_WRITE_REG32(mac_base, R_MAC_WD_JB_TIMEOUT, 0U);			// watchdog back to the default (JE / WD of MAC_Configuration decide)
	eth_rx_flush(mac_idx);
	is_clear(mac_base);

	/* H: stop commands */
	ok += is_stop_case(mac_idx, mac_base, "H1", R_DMA_CH0_TX_CONTROL, DMA_CH0_TX_CONTROL_ST, DMA_CH0_STATUS_TPS, DMA_DEBUG_STATUS0_TPS0_bit,
					   "DMA_CH0_Tx_Control.ST = 0: TPS = 1 (transmit process stopped, an abnormal-group bit), TPS0 = 0; then ST = 1 and a frame");
	ok += is_stop_case(mac_idx, mac_base, "H2", R_DMA_CH0_RX_CONTROL, DMA_CH0_RX_CONTROL_SR, DMA_CH0_STATUS_RPS, DMA_DEBUG_STATUS0_RPS0_bit,
					   "DMA_CH0_Rx_Control.SR = 0: RPS = 1 (receive process stopped), RPS0 = 0; then SR = 1 and a frame");

	/* I: fatal bus error, last case */
	is_title("I", "TX descriptor with its buffer at 0x20000000 (DTCM): the DMA cannot read it -> FBE, TEB, TPS; the DMA stays dead");
	eth_printf("      (last case: after a bus error the channel disables all bus accesses, p.3095; the next test starts with eth_reset_mac)\n");
	is_clear(mac_base);
	idx = eth_tx_head;
	if (tx_desc[idx].des3 & TDES3_OWN) {
		eth_printf("      tx_desc[%lu] still owned by the DMA: case skipped\n", (unsigned long)idx);
		extra = 0;
		is_read(mac_base, &s);
	} else {
		tx_desc[idx].des0 = IS_DTCM_ADDR;					// TDES0: buffer 1 address, out of reach of the Ethernet DMA
		tx_desc[idx].des1 = 0;
		tx_desc[idx].des2 = IS_FRAME_LEN & TDES2_B1L_MASK;	// TDES2: B1L [13:0]
		ETH_DSB();
		tx_desc[idx].des3 = TDES3_OWN | TDES3_FD | TDES3_LD | (IS_FRAME_LEN & TDES3_FL_MASK);	// OWN [31], FD [29], LD [28], FL [14:0]
		ETH_DSB();
		eth_tx_head = (idx + 1) & (MAC_100M_TX_DESC_CNT - 1);
		AX_BASE_WRITE_REG32(mac_base, R_DMA_CH0_TXDESC_TAIL_POINTER, (uint32_t)(uintptr_t)&tx_desc[eth_tx_head]);
		n = IS_FBE_WAIT_US;
		while (((AX_BASE_READ_REG32(mac_base, R_DMA_CH0_STATUS) & DMA_CH0_STATUS_FBE) == 0U) && n > 0U) {
			udelay(1);
			n--;
		}
		is_read(mac_base, &s);
		v = (s.dmacsr >> DMA_CH0_STATUS_TEB_bit) & DMA_CH0_STATUS_EB_MASK;
		eth_printf("      FBE after %lu us; TEB = %lu: data transfer by TX DMA %d, descriptor access %d (0 = data buffer), read %d; TDES3 = 0x%08lX (OWN %d)\n",
				   (unsigned long)(IS_FBE_WAIT_US - n), (unsigned long)v, (v & IS_TEB_TXDATA) != 0, (v & IS_TEB_DESC) != 0,
				   (v & IS_TEB_READ) != 0, (unsigned long)tx_desc[idx].des3, (tx_desc[idx].des3 & TDES3_OWN) != 0);
		extra = ((v & IS_TEB_READ) != 0U) && ((v & IS_TEB_DESC) == 0U)
			 && (((s.dmadsr >> DMA_DEBUG_STATUS0_TPS0_bit) & DMA_DEBUG_STATUS0_PS_MASK) == DMA_DEBUG_STATUS0_PS_STOPPED);
	}
	ok += is_check("I", &s, DMA_CH0_STATUS_FBE | DMA_CH0_STATUS_TPS, DMA_CH0_STATUS_AIS | DMA_CH0_STATUS_NIS, extra);
	eth_rx_ioc = 0;		// plain descriptors again for the other tests

	eth_printf("\nCases: %s\n", is_summary);
	eth_printf("Result: %lu/%d cases as expected.\n", (unsigned long)ok, IS_N_CASES);
	eth_print_mac_counters(mac_idx);
	return (ok == IS_N_CASES) ? 1 : 0;
}
