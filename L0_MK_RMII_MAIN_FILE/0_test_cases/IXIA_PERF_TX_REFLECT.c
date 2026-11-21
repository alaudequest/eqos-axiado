#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* Ixia 400 performance tests (STM32H755_Ixia_Test_Firmware_Spec.md).
 *   ID 32 TEST_IXIA_DUT_TX  : Test 2, DUT max TX rate (STM32 -> Ixia)
 *   ID 33 TEST_IXIA_REFLECT : Test 4, RFC 2544 throughput, reflect mode
 *
 * main.c includes this file AFTER the TX/RX rings are declared, brings the
 * link up with PHY auto-negotiation (MACCR.LM=0, FES/DM from the PHY), starts
 * TX/RX and calls the selected function; do not add it as a separate source.
 * All measuring is done on the Ixia side; the STM32 only keeps counters.
 * No printf inside the send / reflect loops.
 *
 * Addresses: the STM32 keeps its existing MAC 00:80:E1:00:00:00 (tell the Ixia
 * side); IPv4 / UDP values are the spec values.
 *
 * Frame (spec section 2), size = whole frame incl. 4-byte CRC, the STM32
 * builds size - 4 bytes and the MAC adds the CRC:
 *   0 Dst MAC | 6 Src MAC | 12 EtherType 0x0800 | 14 IPv4 (UDP, TTL 64)
 *   34 UDP (checksum 0) | 42 "IXTS" | 46 seq (BE, from 0) | 50 DE AD BE EF ...
 *
 * TX runs pipelined (eth_pkt_set_timeout(0, ...)): eth_egress_packet() returns
 * once the frame is queued. Each frame uses exactly one descriptor, so with
 * IXIA_NBUF = 2 x EQOS_TX_DESC_COUNT frame buffers, buffer i % IXIA_NBUF was
 * last used by frame i - IXIA_NBUF, whose descriptor slot was already reused
 * (after OWN=0) by frame i - EQOS_TX_DESC_COUNT: the buffer is free to rewrite.
 */

#define IXIA_FRAME_MAX        1514U        /* 1518 B incl. CRC */
#define IXIA_NBUF             (2U * EQOS_TX_DESC_COUNT)
#define IXIA_TX_FRAME_COUNT   1000000U     /* frames per USER press (Test 2) */
// #define IXIA_TX_FRAME_COUNT   100U     /* frames per USER press (Test 2) */
#define IXIA_FLUSH_TIMEOUT_US 100000U
#define IXIA_ETHERTYPE_IPV4   0x0800U
#define IXIA_ETHERTYPE_ARP    0x0806U
#define IXIA_UDP_SRC_PORT     5000U        /* STM32 */
#define IXIA_UDP_DST_PORT     6000U        /* Ixia */
#define IXIA_OFF_MAGIC        42U
#define IXIA_OFF_SEQ          46U
#define IXIA_OFF_FILL         50U

static const uint8_t ixia_peer_mac[6] = {0x00, 0x00, 0xAA, 0xBB, 0xCC, 0x02};
// static const uint8_t ixia_peer_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t ixia_dut_ip[4]   = {192, 168, 100, 20};
static const uint8_t ixia_peer_ip[4]  = {192, 168, 100, 10};
static const uint16_t ixia_sizes[]    = {64, 128, 256, 512, 1024, 1280, 1518};

static uint8_t ixia_buf[IXIA_NBUF][EQOS_BUF_SIZE] __attribute__((aligned(32)));

static void ixia_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void ixia_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t ixia_get16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void ixia_swap(uint8_t *a, uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0U; i < n; ++i) {
        const uint8_t t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

/* IPv4 header checksum over 20 bytes (checksum field = 0 while computing). */
static uint16_t ixia_ip_checksum(const uint8_t *ip)
{
    uint32_t sum = 0U;
    for (uint32_t i = 0U; i < 20U; i += 2U)
        sum += ixia_get16(ip + i);
    while ((sum >> 16) != 0U)
        sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Build a spec frame of len bytes (CRC excluded), sequence number 0. */
static void ixia_build_frame(uint8_t *f, uint32_t len, const uint8_t *dut_mac)
{
    static const uint8_t fill[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t *ip = f + 14;
    uint8_t *udp = f + 34;

    memcpy(f, ixia_peer_mac, 6U);
    memcpy(f + 6, dut_mac, 6U);
    ixia_put16(f + 12, IXIA_ETHERTYPE_IPV4);

    memset(ip, 0, 20U);
    ip[0] = 0x45U;                                   /* IPv4, IHL = 5 */
    ixia_put16(ip + 2, (uint16_t)(len - 14U));       /* total length */
    ip[8] = 64U;                                     /* TTL */
    ip[9] = 17U;                                     /* UDP */
    memcpy(ip + 12, ixia_dut_ip, 4U);
    memcpy(ip + 16, ixia_peer_ip, 4U);
    ixia_put16(ip + 10, ixia_ip_checksum(ip));

    ixia_put16(udp + 0, IXIA_UDP_SRC_PORT);
    ixia_put16(udp + 2, IXIA_UDP_DST_PORT);
    ixia_put16(udp + 4, (uint16_t)(len - 34U));      /* UDP length = 8 + payload */
    ixia_put16(udp + 6, 0U);                         /* checksum 0 allowed */

    memcpy(f + IXIA_OFF_MAGIC, "IXTS", 4U);
    ixia_put32(f + IXIA_OFF_SEQ, 0U);
    for (uint32_t i = IXIA_OFF_FILL; i < len; ++i)
        f[i] = fill[(i - IXIA_OFF_FILL) & 3U];
}

static uint32_t ixia_dma_missed(void)
{
    return AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_MISSED_FRAME_COUNT);
}

static uint32_t ixia_mmc_tx_good(void)
{
    return AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MMC_TX_PACKET_COUNT_GOOD);
}

static int ixia_setup(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                      struct eqos_desc *rx_desc, uint8_t rx_buf[][EQOS_BUF_SIZE],
                      uint32_t rx_timeout_us)
{
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    if (eth_pkt_ring_init(0, tx_desc, EQOS_TX_DESC_COUNT,
                          rx_desc, EQOS_RX_DESC_COUNT, rx_buf) != EQOS_OK) {
        printf("FAIL: eth_pkt_ring_init\r\n");
        return -1;
    }
    eth_pkt_set_timeout(0U, rx_timeout_us);              /* TX pipelined */
    /* Only frames to MAC0 / broadcast (ARP): no promiscuous mode. */
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, EQOS_MAC_PKT_FILTER_PR, 0U)) {
        printf("FAIL: MACPFR readback\r\n");
        return -1;
    }
    const uint32_t maccr = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);
    printf("Link: %s Mbps %s duplex, MACCR=0x%08lX (LM=%lu)\r\n",
           (maccr & EQOS_MAC_CFG_FES) ? "100" : "10",
           (maccr & EQOS_MAC_CFG_DM) ? "full" : "half", (unsigned long)maccr,
           (unsigned long)((maccr & EQOS_MAC_CFG_LM) != 0U));
    printf("DUT MAC %02X:%02X:%02X:%02X:%02X:%02X  IP %u.%u.%u.%u  UDP %u\r\n",
           eth->cfg.mac_addr[0], eth->cfg.mac_addr[1], eth->cfg.mac_addr[2],
           eth->cfg.mac_addr[3], eth->cfg.mac_addr[4], eth->cfg.mac_addr[5],
           ixia_dut_ip[0], ixia_dut_ip[1], ixia_dut_ip[2], ixia_dut_ip[3],
           (unsigned int)IXIA_UDP_SRC_PORT);
    return 0;
}

/* ========================= Test 2: DUT max TX rate ========================= */

/*
 * Each USER press sends IXIA_TX_FRAME_COUNT frames of the next size in
 * ixia_sizes[] (64, 128, ... 1518, then again from 64) as fast as possible.
 * Printed after each run: frames sent, egress errors, MMC TX good delta
 * (cross-check), elapsed ms and the STM32-side fps / L2 Mbps estimate.
 * Never returns.
 */
void eth_test_ixia_dut_tx(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                          struct eqos_desc *rx_desc, uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    unsigned int size_idx = 0U;

    printf("\r\n ========== Ixia Test 2: DUT max TX rate ==========\r\n");
    if (ixia_setup(eth, tx_desc, rx_desc, rx_buf, 0U) != 0)
        while (1) { }
    printf("Dst MAC %02X:%02X:%02X:%02X:%02X:%02X  IP %u.%u.%u.%u  UDP %u, %lu frames per run\r\n",
           ixia_peer_mac[0], ixia_peer_mac[1], ixia_peer_mac[2],
           ixia_peer_mac[3], ixia_peer_mac[4], ixia_peer_mac[5],
           ixia_peer_ip[0], ixia_peer_ip[1], ixia_peer_ip[2], ixia_peer_ip[3],
           (unsigned int)IXIA_UDP_DST_PORT, (unsigned long)IXIA_TX_FRAME_COUNT);

    while (1) {
        const uint32_t size = ixia_sizes[size_idx];
        const uint32_t len = size - 4U;                  /* MAC adds the CRC */

        printf("\r\nPress USER to send %lu frames of %lu B.\r\n",
               (unsigned long)IXIA_TX_FRAME_COUNT, (unsigned long)size);
        send_requested = 0U;
        while (send_requested == 0U) { }
        send_requested = 0U;

        for (uint32_t b = 0U; b < IXIA_NBUF; ++b)
            ixia_build_frame(ixia_buf[b], len, eth->cfg.mac_addr);

        uint32_t sent = 0U;
        uint32_t errors = 0U;
        int last_err = 0;
        const uint32_t mmc0 = ixia_mmc_tx_good();
        const uint32_t t0 = HAL_GetTick();

        /* ---------- hot loop: no printf ---------- */
        for (uint32_t seq = 0U; seq < IXIA_TX_FRAME_COUNT; ++seq) {
            uint8_t *f = ixia_buf[seq % IXIA_NBUF];
            ixia_put32(f + IXIA_OFF_SEQ, seq);
            const int ret = eth_egress_packet(0, f, (int)len);
            if (ret < 0) {
                ++errors;
                last_err = ret;
                break;                               /* ring stuck: stop the run */
            }
            ++sent;
        }
        const int flush = eth_egress_flush(0, IXIA_FLUSH_TIMEOUT_US);
        const uint32_t ms = HAL_GetTick() - t0;
        const uint32_t mmc = ixia_mmc_tx_good() - mmc0;

        const uint32_t fps = (ms != 0U) ? (uint32_t)(((uint64_t)sent * 1000U) / ms) : 0U;
        const uint32_t kbps = (uint32_t)(((uint64_t)fps * size * 8U) / 1000U);
        printf("Size %lu B: sent %lu, egress errors %lu (last %d), flush %s, MMC TX good %lu\r\n",
               (unsigned long)size, (unsigned long)sent, (unsigned long)errors, last_err,
               flush == EQOS_OK ? "OK" : "TIMEOUT", (unsigned long)mmc);
        printf("  %lu ms, ~%lu fps, ~%lu.%03lu Mbps L2 (STM32 estimate, Ixia is the reference)\r\n",
               (unsigned long)ms, (unsigned long)fps,
               (unsigned long)(kbps / 1000U), (unsigned long)(kbps % 1000U));
        if (errors != 0U || flush != EQOS_OK)
            printf("  DMA_CH0_Status=0x%08lX: reset before the next run\r\n",
                   (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));

        size_idx = (size_idx + 1U) % (sizeof(ixia_sizes) / sizeof(ixia_sizes[0]));
    }
}

/* ========================= Test 4: reflect mode ========================= */

struct ixia_reflect_cnt {
    uint32_t rx;          /* frames received without RX error */
    uint32_t reflected;   /* UDP frames sent back */
    uint32_t dropped;     /* egress failed (no free TX slot) */
    uint32_t arp_replies;
    uint32_t ignored;     /* not IPv4/UDP to MAC0, not an ARP request for DUT IP */
    uint32_t rx_errors;   /* ingress < 0 (other than "no frame") or RDES3.ES=1 */
};

/* ARP request for the DUT IP -> reply in place. Returns true when sent. */
static bool ixia_arp_reply(uint8_t *f, int n, const uint8_t *dut_mac)
{
    uint8_t *arp = f + 14;

    if (n < 42 || ixia_get16(arp + 0) != 1U || ixia_get16(arp + 2) != IXIA_ETHERTYPE_IPV4 ||
        ixia_get16(arp + 6) != 1U || memcmp(arp + 24, ixia_dut_ip, 4U) != 0)
        return false;                               /* not a request for us */

    memcpy(f, arp + 8, 6U);                         /* Ethernet dst = requester */
    memcpy(f + 6, dut_mac, 6U);
    ixia_put16(arp + 6, 2U);                        /* reply */
    memcpy(arp + 18, arp + 8, 10U);                 /* target = old sender (MAC + IP) */
    memcpy(arp + 8, dut_mac, 6U);                   /* sender = DUT */
    memcpy(arp + 14, ixia_dut_ip, 4U);
    return eth_egress_packet(0, f, n) >= 0;
}

/*
 * For each IPv4/UDP frame to MAC0: swap dst/src MAC (src = MAC0), swap src/dst
 * IP, swap UDP ports, send it back with the same length. The payload is not
 * touched; IP and UDP checksums stay valid (sums are order-independent).
 * ARP requests for 192.168.100.20 are answered.
 * USER press: print the counters since the last press, then reset them.
 * Never returns.
 */
void eth_test_ixia_reflect(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                           struct eqos_desc *rx_desc, uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    struct ixia_reflect_cnt cnt = {0};
    uint32_t slot = 0U;

    printf("\r\n ========== Ixia Test 4: RFC 2544 throughput, reflect mode ==========\r\n");
    if (ixia_setup(eth, tx_desc, rx_desc, rx_buf, 0U) != 0)  /* RX: poll, no wait */
        while (1) { }
    const uint8_t *mac0 = eth->cfg.mac_addr;
    (void)ixia_dma_missed();                         /* start from a fresh count */
    uint32_t t0 = HAL_GetTick();
    printf("Reflecting. Press USER to print and reset the counters.\r\n");
    send_requested = 0U;

    while (1) {
        if (send_requested != 0U) {
            send_requested = 0U;
            const uint32_t missed = ixia_dma_missed();
            printf("\r\n%lu ms: rx %lu, reflected %lu, dropped %lu, ARP replies %lu, "
                   "ignored %lu, rx errors %lu\r\n",
                   (unsigned long)(HAL_GetTick() - t0), (unsigned long)cnt.rx,
                   (unsigned long)cnt.reflected, (unsigned long)cnt.dropped,
                   (unsigned long)cnt.arp_replies, (unsigned long)cnt.ignored,
                   (unsigned long)cnt.rx_errors);
            printf("  DMA missed frames: %lu%s (DMA_CH0_Miss_Frame_Cnt=0x%08lX)\r\n",
                   (unsigned long)(missed & 0x7FFU),
                   (missed & (1U << 15)) ? " OVERFLOW" : "", (unsigned long)missed);
            memset(&cnt, 0, sizeof(cnt));
            t0 = HAL_GetTick();
        }

        /* ---------- hot path: no printf ---------- */
        uint8_t *f = ixia_buf[slot % IXIA_NBUF];
        const int n = eth_ingress_packet(0, f, EQOS_BUF_SIZE);
        if (n == ETH_PKT_ETIMEOUT)
            continue;                                /* no frame yet */
        if (n < 0 || (eth_pkt_last_rx(0)->rdes3 & EQOS_RDES3_WB_ES) != 0U) {
            ++cnt.rx_errors;
            continue;
        }
        ++cnt.rx;

        const uint16_t type = (n >= 14) ? ixia_get16(f + 12) : 0U;
        if (type == IXIA_ETHERTYPE_ARP) {
            if (ixia_arp_reply(f, n, mac0)) {
                ++cnt.arp_replies;
                ++slot;                              /* buffer now owned by TX */
            } else {
                ++cnt.ignored;
            }
            continue;
        }

        uint8_t *ip = f + 14;
        const uint32_t ihl = (n >= 34) ? (uint32_t)(ip[0] & 0x0FU) * 4U : 0U;
        if (type != IXIA_ETHERTYPE_IPV4 || ihl < 20U || ip[9] != 17U ||
            (uint32_t)n < 14U + ihl + 8U || memcmp(f, mac0, 6U) != 0) {
            ++cnt.ignored;
            continue;
        }
        uint8_t *udp = ip + ihl;
        memcpy(f, f + 6, 6U);                        /* dst = sender */
        memcpy(f + 6, mac0, 6U);                     /* src = DUT */
        ixia_swap(ip + 12, ip + 16, 4U);             /* src IP <-> dst IP */
        ixia_swap(udp + 0, udp + 2, 2U);             /* src port <-> dst port */

        if (eth_egress_packet(0, f, n) < 0) {
            ++cnt.dropped;
        } else {
            ++cnt.reflected;
            ++slot;                                  /* buffer now owned by TX */
        }
    }
}
