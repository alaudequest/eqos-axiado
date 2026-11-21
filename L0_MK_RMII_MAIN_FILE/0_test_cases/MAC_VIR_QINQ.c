#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* Test : QinQ, two VLAN tags through the MAC loopback, cases A-E (test ID 48).
 * MAC loopback (LM=1), 100M full duplex. Called by eth_rmii_mac_lb() after mac_start();
 * every run starts from a MAC reset.
 *
 *   Register / descriptor  Bit [pos]            Value   Function
 *   ---------------------- -------------------- ------- ------------------------------
 *   MAC_VLAN_Incl          VLTI [20]            1       outer tag taken from the TX context descriptor
 *                          VLP [18]             0       insertion decided per frame by TDES2.VTIR
 *                          CSVL [19]            1       outer type 0x88A8 (S-VLAN)
 *   MAC_Inner_VLAN_Incl    VLTI [20]            1       inner tag taken from the context (TDES2.IVT)
 *                          VLP [18], VLC [17:16] 1, 2   A: insert the inner tag in every frame; B: 0 (off)
 *                          VLP [18]             0       C, D, E: the context's IVTIR decides per descriptor
 *   MAC_VLAN_Tag           EDVLP [26], ESVL [18] 1      RX: two tags, 0x88A8 counts as VLAN
 *                          EVLRXS [24], EIVLRXS [31] 1  RX: outer / inner tag reported in RDES0
 *   TDES3 context          CTXT [30], VLTV [16] 1       context descriptor with a valid outer tag
 *                          IVLTV [17]           1       + inner tag valid
 *                          IVTIR [19:18]        0/2/3/1 A: none (VLC path), C: insert, D: replace, E: remove
 *                          VT [15:0]            tag     outer tag; TDES2.IVT [31:16] inner tag
 *   TDES2                  VTIR [15:14]         2 / 0   A, B, C: insert the outer tag; D, E: frame already tagged
 *
 * VLTI = 1 -> the MAC inserts the VT field of the TX context descriptor (databook Table 7-3,
 * EQOS-AHB/DMA). The TDES3 context VT description says "only when VLTI is 0": the two texts
 * disagree; this test follows Table 7-3.
 *
 * Frames (FCS excluded):  untagged 60 B = DA=SA=MAC0 | 88B5 | pattern
 *                         tagged   68 B = DA=SA=MAC0 | 88A8 07D0 | 8100 E0C8 | 88B5 | pattern
 * Returns the failure count, -1 on setup error. */

#define QINQ_UNTAGGED_LEN           60U
#define QINQ_TAGGED_LEN             68U
#define QINQ_TAG_BYTES              8U       /* two tags: S 4 B + C 4 B */
#define QINQ_TAG_S                  0x07D0U  /* outer S-VLAN: PCP 0, VID 2000 */
#define QINQ_TAG_C                  0x0064U  /* inner C-VLAN put by the MAC: PCP 0, VID 100 */

/* Registers and bits not defined in eqos_hw.h (EQoS databook names) */
#define QINQ_MAC_VLAN_INCL          0x060u
#define QINQ_MAC_INNER_VLAN_INCL    0x064u
#define QINQ_VLAN_INCL_VLTI         (1u << 20)
#define QINQ_VLAN_INCL_CSVL         (1u << 19)
#define QINQ_VLAN_INCL_VLP          (1u << 18)
#define QINQ_VLAN_INCL_VLC_INSERT   (2u << 16)
#define QINQ_VLAN_TAG_ESVL          (1u << 18)
#define QINQ_VLAN_TAG_EDVLP         (1u << 26)
#define QINQ_VLAN_TAG_EIVLRXS       (1u << 31)
#define QINQ_CTX_TDES2_IVT_SHIFT    16u
#define QINQ_CTX_TDES3_VLTV         (1u << 16)
#define QINQ_CTX_TDES3_IVLTV        (1u << 17)
#define QINQ_CTX_TDES3_IVTIR_SHIFT  18u
#define QINQ_CTX_TDES3_CDE          (1u << 23)
#define QINQ_VTIR_NONE              0u
#define QINQ_VTIR_INSERT            (2u << 14)  /* TDES2.VTIR [15:14] */
#define QINQ_IVTIR_NONE             0u          /* TDES3.IVTIR [19:18] of the context */
#define QINQ_IVTIR_REMOVE           1u
#define QINQ_IVTIR_INSERT           2u
#define QINQ_IVTIR_REPLACE          3u

static uint8_t qinq_tx_frame[QINQ_UNTAGGED_LEN] __attribute__((aligned(32)));
static uint8_t qinq_tx_tagged[QINQ_TAGGED_LEN] __attribute__((aligned(32)));
static uint8_t qinq_rx_frame[EQOS_BUF_SIZE];

/* Tags at bytes 12.. of the frame sent (tagged frame) and of the frame expected back.
 * One tag expected (B, E) = the first 4 bytes of qinq_tags_exp. */
static const uint8_t qinq_tags_sent[QINQ_TAG_BYTES] = {0x88, 0xA8, 0x07, 0xD0, 0x81, 0x00, 0xE0, 0xC8};
static const uint8_t qinq_tags_exp[QINQ_TAG_BYTES]  = {0x88, 0xA8, 0x07, 0xD0, 0x81, 0x00, 0x00, 0x64};

/* Untagged frame DA=SA=mac0 | 88B5 | pattern, and the same with the two tags of qinq_tags_sent
 * between SA and EtherType. Returns false if the frame cannot be built. */
static bool qinq_build_frames(const uint8_t *mac0)
{
    static const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};

    if (eth_construct_frame(qinq_tx_frame, sizeof(qinq_tx_frame), QINQ_UNTAGGED_LEN,
                            mac0, mac0, 0x88B5U, pattern, sizeof(pattern)) == 0U)
        return false;
    memcpy(qinq_tx_tagged, qinq_tx_frame, 12U);                                   /* DA, SA */
    memcpy(qinq_tx_tagged + 12U, qinq_tags_sent, QINQ_TAG_BYTES);                 /* S 07D0, C E0C8 */
    memcpy(qinq_tx_tagged + 20U, qinq_tx_frame + 12U, QINQ_UNTAGGED_LEN - 12U);   /* 88B5 + payload */
    return true;
}

/* One case: [context S 07D0 + C 0064 with ivtir] + frame with TDES2.VTIR = vtir, then RX.
 *   ctx       true: context descriptor before the frame; false: the MAC keeps the last one
 *   tagged    true: send qinq_tx_tagged (68 B), false: qinq_tx_frame (60 B)
 *   exp_tags  tag bytes expected at 12.. of the RX frame (8: S + C, 4: S only)
 * Checks: TX done without ES, context closed (OWN=0, CDE=0), RX length, DA/SA, tags,
 * EtherType + payload as sent. Returns true if the case is as expected. */
static bool qinq_case(char id, bool ctx, uint32_t ivtir, uint32_t vtir, bool tagged,
                      uint32_t exp_tags, const char *what)
{
    const uint8_t *tx = tagged ? qinq_tx_tagged : qinq_tx_frame;
    const uint32_t tx_len = tagged ? QINQ_TAGGED_LEN : QINQ_UNTAGGED_LEN;
    const uint32_t skip = tagged ? QINQ_TAG_BYTES : 0U;     /* tag bytes in the frame sent */
    const uint32_t exp_len = tx_len - skip + exp_tags;
    struct eth_tx_req req = {0};

    printf("\r\n--- Case %c: %s\r\n", id, what);
    printf("expect %lu B, bytes 12..%lu =", (unsigned long)exp_len, (unsigned long)(11U + exp_tags));
    for (uint32_t k = 0U; k < exp_tags; ++k)
        printf(" %02X", (unsigned int)qinq_tags_exp[k]);
    printf(", then 88 B5\r\n");

    /* 1. Context (optional) + frame, then RX */
    req.ctx_valid = ctx;
    req.ctx_tdes[2] = (uint32_t)QINQ_TAG_C << QINQ_CTX_TDES2_IVT_SHIFT;          /* IVT: inner tag */
    req.ctx_tdes3 = QINQ_CTX_TDES3_VLTV | QINQ_CTX_TDES3_IVLTV |
                    (ivtir << QINQ_CTX_TDES3_IVTIR_SHIFT) | QINQ_TAG_S;        /* VT: outer tag */
    req.buf1 = tx;
    req.len1 = tx_len;
    req.fd_tdes2 = vtir;
    const int tx_ret = eth_egress_packet_ex(0, &req);
    const struct eth_tx_frame *txf = eth_pkt_last_tx(0);
    const int rx_len = eth_ingress_packet(0, qinq_rx_frame, sizeof(qinq_rx_frame));
    const struct eth_rx_frame *rxf = eth_pkt_last_rx(0);

    /* 2. Check piece by piece */
    const bool tx_ok   = (tx_ret == (int)tx_len) && ((txf->tdes3 & EQOS_TDES3_WB_ES) == 0U);
    const bool ctx_ok  = !ctx || ((txf->ctx_tdes3 & (EQOS_TDES3_OWN | QINQ_CTX_TDES3_CDE)) == 0U);
    const bool len_ok  = (rx_len == (int)exp_len);
    const bool addr_ok = len_ok && memcmp(qinq_rx_frame, tx, 12U) == 0;
    const bool tag_ok  = len_ok && memcmp(qinq_rx_frame + 12U, qinq_tags_exp, exp_tags) == 0;
    const bool rest_ok = len_ok && memcmp(qinq_rx_frame + 12U + exp_tags, tx + 12U + skip,
                                          tx_len - 12U - skip) == 0;
    const bool pass = tx_ok && ctx_ok && len_ok && addr_ok && tag_ok && rest_ok;

    /* 3. One line + the failed checks */
    printf("TX ret=%d ctx TDES3=0x%08lX, RX len=%d RDES0=0x%08lX, bytes 12..21:",
           tx_ret, (unsigned long)txf->ctx_tdes3, rx_len, (unsigned long)rxf->rdes0);
    for (int k = 12; k < 22 && k < rx_len; ++k)
        printf(" %02X", (unsigned int)qinq_rx_frame[k]);
    printf("\r\n");
    if (!pass)
        printf("failed:%s%s%s%s%s%s\r\n", tx_ok ? "" : " TX", ctx_ok ? "" : " context",
               len_ok ? "" : " length", addr_ok ? "" : " DA/SA", tag_ok ? "" : " tags",
               rest_ok ? "" : " rest-of-frame");
    printf("RESULT: %s\r\n", pass ? "PASS" : "FAIL");
    return pass;
}

int eth_test_mac_vir_qinq(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                          struct eqos_desc *rx_desc, uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    const unsigned int case_count = 5U;
    unsigned int passes = 0U;

    printf("\r\n ========== Begin MAC_VLAN_Incl QinQ test (MAC loopback) ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    if (!qinq_build_frames(eth->cfg.mac_addr)) {
        printf("FAIL: eth_construct_frame\r\n");
        return -1;
    }

    /* ---------- 1. Setup: outer tag from the context (S-VLAN), RX sees two tags ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, QINQ_MAC_VLAN_INCL, QINQ_VLAN_INCL_VLTI | QINQ_VLAN_INCL_CSVL);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG,
                        EQOS_MAC_VLAN_TAG_EVLRXS | QINQ_VLAN_TAG_EDVLP |
                        QINQ_VLAN_TAG_EIVLRXS | QINQ_VLAN_TAG_ESVL);

    /* ---------- 2. A, B: inner tag decided by the register (VLP = 1, VLC = insert / off) ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, QINQ_MAC_INNER_VLAN_INCL,
                        QINQ_VLAN_INCL_VLTI | QINQ_VLAN_INCL_VLP | QINQ_VLAN_INCL_VLC_INSERT);
    printf("MAC_VLAN_Incl=0x%08lX MAC_Inner_VLAN_Incl=0x%08lX MAC_VLAN_Tag=0x%08lX\r\n",
           (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, QINQ_MAC_VLAN_INCL),
           (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, QINQ_MAC_INNER_VLAN_INCL),
           (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_VLAN_TAG));
    passes += qinq_case('A', true,  QINQ_IVTIR_NONE, QINQ_VTIR_INSERT, false, 8U,
                        "MAC_Inner_VLAN_Incl VLP=1 VLC=insert, context S 07D0 + C 0064, VTIR=insert");
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, QINQ_MAC_INNER_VLAN_INCL, 0U);
    passes += qinq_case('B', false, QINQ_IVTIR_NONE, QINQ_VTIR_INSERT, false, 4U,
                        "MAC_Inner_VLAN_Incl = 0, no new context, VTIR=insert: only the stored outer S tag");

    /* ---------- 3. C, D, E: inner tag decided by the context (VLP = 0, IVTIR) ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, QINQ_MAC_INNER_VLAN_INCL, QINQ_VLAN_INCL_VLTI);
    passes += qinq_case('C', true,  QINQ_IVTIR_INSERT,  QINQ_VTIR_INSERT, false, 8U,
                        "VLP=0, context IVTIR=insert, VTIR=insert: two tags, decided by the descriptor");
    passes += qinq_case('D', true,  QINQ_IVTIR_REPLACE, QINQ_VTIR_NONE,   true,  8U,
                        "frame sent with S 07D0 + C E0C8, IVTIR=replace, VTIR=none: inner becomes C 0064");
    passes += qinq_case('E', true,  QINQ_IVTIR_REMOVE,  QINQ_VTIR_NONE,   true,  4U,
                        "same two-tag frame, IVTIR=remove, VTIR=none: inner tag cut out, 4 bytes less");

    printf("\r\n========== MAC_VLAN_Incl QinQ test: %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, case_count - passes);
    return (int)(case_count - passes);
}
