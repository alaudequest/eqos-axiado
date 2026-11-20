#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* DMA CH0 packet TX/RX test (test ID 31).
 *
 * Checks the B0-style eth_egress_packet*() / eth_ingress_packet() on CH0.
 *
 * main.c includes this file AFTER the TX/RX rings are declared and runs it in
 * the MAC control flow (MACCR.LM=1, 100M full duplex); do not add it as a
 * separate build source.
 *
 * Every frame: DA = SA = MAC0, EtherType 0x88B5, bytes 14..15 = sequence
 * number (a stale or repeated frame fails the compare).
 *   Case 1: one buffer, lengths 60, 61, 62, 63, 1514 (incl. non-multiple of 4)
 *   Case 2: two buffers: buf1 = 14-byte header, buf2 = payload in another array
 *   Case 3: context descriptor + normal descriptor, order plain/ctx/ctx/plain so
 *           a context/normal pair crosses the ring wrap (TX ring of 4)
 *   Case 4: API guards: CH1 not initialized, len 0, RX frame > blen; the next
 *           frame must still be received (ring in step)
 * Path: MAC TX -> MAC loopback (LM=1) -> MAC RX. MACCR.CST=1 so RX length
 * equals TX length (FCS stripped). */

#define PKT_TEST_TIMEOUT_US   100000U
#define PKT_TEST_ETHERTYPE    0x88B5U
#define PKT_TEST_MAX_LEN      1514U
#define PKT_TEST_HDR_LEN      14U

static uint8_t pkt_tx_buf[PKT_TEST_MAX_LEN] __attribute__((aligned(32)));
static uint8_t pkt_tx_payload[PKT_TEST_MAX_LEN] __attribute__((aligned(32)));
static uint8_t pkt_rx_frame[EQOS_BUF_SIZE];
static uint16_t pkt_seq;

/* Build a frame of len bytes with the next sequence number at bytes 14..15. */
static uint32_t pkt_build(const uint8_t *mac0, uint32_t len)
{
    static const uint8_t pattern[] = {0x00, 0x00, 0xAA, 0x55, 0x01, 0x02};
    if (eth_construct_frame(pkt_tx_buf, sizeof(pkt_tx_buf), len, mac0, mac0,
                            PKT_TEST_ETHERTYPE, pattern, sizeof(pattern)) == 0U)
        return 0U;
    ++pkt_seq;
    pkt_tx_buf[14] = (uint8_t)(pkt_seq >> 8);
    pkt_tx_buf[15] = (uint8_t)pkt_seq;
    return len;
}

/* Receive one frame and compare it with expected[0..len). */
static bool pkt_rx_check(const uint8_t *expected, uint32_t len)
{
    const int rx_len = eth_ingress_packet(0, pkt_rx_frame, sizeof(pkt_rx_frame));
    const struct eth_rx_frame *rxf = eth_pkt_last_rx(0);

    if (rx_len < 0) {
        printf("FAIL: RX ret=%d (%s), DMACSR=0x%08lX\r\n", rx_len,
               rx_len == ETH_PKT_ETIMEOUT ? "timeout" :
               rx_len == ETH_PKT_ESTALL ? "stalled mid-way" : "too big",
               (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
        return false;
    }
    if ((rxf->rdes3 & EQOS_RDES3_WB_ES) != 0U) {
        printf("FAIL: RX ES=1, RDES3=0x%08lX\r\n", (unsigned long)rxf->rdes3);
        return false;
    }
    if ((uint32_t)rx_len != len) {
        printf("FAIL: RX length %d, expected %lu\r\n", rx_len, (unsigned long)len);
        return false;
    }
    for (uint32_t i = 0U; i < len; ++i) {
        if (pkt_rx_frame[i] != expected[i]) {
            printf("FAIL at byte %lu: expected %02X, got %02X\r\n", (unsigned long)i,
                   (unsigned int)expected[i], (unsigned int)pkt_rx_frame[i]);
            return false;
        }
    }
    return true;
}

/* Check egress return value and TDES3.ES. */
static bool pkt_tx_check(int tx_ret, uint32_t len)
{
    const struct eth_tx_frame *txf = eth_pkt_last_tx(0);

    if (tx_ret != (int)len) {
        printf("FAIL: TX ret=%d, expected %lu, DMACSR=0x%08lX\r\n", tx_ret,
               (unsigned long)len,
               (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
        return false;
    }
    if ((txf->tdes3 & EQOS_TDES3_WB_ES) != 0U) {
        printf("FAIL: TX ES=1, TDES3=0x%08lX\r\n", (unsigned long)txf->tdes3);
        return false;
    }
    return true;
}

int eth_test_dma_ch0_pkt_txrx(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                              struct eqos_desc *rx_desc,
                              uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    const unsigned int case_count = 4U;
    unsigned int failures = 0U;
    unsigned int passes = 0U;

    printf("\r\n ========== Begin DMA CH0 packet TX/RX test (MAC loopback) ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint8_t *mac0 = eth->cfg.mac_addr;

    /* ---------- 1. Setup: bind CH0 rings, CST=1 so RX length = TX length ---------- */
    if (eth_pkt_ring_init(0, tx_desc, EQOS_TX_DESC_COUNT,
                          rx_desc, EQOS_RX_DESC_COUNT, rx_buf) != EQOS_OK) {
        printf("FAIL: eth_pkt_ring_init\r\n");
        return -1;
    }
    eth_pkt_set_timeout(PKT_TEST_TIMEOUT_US, PKT_TEST_TIMEOUT_US);
    const uint32_t maccr_saved = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION);
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION,
                        0U, EQOS_MAC_CFG_CST | EQOS_MAC_CFG_ACS)) {
        printf("FAIL: MACCR setup readback\r\n");
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, maccr_saved);
        return -1;
    }

    /* ---------- Case 1: one buffer, several lengths ---------- */
    {
        static const uint32_t lens[] = {60U, 61U, 62U, 63U, PKT_TEST_MAX_LEN};
        bool case_pass = true;
        printf("\r\n--- Case 1/%u: one buffer, lengths 60/61/62/63/1514 ---\r\n", case_count);
        for (unsigned int i = 0U; i < sizeof(lens) / sizeof(lens[0]) && case_pass; ++i) {
            const uint32_t len = pkt_build(mac0, lens[i]);
            const int tx = eth_egress_packet(0, pkt_tx_buf, (int)len);
            case_pass = pkt_tx_check(tx, len) && pkt_rx_check(pkt_tx_buf, len);
            printf("len %4lu: TDES3=0x%08lX desc_used=%lu -> %s\r\n", (unsigned long)len,
                   (unsigned long)eth_pkt_last_tx(0)->tdes3,
                   (unsigned long)eth_pkt_last_tx(0)->desc_used,
                   case_pass ? "OK" : "FAIL");
        }
        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        case_pass ? ++passes : ++failures;
    }

    /* ---------- Case 2: header in buffer 1, payload in buffer 2 ---------- */
    {
        bool case_pass = true;
        printf("\r\n--- Case 2/%u: buf1 = 14 B header, buf2 = 1500 B payload ---\r\n", case_count);
        const uint32_t len = pkt_build(mac0, PKT_TEST_MAX_LEN);
        memcpy(pkt_tx_payload, pkt_tx_buf + PKT_TEST_HDR_LEN, len - PKT_TEST_HDR_LEN);
        struct eth_tx_req req = {0};
        req.buf1 = pkt_tx_buf;                       /* header only */
        req.len1 = PKT_TEST_HDR_LEN;
        req.buf2 = pkt_tx_payload;                   /* payload from another array */
        req.len2 = len - PKT_TEST_HDR_LEN;
        const int tx = eth_egress_packet_ex(0, &req);
        case_pass = pkt_tx_check(tx, len) && pkt_rx_check(pkt_tx_buf, len);
        if (case_pass && eth_pkt_last_tx(0)->desc_used != 1U) {
            printf("FAIL: desc_used=%lu, expected 1\r\n",
                   (unsigned long)eth_pkt_last_tx(0)->desc_used);
            case_pass = false;
        }
        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        case_pass ? ++passes : ++failures;
    }

    /* ---------- Case 3: context + normal descriptor, crossing the ring wrap ---------- */
    {
        static const bool use_ctx[] = {false, true, true, false};
        bool case_pass = true;
        printf("\r\n--- Case 3/%u: context descriptor before normal, plain/ctx/ctx/plain ---\r\n",
               case_count);
        for (unsigned int i = 0U; i < sizeof(use_ctx) / sizeof(use_ctx[0]) && case_pass; ++i) {
            const uint32_t len = pkt_build(mac0, 128U);
            struct eth_tx_req req = {0};
            req.ctx_valid = use_ctx[i];   /* empty context: TCMSSV=OSTC=VLTV=0, nothing to apply */
            req.buf1 = pkt_tx_buf;
            req.len1 = len;
            const int tx = eth_egress_packet_ex(0, &req);
            const struct eth_tx_frame *txf = eth_pkt_last_tx(0);
            case_pass = pkt_tx_check(tx, len) && pkt_rx_check(pkt_tx_buf, len);
            if (case_pass && txf->desc_used != (use_ctx[i] ? 2U : 1U)) {
                printf("FAIL: desc_used=%lu\r\n", (unsigned long)txf->desc_used);
                case_pass = false;
            }
            if (case_pass && use_ctx[i] &&
                (txf->ctx_tdes3 & (EQOS_TDES3_OWN | EQOS_TDES3_CTXT)) != EQOS_TDES3_CTXT) {
                printf("FAIL: context not closed, ctx TDES3=0x%08lX\r\n",
                       (unsigned long)txf->ctx_tdes3);
                case_pass = false;
            }
            printf("frame %u ctx=%u: ctx TDES3=0x%08lX TDES3=0x%08lX -> %s\r\n", i + 1U,
                   (unsigned int)use_ctx[i], (unsigned long)txf->ctx_tdes3,
                   (unsigned long)txf->tdes3, case_pass ? "OK" : "FAIL");
        }
        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        case_pass ? ++passes : ++failures;
    }

    /* ---------- Case 4: API guards, ring stays in step ---------- */
    {
        bool case_pass = true;
        printf("\r\n--- Case 4/%u: CH1 not initialized, len 0, RX frame > blen ---\r\n", case_count);
        uint32_t len = pkt_build(mac0, 60U);
        int ret = eth_egress_packet(1, pkt_tx_buf, (int)len);
        printf("egress CH1: ret=%d, expect %d\r\n", ret, ETH_PKT_EINVAL);
        case_pass &= (ret == ETH_PKT_EINVAL);
        ret = eth_egress_packet(0, pkt_tx_buf, 0);
        printf("egress len 0: ret=%d, expect %d\r\n", ret, ETH_PKT_EINVAL);
        case_pass &= (ret == ETH_PKT_EINVAL);

        /* 60-byte frame into a 32-byte buffer: consumed, -2 returned. */
        ret = eth_egress_packet(0, pkt_tx_buf, (int)len);
        case_pass &= pkt_tx_check(ret, len);
        ret = eth_ingress_packet(0, pkt_rx_frame, 32);
        printf("ingress blen 32 for 60 B: ret=%d, expect %d, PL=%lu\r\n", ret,
               ETH_PKT_EINVAL, (unsigned long)eth_pkt_last_rx(0)->len);
        case_pass &= (ret == ETH_PKT_EINVAL);

        /* Next frame must be the new one (sequence number checked by compare). */
        len = pkt_build(mac0, 60U);
        ret = eth_egress_packet(0, pkt_tx_buf, (int)len);
        case_pass &= pkt_tx_check(ret, len) && pkt_rx_check(pkt_tx_buf, len);
        printf("next frame after drop: %s\r\n", case_pass ? "OK" : "FAIL");
        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        case_pass ? ++passes : ++failures;
    }

    /* ---------- Restore MACCR ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, maccr_saved);

    printf("\r\n========== DMA CH0 packet TX/RX test: %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, failures);
    return (int)failures;
}
