#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "eth_registers.h"
#include "eth_utils.h"
#include "mac_defines.h"
#include "eth_functions_utils.h"
#include "eth_burst_utils.h"
#include "eth_traffic_test.h"
#include "eth_printf.h"
#include "eth_flow_utils.h"

/*
 * DMA burst test (eth_run_test_ops = 2): what DMA_SysBus_Mode.FB and AAL change (RM0399 p.3076),
 * through the MAC loopback.
 * For each mode of BT_MODES[] and each frame length of BT_LENS[]:
 *   fill the RX buffer with BT_FILL -> send 1 frame -> receive it -> compare it with the frame sent
 *   -> look at the bytes AFTER the frame end (RDES3.PL) in the RX buffer: did the DMA write there?
 *
 *   FB = 1   fixed bursts SINGLE / INCR4 / INCR8 / INCR16. If the frame ends inside a fixed burst,
 *            the DMA finishes the burst with placeholder beats (Synopsys databook §2.2.1.5 p.67,
 *            Figure 2-11): bytes after PL can change.
 *   FB = 0   INCR bursts of unspecified length (MB is read-only 0 on STM32H755): the burst stops at
 *            the end of the data, so only the rest of the last 32-bit word (0-3 bytes) can change.
 *   AAL = 1  bursts start on an address aligned to the burst size (databook §2.2.1.3 p.67).
 *
 * PASS = every frame comes back the same in every mode. The "after" column only shows what the
 * hardware did; the summary at the end compares it with the databook.
 * Each mode starts with eth_reset_mac() + init, so DMA_SysBus_Mode is written while the DMA is stopped
 * (RM0399 §61.9.1 order): the [4]..[8] FLOW lines come once per mode, the tables at the end.
 */

#define BT_FILL		0xAA	// RX buffer filled with this before each frame (the payload never uses it)
#define BT_SHOW		16		// bytes after PL printed in the table
#define BT_WORD		4		// AHB data bus of the Ethernet DMA: 32 bits = 4 bytes per beat

/* Modes: value written to DMA_SysBus_Mode (FB = bit 0, AAL = bit 12). Add a line to try another one */
static const struct burst_mode {
	const char *name;
	uint32_t    sbmr;
} BT_MODES[] = {
	{"FB=1 AAL=1 (default)", DMA_SYSBUS_MODE_FB | DMA_SYSBUS_MODE_AAL},
	{"FB=0 AAL=1",           DMA_SYSBUS_MODE_AAL},
	{"FB=1 AAL=0",           DMA_SYSBUS_MODE_FB},
	{"FB=0 AAL=0",           0},
};
#define BT_N_MODES (sizeof BT_MODES / sizeof BT_MODES[0])

/* Frame lengths in bytes (no FCS, >= 60 so the MAC adds no pad): 60..63 = the 4 positions of the
 * end in a 32-bit word; 127/128/129 around 32 beats; 255/256/257 around one DMA request
 * (PBL 8 x PBLX8 = 64 beats = 256 bytes). Frames > 2044 bytes are cut by the RX watchdog (WTO = 0) */
static const uint16_t BT_LENS[] = {60, 61, 62, 63, 64, 100, 127, 128, 129, 255, 256, 257, 1001, 1514};
#define BT_N_LENS (sizeof BT_LENS / sizeof BT_LENS[0])

/* Result of one frame */
struct burst_result {
	int      tx;              // egress_packet() return value
	int      rx;              // ingress_packet_status() return value: length, or < 0 = error
	uint32_t pl;              // RDES3.PL: frame length written by the DMA
	uint32_t end;             // address of the first byte after the frame in the RX buffer
	uint32_t after;           // bytes from PL to the last byte != BT_FILL (0 = nothing written after PL)
	int      same;            // 1 = frame received = frame sent
	uint8_t  show[BT_SHOW];   // first BT_SHOW bytes after PL
};

/* Reset + init, then DMA_SysBus_Mode = sbmr: mac_init_100m() wrote the default, the DMA is still stopped */
static uint8_t burst_mode_init(uint8_t mac_idx, uint32_t sbmr)
{
	uint32_t mac_base = MAC_BASE_ADRS(mac_idx);

	if (eth_reset_mac(mac_idx) == 0) {
		return 0;
	}
	mac_init_100m(mac_idx);
	AX_BASE_WRITE_REG32(mac_base, R_DMA_SYSBUS_MODE, sbmr);
	FLOW("[5] Burst test: DMA_SysBus_Mode written again before the DMA starts\n");
	flow_regw("DMA_SysBus_Mode (burst test)", mac_base, R_DMA_SYSBUS_MODE, sbmr, FF_DMA_SYSBUS_MODE);
	init_mac_internal_lb(mac_idx);
	mac_start(mac_idx);
	return 1;
}
/* One frame: fill the RX buffer, send, receive, compare, look at the bytes after PL */
static void burst_run_frame(uint8_t mac_idx, uint16_t len, struct burst_result *r)
{
	static uint8_t frame[MAC_100M_BUF_SIZE];
	static uint8_t rxbuf[MAC_100M_BUF_SIZE];
	unsigned char da[ETH_TEST_MAC_SIZE] = ETH_TEST_DEST_MAC;
	unsigned char sa[ETH_TEST_MAC_SIZE] = ETH_TEST_SRC_MAC;
	/* Buffer of the next RX descriptor: RDES0 (read format) holds its address, written by
	 * mac_desc_init() or the last ingress_packet_status(). Read it now: the write-back overwrites RDES0 */
	uint8_t *dma_buf = (uint8_t *)(uintptr_t)rx_desc[eth_rx_head].des0;
	uint32_t rdes3 = 0;
	uint32_t i;

	memset(r, 0, sizeof(*r));
	/* nothing comes in before egress_packet(), so the DMA does not write this buffer now */
	memset(dma_buf, BT_FILL, MAC_100M_BUF_SIZE);
	ETH_DSB();	// the fill is in RAM before the frame is sent

	// Frame: DA | SA | EtherType | payload 0x10..0x6F (never BT_FILL)
	memcpy(frame, da, ETH_TEST_MAC_SIZE);
	memcpy(frame + ETH_TEST_MAC_SIZE, sa, ETH_TEST_MAC_SIZE);
	frame[12] = (ETH_TEST_ETHER_TYPE >> 8) & 0xFF;
	frame[13] = ETH_TEST_ETHER_TYPE & 0xFF;
	for (i = ETH_TEST_HEADER_SIZE; i < len; i++) {
		frame[i] = (uint8_t)(0x10 + (i % 0x60));
	}

	r->tx = egress_packet(frame, len);
	if (r->tx != len) {
		return;
	}
	r->rx = ingress_packet_status(mac_idx, rxbuf, sizeof(rxbuf), ETH_TEST_RX_TIMEOUT_US, NULL, &rdes3);
	if (r->rx < 0) {
		return;
	}
	r->pl = rdes3 & RDES3_PL_MASK;
	r->same = (r->rx == len) && (memcmp(rxbuf, frame, len) == 0);
	r->end = (uint32_t)(uintptr_t)(dma_buf + r->pl);

	/* The descriptor is back with the DMA but no other frame comes in: the buffer still holds
	 * what the DMA wrote. Last byte != BT_FILL after PL = how far the DMA wrote */
	for (i = r->pl; i < MAC_100M_BUF_SIZE; i++) {
		if (dma_buf[i] != BT_FILL) {
			r->after = i + 1 - r->pl;
		}
	}
	for (i = 0; i < BT_SHOW && r->pl + i < MAC_100M_BUF_SIZE; i++) {
		r->show[i] = dma_buf[r->pl + i];
	}
}
/* Table of one mode */
static void burst_print_mode(const struct burst_mode *m, const struct burst_result res[])
{
	eth_printf("\n%s: DMA_SysBus_Mode = 0x%08lX\n", m->name, (unsigned long)m->sbmr);
	eth_printf("   len    PL  frame end   end%%64  data | after | first %d bytes after PL\n", BT_SHOW);
	for (uint32_t k = 0; k < BT_N_LENS; ++k) {
		const struct burst_result *r = &res[k];

		eth_printf("  %4u ", (unsigned int)BT_LENS[k]);
		if (r->tx != BT_LENS[k]) {
			eth_printf("TX error %d\n", r->tx);
			continue;
		}
		if (r->rx < 0) {
			eth_printf("RX error %d\n", r->rx);
			continue;
		}
		eth_printf("%5lu  0x%08lX  %2lu      %-4s | %5lu | ",(unsigned long)r->pl, (unsigned long)r->end,
				   (unsigned long)(r->end & 63U), r->same ? "SAME" : "DIFF", (unsigned long)r->after);
		for (uint32_t i = 0; i < BT_SHOW; ++i) {
			eth_printf("%02X ", (unsigned int)r->show[i]);
		}
		eth_printf("\n");
	}
}
/* Every table, then one line per mode and the comparison with the databook */
static void burst_print_results(struct burst_result res[][BT_N_LENS])
{
	uint32_t fb0_bad = 0, fb1_seen = 0;

	eth_printf("\n================ DMA BURST TEST (DMA_SysBus_Mode.FB / AAL, RM0399 p.3076) ================\n");
	eth_printf("Before each frame the RX buffer is filled with %02X. After RX:\n", BT_FILL);
	eth_printf("  data   = frame received compared with the frame sent (SAME / DIFF)\n");
	eth_printf("  after  = bytes after the frame end (PL) written by the DMA (%02X -> other value):\n", BT_FILL);
	eth_printf("           0-3 = rest of the last 32-bit word, more = placeholder beats of a fixed burst\n");
	eth_printf("  end%%64 = position of the frame end in a 64-byte block (INCR16 = 16 beats x 4 bytes)\n");
	for (uint32_t m = 0; m < BT_N_MODES; ++m) {
		burst_print_mode(&BT_MODES[m], res[m]);
	}

	eth_printf("\nSummary:\n");
	for (uint32_t m = 0; m < BT_N_MODES; ++m) {
		uint32_t max_after = 0, n_over = 0, n_same = 0;
		int fb = (BT_MODES[m].sbmr & DMA_SYSBUS_MODE_FB) != 0U;

		for (uint32_t k = 0; k < BT_N_LENS; ++k) {
			const struct burst_result *r = &res[m][k];
			n_same += (uint32_t)r->same;
			if (r->rx >= 0 && r->after > max_after) {
				max_after = r->after;
			}
			if (r->rx >= 0 && r->after >= BT_WORD) {
				n_over++;
			}
		}
		if (fb) {
			fb1_seen += n_over;
		} else {
			fb0_bad += n_over;
		}
		eth_printf("  %-22s data SAME %2lu/%lu, max after = %2lu, frames with after > 3: %2lu\n",
				   BT_MODES[m].name, (unsigned long)n_same, (unsigned long)BT_N_LENS,
				   (unsigned long)max_after, (unsigned long)n_over);
	}
	if (fb0_bad == 0U) {
		eth_printf("FB=0: OK, never more than the last 32-bit word after PL (INCR burst stops with the data)\n");
	} else {
		eth_printf("FB=0: %lu frame(s) with bytes written after the last word: differs from the databook\n",
				   (unsigned long)fb0_bad);
	}
	if (fb1_seen != 0U) {
		eth_printf("FB=1: placeholder beats after %lu frame(s) (databook 2.2.1.5, Figure 2-11)\n",
				   (unsigned long)fb1_seen);
	} else {
		eth_printf("FB=1: no placeholder beat with these lengths: the DMA used exact INCRx/SINGLE (databook 2.3.1 p.81)\n");
	}
}
/**
 * DMA burst test through the MAC loopback (eth_run_test_ops = 2)
 * For each mode: reset, init, DMA_SysBus_Mode = mode, enable the loopback, start DMA and MAC, send BT_LENS[]
 *
 *   Register             Bit [pos]   Value      Function
 *   -------------------- ----------- ---------- ------------------------------
 *   DMA_SysBus_Mode p.3076   FB [0]      1 / 0      fixed bursts INCR4/8/16 / INCR of any length
 *                        AAL [12]    1 / 0      bursts aligned to their size / not aligned
 *   other registers: same bits as eth_mac_internal_lb() (eth_functions.c)
 * The MAC stays in the last mode (FB=0 AAL=0); the next test starts with eth_reset_mac().
 * Return 1: every frame came back the same in every mode, 0: else
 */
uint16_t eth_mac_burst_lb(uint8_t mac_idx)
{
	static struct burst_result res[BT_N_MODES][BT_N_LENS];   // kept for the tables at the end
	uint32_t ok = 0;
	eth_printf("Start Init: Port[%d]\n", mac_idx);

	eth_flow_trace = 0;	// no [10]/[11] lines for these frames
	for (uint32_t m = 0; m < BT_N_MODES; ++m) {
		eth_printf("\n---------------- Burst mode %lu/%lu: %s ----------------\n", (unsigned long)(m + 1),
				   (unsigned long)BT_N_MODES, BT_MODES[m].name);
		if (burst_mode_init(mac_idx, BT_MODES[m].sbmr) == 0) {
			return 0;
		}
		eth_printf("Send %lu frames ", (unsigned long)BT_N_LENS);
		for (uint32_t k = 0; k < BT_N_LENS; ++k) {
			burst_run_frame(mac_idx, BT_LENS[k], &res[m][k]);
			ok += (uint32_t)res[m][k].same;
			eth_printf(".");
		}
		eth_printf("\n");
	}

	burst_print_results(res);
	eth_printf("\nMMC counters of the last mode:\n");
	eth_print_mac_counters(mac_idx);
	ETH_PRINTF_L1("Burst test finished. Frames SAME Actual/Expected %lu/%lu", (unsigned long)ok,
				  (unsigned long)(BT_N_MODES * BT_N_LENS));
	return (ok == BT_N_MODES * BT_N_LENS) ? 1 : 0;
}
