#include <stdint.h>
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_flow_utils.h"
#include "eth_printf.h"

/*
 * FLOW log functions (declared in eth_flow_utils.h).
 * They only READ registers and descriptors to print them; nothing is written to the hardware.
 * Bit positions checked against RM0399 Rev 4 chapter 61.
 */

uint8_t eth_flow_trace;

/* One bit field: name, high bit, low bit */
struct flow_field { const char *name; uint8_t hi; uint8_t lo; };
#define FIELD_CNT(a) (sizeof(a) / sizeof((a)[0]))

static const struct flow_field F_DMA_MODE[]     = {{"SWR",0,0},{"DA",1,1},{"TXPR",11,11},{"PR",14,12},{"INTM",17,16}};
static const struct flow_field F_DMA_SYSBUS_MODE[]   = {{"FB",0,0},{"AAL",12,12},{"MB",14,14},{"RB",15,15}};
static const struct flow_field F_DMA_CH0_CONTROL[]    = {{"MSS",13,0},{"PBLX8",16,16},{"DSL",20,18}};
static const struct flow_field F_DMA_CH0_TX_CONTROL[]  = {{"ST",0,0},{"OSF",4,4},{"TSE",12,12},{"TXPBL",21,16}};
static const struct flow_field F_DMA_CH0_RX_CONTROL[]  = {{"SR",0,0},{"RBSZ",14,1},{"RXPBL",21,16},{"RPF",31,31}};
static const struct flow_field F_DMA_CH0_STATUS[]    = {{"TI",0,0},{"TPS",1,1},{"TBU",2,2},{"RI",6,6},{"RBU",7,7},{"RPS",8,8},
                                                {"RWT",9,9},{"ETI",10,10},{"ERI",11,11},{"FBE",12,12},{"CDE",13,13},
                                                {"AIS",14,14},{"NIS",15,15},{"TEB",18,16},{"REB",21,19}};
static const struct flow_field F_MTL_TXQ0_OPERATION_MODE[] = {{"FTQ",0,0},{"TSF",1,1},{"TXQEN",3,2},{"TTC",6,4},{"TQS",18,16}};
static const struct flow_field F_MTL_RXQ0_OPERATION_MODE[] = {{"RTC",1,0},{"FUP",3,3},{"FEP",4,4},{"RSF",5,5},{"DIS_TCP_EF",6,6},{"RQS",22,20}};
static const struct flow_field F_MAC_CONFIGURATION[]     = {{"RE",0,0},{"TE",1,1},{"PRELEN",3,2},{"DO",10,10},{"ECRSFD",11,11},{"LM",12,12},
                                                {"DM",13,13},{"FES",14,14},{"JE",16,16},{"JD",17,17},{"WD",19,19},
                                                {"ACS",20,20},{"CST",21,21},{"S2KP",22,22},{"GPSLCE",23,23},{"IPG",26,24},{"IPC",27,27},{"SARC",30,28}};
static const struct flow_field F_MAC_PACKET_FILTER[]    = {{"PR",0,0},{"HUC",1,1},{"HMC",2,2},{"DAIF",3,3},{"PM",4,4},{"DBF",5,5},
                                                {"PCF",7,6},{"SAIF",8,8},{"SAF",9,9},{"HPF",10,10},{"RA",31,31}};
static const struct flow_field F_MAC_WD_JB_TIMEOUT[]    = {{"WTO",3,0},{"PWE",8,8}};
static const struct flow_field F_MAC_TIMESTAMP_CONTROL[]   = {{"TSENA",0,0},{"TSCFUPDT",1,1},{"TSINIT",2,2},{"TSUPDT",3,3},
                                                {"TSADDREG",5,5},{"TSENALL",8,8},{"TSCTRLSSR",9,9},{"TXTSSTSM",24,24}};
static const struct flow_field F_MAC_VLAN_TAG[]    = {{"VL",15,0},{"ETV",16,16},{"VTIM",17,17},{"ESVL",18,18},{"ERSVLM",19,19},
                                                {"DOVLTC",20,20},{"EVLS",22,21},{"EVLRXS",24,24},{"VTHM",25,25},
                                                {"EDVLP",26,26},{"ERIVLT",27,27},{"EIVLS",29,28},{"EIVLRXS",31,31}};
static const struct flow_field F_MAC_VLAN_INCL[]    = {{"VLT",15,0},{"VLC",17,16},{"VLP",18,18},{"CSVL",19,19},{"VLTI",20,20}};
static const struct flow_field F_TDES3_RD[]  = {{"FL",14,0},{"CIC",17,16},{"SAIC",25,23},{"CPC",27,26},{"LD",28,28},{"FD",29,29},{"CTXT",30,30},{"OWN",31,31}};
static const struct flow_field F_TDES3_WB[]  = {{"IHE",0,0},{"DB",1,1},{"UF",2,2},{"ED",3,3},{"CC",7,4},{"EC",8,8},{"LC",9,9},
                                                {"NC",10,10},{"LoC",11,11},{"PCE",12,12},{"FF",13,13},{"JT",14,14},{"ES",15,15},
                                                {"TTSS",17,17},{"LD",28,28},{"FD",29,29},{"CTXT",30,30},{"OWN",31,31}};
static const struct flow_field F_RDES3_RD[]  = {{"BUF1V",24,24},{"BUF2V",25,25},{"IOC",30,30},{"OWN",31,31}};
static const struct flow_field F_RDES3_WB[]  = {{"PL",14,0},{"ES",15,15},{"LT",18,16},{"DE",19,19},{"RE",20,20},{"OE",21,21},
                                                {"RWT",22,22},{"GP",23,23},{"CE",24,24},{"RS0V",25,25},{"RS1V",26,26},
                                                {"RS2V",27,27},{"LD",28,28},{"FD",29,29},{"CTXT",30,30},{"OWN",31,31}};

/* enum flow_fields_id -> table */
static const struct {
	const struct flow_field *f;
	uint32_t n;
} flow_tables[] = {
	[FF_NONE]      = {0, 0},
	[FF_DMA_MODE]     = {F_DMA_MODE, FIELD_CNT(F_DMA_MODE)},
	[FF_DMA_SYSBUS_MODE]   = {F_DMA_SYSBUS_MODE, FIELD_CNT(F_DMA_SYSBUS_MODE)},
	[FF_DMA_CH0_CONTROL]    = {F_DMA_CH0_CONTROL, FIELD_CNT(F_DMA_CH0_CONTROL)},
	[FF_DMA_CH0_TX_CONTROL]  = {F_DMA_CH0_TX_CONTROL, FIELD_CNT(F_DMA_CH0_TX_CONTROL)},
	[FF_DMA_CH0_RX_CONTROL]  = {F_DMA_CH0_RX_CONTROL, FIELD_CNT(F_DMA_CH0_RX_CONTROL)},
	[FF_DMA_CH0_STATUS]    = {F_DMA_CH0_STATUS, FIELD_CNT(F_DMA_CH0_STATUS)},
	[FF_MTL_TXQ0_OPERATION_MODE] = {F_MTL_TXQ0_OPERATION_MODE, FIELD_CNT(F_MTL_TXQ0_OPERATION_MODE)},
	[FF_MTL_RXQ0_OPERATION_MODE] = {F_MTL_RXQ0_OPERATION_MODE, FIELD_CNT(F_MTL_RXQ0_OPERATION_MODE)},
	[FF_MAC_CONFIGURATION]     = {F_MAC_CONFIGURATION, FIELD_CNT(F_MAC_CONFIGURATION)},
	[FF_MAC_PACKET_FILTER]    = {F_MAC_PACKET_FILTER, FIELD_CNT(F_MAC_PACKET_FILTER)},
	[FF_MAC_WD_JB_TIMEOUT]    = {F_MAC_WD_JB_TIMEOUT, FIELD_CNT(F_MAC_WD_JB_TIMEOUT)},
	[FF_MAC_TIMESTAMP_CONTROL]   = {F_MAC_TIMESTAMP_CONTROL, FIELD_CNT(F_MAC_TIMESTAMP_CONTROL)},
	[FF_MAC_VLAN_TAG]    = {F_MAC_VLAN_TAG, FIELD_CNT(F_MAC_VLAN_TAG)},
	[FF_MAC_VLAN_INCL]    = {F_MAC_VLAN_INCL, FIELD_CNT(F_MAC_VLAN_INCL)},
	[FF_MAC_INNER_VLAN_INCL]   = {F_MAC_VLAN_INCL, FIELD_CNT(F_MAC_VLAN_INCL)},	// same layout as MAC_VLAN_Incl (p.3127)
	[FF_TDES3_RD]  = {F_TDES3_RD, FIELD_CNT(F_TDES3_RD)},
	[FF_TDES3_WB]  = {F_TDES3_WB, FIELD_CNT(F_TDES3_WB)},
	[FF_RDES3_RD]  = {F_RDES3_RD, FIELD_CNT(F_RDES3_RD)},
	[FF_RDES3_WB]  = {F_RDES3_WB, FIELD_CNT(F_RDES3_WB)},
};

/* DMA process state, DMA_Debug_Status0.TPS0 / RPS0 (p.3078) */
static const char *const tps_name[8] = {
	"Stopped", "Running: fetching TX descriptor", "Running: waiting for status", "Running: RAM -> TX FIFO",
	"Timestamp write", "(reserved)", "Suspended: no descriptor or underflow", "Running: closing TX descriptor"};
static const char *const rps_name[8] = {
	"Stopped", "Running: fetching RX descriptor", "(reserved)", "Running: waiting for packet",
	"Suspended: no RX descriptor", "Running: closing RX descriptor", "Timestamp write", "Running: RX FIFO -> RAM"};
/* MTL queue state, MTL_TxQ0_Debug (p.3103) / MTL_RxQ0_Debug (p.3109) */
static const char *const trcsts_name[4] = {"Idle", "Read (data to MAC TX)", "Waiting for TX status", "Flushing"};
static const char *const rxqsts_name[4] = {"empty", "below threshold", "above threshold", "full"};
static const char *const rrcsts_name[4] = {"Idle", "reading data", "reading status", "flushing"};
/* MAC transmitter state, MAC_Debug.TFCSTS (p.3142) */
static const char *const tfcsts_name[4] = {"Idle", "waiting status/IPG/backoff", "sending Pause", "sending packet"};

/* Bit fields of v, e.g. "        -> ST=1 OSF=1 TSE=0 TXPBL=8" */
void flow_fields(enum flow_fields_id f, uint32_t v)
{
	if (FLOW_LOG < 2 || f == FF_NONE) {
		return;
	}
	FLOW2("        ->");
	for (uint32_t i = 0; i < flow_tables[f].n; i++) {
		const struct flow_field *fd = &flow_tables[f].f[i];
		// field width = hi - lo + 1; mask of that width, then shift the field down to bit 0
		uint32_t w = (uint32_t)fd->hi - fd->lo + 1;
		uint32_t mask = (w >= 32) ? 0xFFFFFFFFU : ((1UL << w) - 1);
		FLOW2(" %s=%lu", fd->name, (unsigned long)((v >> fd->lo) & mask));
	}
	FLOW2("\n");
}
/* Register name, absolute address (base + offset) and the value READ BACK from the hardware.
 * Reading back after a write shows what the hardware really keeps
 * (e.g. a self-clearing bit such as DMA_Mode.SWR reads 0). */
void flow_reg(const char *name, uint32_t base, uint32_t offset, enum flow_fields_id f)
{
	uint32_t v;

	if (FLOW_LOG < 1) {
		return;
	}
	v = AX_BASE_READ_REG32(base, offset);
	FLOW("      %-28s @0x%08lX = 0x%08lX\n", name, (unsigned long)(base + offset), (unsigned long)v);
	flow_fields(f, v);
}
/* flow_reg, only at FLOW_LOG 2 */
void flow_reg2(const char *name, uint32_t base, uint32_t offset, enum flow_fields_id f)
{
	if (FLOW_LOG < 2) {
		return;
	}
	flow_reg(name, base, offset, f);
}
/* flow_reg after a write: at FLOW_LOG 2 also prints the value written when the read-back differs
 * (read-only or self-clearing bits, e.g. MAC_Configuration bit 15, MAC_Address0_High.AE) */
void flow_regw(const char *name, uint32_t base, uint32_t offset, uint32_t written, enum flow_fields_id f)
{
	uint32_t v;

	if (FLOW_LOG < 1) {
		return;
	}
	v = AX_BASE_READ_REG32(base, offset);
	FLOW("      %-28s @0x%08lX = 0x%08lX", name, (unsigned long)(base + offset), (unsigned long)v);
	if (v != written) {
		FLOW2("   (written 0x%08lX)", (unsigned long)written);
	}
	FLOW("\n");
	flow_fields(f, v);
}
/* Title of a step + time since boot */
void flow_step(const char *tag, const char *title)
{
	FLOW2("\n================ %s %s   (t = %lu ms) ================\n", tag, title, (unsigned long)eth_get_ms());
}
/* "tx_desc[i]" when addr is a descriptor of the ring, "(out of ring)" else */
static void flow_desc_name(const char *ring, uint32_t addr, uint32_t base, uint32_t cnt)
{
	if (addr >= base && addr < base + cnt * 16 && ((addr - base) % 16) == 0) {
		FLOW2("%s[%lu]", ring, (unsigned long)((addr - base) / 16));
	} else {
		FLOW2("(out of ring)");
	}
}
/* DMA: process state (DMA_Debug_Status0 p.3077), current descriptors (p.3092), DMA_CH0_Status (p.3093) */
void flow_dma_state(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t dsr, tps, rps, ctx, crx;

	if (FLOW_LOG < 2) {
		return;
	}
	dsr = AX_BASE_READ_REG32(mac_base, R_DMA_DEBUG_STATUS0);
	tps = (dsr >> DMA_DEBUG_STATUS0_TPS0_bit) & 0x7;
	rps = (dsr >> DMA_DEBUG_STATUS0_RPS0_bit) & 0x7;
	FLOW2("      DMA TX: TPS0=%lu %-38s | DMA RX: RPS0=%lu %s\n",
		  (unsigned long)tps, tps_name[tps], (unsigned long)rps, rps_name[rps]);
	ctx = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_CURRENT_APP_TXDESC);
	crx = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_CURRENT_APP_RXDESC);
	FLOW2("      DMA is at: TX 0x%08lX = ", (unsigned long)ctx);
	flow_desc_name("tx_desc", ctx, (uint32_t)(uintptr_t)tx_desc, MAC_100M_TX_DESC_CNT);
	FLOW2(" | RX 0x%08lX = ", (unsigned long)crx);
	flow_desc_name("rx_desc", crx, (uint32_t)(uintptr_t)rx_desc, MAC_100M_RX_DESC_CNT);
	FLOW2("\n");
	flow_reg("DMA_CH0_Status (status)", mac_base, R_DMA_CH0_STATUS, FF_DMA_CH0_STATUS);
}
/* The 2 MTL FIFOs */
void flow_mtl_state(uint8_t mac_idx)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t tq, rq;

	if (FLOW_LOG < 2) {
		return;
	}
	tq = AX_BASE_READ_REG32(mac_base, R_MTL_TXQ0_DEBUG);
	rq = AX_BASE_READ_REG32(mac_base, R_MTL_RXQ0_DEBUG);
	FLOW2("      MTL TX FIFO: %s, TRCSTS=%lu %s\n", (tq & MTL_TXQ0_DEBUG_TXQSTS) ? "not empty" : "empty",
		  (unsigned long)((tq >> MTL_TXQ0_DEBUG_TRCSTS_bit) & 3), trcsts_name[(tq >> MTL_TXQ0_DEBUG_TRCSTS_bit) & 3]);
	FLOW2("      MTL RX FIFO: PRXQ=%lu packets, RXQSTS=%lu %s, RRCSTS=%lu %s\n",
		  (unsigned long)((rq >> MTL_RXQ0_DEBUG_PRXQ_bit) & 0x3FFF),
		  (unsigned long)((rq >> MTL_RXQ0_DEBUG_RXQSTS_bit) & 3), rxqsts_name[(rq >> MTL_RXQ0_DEBUG_RXQSTS_bit) & 3],
		  (unsigned long)((rq >> MTL_RXQ0_DEBUG_RRCSTS_bit) & 3), rrcsts_name[(rq >> MTL_RXQ0_DEBUG_RRCSTS_bit) & 3]);
}
/* MAC transmitter / receiver (MAC_Debug p.3141) */
void flow_mac_state(uint8_t mac_idx)
{
	uint32_t d;

	if (FLOW_LOG < 2) {
		return;
	}
	d = AX_BASE_READ_REG32(MAC_BASE_ADRS(mac_idx), R_MAC_DEBUG);
	FLOW2("      MAC: TX engine %s, TFCSTS=%lu %s | RX engine %s\n",
		  (d & MAC_DEBUG_TPESTS) ? "active" : "idle", (unsigned long)((d >> MAC_DEBUG_TFCSTS_bit) & 3),
		  tfcsts_name[(d >> MAC_DEBUG_TFCSTS_bit) & 3], (d & MAC_DEBUG_RPESTS) ? "active" : "idle");
}
/* MMC counters (read only; reset-on-read is off, so reading does not clear them) */
void flow_mmc(uint8_t mac_idx)
{
	if (FLOW_LOG < 2) {
		return;
	}
	FLOW2("      MMC: TX good=%lu  TX 1-collision=%lu  TX multi-collision=%lu | RX unicast good=%lu  RX CRC err=%lu  RX align err=%lu\n",
		  (unsigned long)AX_READ_MAC_REG32(mac_idx, MMC_STATS_TX_PKT_GOOD),
		  (unsigned long)AX_READ_MAC_REG32(mac_idx, MMC_STATS_TX_SINGLE_COL_GOOD),
		  (unsigned long)AX_READ_MAC_REG32(mac_idx, MMC_STATS_TX_MULTI_COL_GOOD),
		  (unsigned long)AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_UNICAST_GOOD),
		  (unsigned long)AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_CRC_ERR),
		  (unsigned long)AX_READ_MAC_REG32(mac_idx, MMC_STATS_RX_ALIGN_ERR));
}
/* Both rings: 4 words per descriptor, owner (OWN), tx/rx head, tail, current DMA descriptor */
void flow_rings(uint8_t mac_idx, const char *why)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);
	uint32_t ttail, rtail, tcur, rcur, a, i;

	if (FLOW_LOG < 2) {
		return;
	}
	ttail = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_TXDESC_TAIL_POINTER);
	rtail = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_RXDESC_TAIL_POINTER);
	tcur = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_CURRENT_APP_TXDESC);
	rcur = AX_BASE_READ_REG32(mac_base, R_DMA_CH0_CURRENT_APP_RXDESC);
	FLOW2("      --- Descriptor rings (%s) ---       DES0     DES1     DES2     DES3\n", why);
	for (i = 0; i < MAC_100M_TX_DESC_CNT; i++) {
		a = (uint32_t)(uintptr_t)&tx_desc[i];
		FLOW2("      tx_desc[%lu] @0x%08lX:  %08lX %08lX %08lX %08lX  %s%s%s%s\n", (unsigned long)i, (unsigned long)a,
			  (unsigned long)tx_desc[i].des0, (unsigned long)tx_desc[i].des1, (unsigned long)tx_desc[i].des2,
			  (unsigned long)tx_desc[i].des3, (tx_desc[i].des3 & TDES3_OWN) ? "[DMA]" : "[CPU]",
			  (i == eth_tx_head) ? " <- tx_head" : "", (a == ttail) ? " <- TX tail" : "", (a == tcur) ? " <- DMA" : "");
	}
	for (i = 0; i < MAC_100M_RX_DESC_CNT; i++) {
		a = (uint32_t)(uintptr_t)&rx_desc[i];
		FLOW2("      rx_desc[%lu] @0x%08lX:  %08lX %08lX %08lX %08lX  %s%s%s%s\n", (unsigned long)i, (unsigned long)a,
			  (unsigned long)rx_desc[i].des0, (unsigned long)rx_desc[i].des1, (unsigned long)rx_desc[i].des2,
			  (unsigned long)rx_desc[i].des3, (rx_desc[i].des3 & RDES3_OWN) ? "[DMA]" : "[CPU]",
			  (i == eth_rx_head) ? " <- rx_head" : "", (a == rtail) ? " <- RX tail" : "", (a == rcur) ? " <- DMA" : "");
	}
}
/* Hex dump of an Ethernet frame, field by field: DA | SA | EtherType | payload | pad */
void flow_frame_dump(const char *title, const uint8_t *p, uint32_t len, uint32_t payload_len)
{
	static const char *const name[5] = {"Dest MAC", "Src MAC", "EtherType", "Payload", "Pad (MAC)"};
	uint32_t cut[6];
	uint32_t s, i;

	if (FLOW_LOG < 2) {
		return;
	}
	if (payload_len > len) {
		payload_len = len;
	}
	cut[0] = 0; cut[1] = 6; cut[2] = 12; cut[3] = 14; cut[4] = 14 + payload_len; cut[5] = len;
	FLOW2("      %s: %lu bytes @0x%08lX\n", title, (unsigned long)len, (unsigned long)(uintptr_t)p);
	for (s = 0; s < 5; s++) {
		uint32_t a = cut[s];
		uint32_t b = (cut[s + 1] < len) ? cut[s + 1] : len;
		if (a >= b) {
			continue;
		}
		FLOW2("        [%4lu..%4lu] %-9s:", (unsigned long)a, (unsigned long)(b - 1), name[s]);
		for (i = a; i < b; i++) {
			// long field (payload, pad): new line every 16 bytes
			if (i > a && ((i - a) % 16) == 0) {
				FLOW2("\n%31s", "");
			}
			FLOW2(" %02X", (unsigned int)p[i]);
		}
		FLOW2("\n");
	}
}
