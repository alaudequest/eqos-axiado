#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

/* MAC_Interrupt_Status.MMCTXIS: MMC TX interrupt, and which read clears TXGPKTIS.
 * Flow: 1_test_flow_markdown/MAC_ISR_MMC_flow.md (TX side only).
 *
 * main.c includes this file AFTER the TX/RX rings are declared; do not add
 * it as a separate build source.
 *
 * MMC_Tx_Interrupt.TXGPKTIS sets when Tx_Packet_Count_Good reaches 0x8000_0000.
 *   Databook text : cleared by reading the counter (bits [7:0]).
 *   ST bit marking: rc_r, cleared by reading MMC_Tx_Interrupt itself.
 * Each case presets all counters to 0x7FFF_FFF0 (CNTPRST) and sends 20 frames:
 *   Case 1: read MMC_Tx_Interrupt first      -> log whether MMCTXIS clears
 *   Case 2: read Tx_Packet_Count_Good first  -> log whether MMCTXIS clears
 * PASS per case: MMCTXIS=MMCIS=1 after the frames, both 0 at the end of the case.
 * All RX MMC interrupts are masked. Path: internal MAC loopback (LM=1). */

#define MMCTX_FRAME_LEN          60U
#define MMCTX_FRAME_COUNT        20U      /* 16 needed from 0x7FFF_FFF0 */
#define MMCTX_TX_TIMEOUT_US      100000U
#define MMCTX_RX_TIMEOUT_US      10000U
#define MMCTX_POLL_TIMEOUT_US    100000U
#define MMCTX_ETHERTYPE          0x88B5U

/* EQOS databook registers (offset from MAC base) and bits not in eqos_hw.h */
#define MMCTX_MAC_INTERRUPT_STATUS   0x0B0u
#define MMCTX_ISR_MMCIS              (1u << 8)
#define MMCTX_ISR_MMCTXIS            (1u << 10)
#define MMCTX_MMC_CONTROL            0x700u
#define MMCTX_CTRL_CNTSTOPRO         (1u << 1)
#define MMCTX_CTRL_RSTONRD           (1u << 2)
#define MMCTX_CTRL_CNTFREEZ          (1u << 3)
#define MMCTX_CTRL_CNTPRST           (1u << 4)
#define MMCTX_CTRL_CNTPRSTLVL        (1u << 5)  /* 0: preset to 0x7FFF_FFF0 */
#define MMCTX_MMC_TX_INTERRUPT       0x708u
#define MMCTX_MMC_RX_INTERRUPT_MASK  0x70Cu
#define MMCTX_MMC_TX_INTERRUPT_MASK  0x710u
#define MMCTX_TXGPKTIS               (1u << 21) /* same bit in the mask: TXGPKTIM */
#define MMCTX_TX_PACKET_COUNT_GOOD   0x768u
#define MMCTX_RX_MASK_ALL            0x0C020060u /* RXLPITRCIM RXLPIUSCIM RXUCGPIM RXALGNERPIM RXCRCERPIM */
#define MMCTX_TX_MASK_ALL            0x0C20C000u /* TXLPITRCIM TXLPIUSCIM TXGPKTIM TXMCOLGPIM TXSCOLGPIM */

static uint8_t mmctx_tx_buf[MMCTX_FRAME_LEN] __attribute__((aligned(32)));
static uint8_t mmctx_rx_frame[EQOS_BUF_SIZE];

int eth_test_mac_isr_mmc_tx(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                            struct eqos_desc *rx_desc,
                            uint8_t rx_buf[][EQOS_BUF_SIZE])
{
    static const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    const unsigned int case_count = 2; //case 1 interrupt-register read, case 2 counter read
    bool cleared_by_ir = false;        /* MMCTXIS cleared by reading MMC_Tx_Interrupt */
    bool cleared_by_cnt = false;       /* MMCTXIS cleared by reading the counter */
    unsigned int failures = 0U;
    unsigned int passes = 0U;
    uint32_t isr;

    printf("\r\n ========== Begin MAC_Interrupt_Status MMCTXIS test ==========\r\n");
    if (eth == NULL || tx_desc == NULL || rx_desc == NULL || rx_buf == NULL)
        return -1;
    const uint8_t *mac0 = eth->cfg.mac_addr;
    const uint32_t saved_ctrl = AX_BASE_READ_REG32(EQOS_MAC_BASE, MMCTX_MMC_CONTROL);
    const uint32_t saved_rx_mask = AX_BASE_READ_REG32(EQOS_MAC_BASE, MMCTX_MMC_RX_INTERRUPT_MASK);
    const uint32_t saved_tx_mask = AX_BASE_READ_REG32(EQOS_MAC_BASE, MMCTX_MMC_TX_INTERRUPT_MASK);

    /* ---------- 1. Setup: MMC counting, only TXGPKTIS unmasked ---------- */
    if (!ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, 0U,
                        EQOS_MAC_CFG_LM | EQOS_MAC_CFG_FES | EQOS_MAC_CFG_DM |
                        EQOS_MAC_CFG_CST | EQOS_MAC_CFG_ACS |
                        EQOS_MAC_CFG_TE | EQOS_MAC_CFG_RE) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, MMCTX_MMC_CONTROL,
                        MMCTX_CTRL_CNTSTOPRO | MMCTX_CTRL_RSTONRD |
                        MMCTX_CTRL_CNTFREEZ | MMCTX_CTRL_CNTPRSTLVL, 0U) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, MMCTX_MMC_RX_INTERRUPT_MASK, MMCTX_RX_MASK_ALL, MMCTX_RX_MASK_ALL) ||
        !ETH_REG_UPDATE(EQOS_MAC_BASE, MMCTX_MMC_TX_INTERRUPT_MASK, MMCTX_TX_MASK_ALL, MMCTX_TX_MASK_ALL & ~MMCTX_TXGPKTIS)) {
        printf("FAIL: MMC setup readback\r\n");
        return -1;
    }

    const uint32_t tx_len = eth_construct_frame(mmctx_tx_buf, sizeof(mmctx_tx_buf),
                                                MMCTX_FRAME_LEN, mac0, mac0,
                                                MMCTX_ETHERTYPE, pattern, sizeof(pattern));
    if (tx_len == 0U) {
        printf("FAIL: eth_construct_frame\r\n");
        return -1;
    }

    for (unsigned int case_idx = 0U; case_idx < case_count; ++case_idx) {
        const bool ir_first = (case_idx == 0U);
        bool case_pass = true;

        printf("\r\n--- Case %u/%u: read %s first ---\r\n", case_idx + 1U, case_count,
               ir_first ? "MMC_Tx_Interrupt" : "Tx_Packet_Count_Good");

        /* ---------- 2. Clear old status, preset counters to 0x7FFF_FFF0 ---------- */
        (void)AX_BASE_READ_REG32(EQOS_MAC_BASE, MMCTX_MMC_TX_INTERRUPT);
        (void)AX_BASE_READ_REG32(EQOS_MAC_BASE, MMCTX_TX_PACKET_COUNT_GOOD);
        AX_BASE_WRITE_REG32(EQOS_MAC_BASE, MMCTX_MMC_CONTROL,
                            AX_BASE_READ_REG32(EQOS_MAC_BASE, MMCTX_MMC_CONTROL) |
                            MMCTX_CTRL_CNTPRST);
        for (uint32_t t = 0U; t < 1000U &&
             (AX_BASE_READ_REG32(EQOS_MAC_BASE, MMCTX_MMC_CONTROL) & MMCTX_CTRL_CNTPRST) != 0U; ++t)
            eqos_udelay(1U);
        isr = AX_BASE_READ_REG32(EQOS_MAC_BASE, MMCTX_MAC_INTERRUPT_STATUS);
        printf("Preset: Tx_Packet_Count_Good=0x%08lX MAC_Interrupt_Status=0x%08lX\r\n",
               (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, MMCTX_TX_PACKET_COUNT_GOOD),
               (unsigned long)isr);
        if ((isr & MMCTX_ISR_MMCTXIS) != 0U) {
            printf("RESULT: FAIL (MMCTXIS already set before the frames)\r\n");
            ++failures;
            continue;
        }

        /* ---------- 3. Send 20 frames (receive each looped-back frame) ---------- */
        for (uint32_t n = 0U; n < MMCTX_FRAME_COUNT && case_pass; ++n) {
            struct eth_tx_frame txf = {0};
            struct eth_rx_frame rxf;
            if (eth_tx_frame_send(eth, tx_desc, mmctx_tx_buf, tx_len, 0U, 0U,
                                  MMCTX_TX_TIMEOUT_US, &txf) != EQOS_OK ||
                (txf.tdes3 & EQOS_TDES3_WB_ES) != 0U) {
                printf("FAIL: TX frame %lu, TDES3=0x%08lX\r\n",
                       (unsigned long)n, (unsigned long)txf.tdes3);
                case_pass = false;
            }
            (void)eth_rx_frame_receive(eth, rx_desc, rx_buf, mmctx_rx_frame,
                                       sizeof(mmctx_rx_frame), MMCTX_RX_TIMEOUT_US, &rxf);
        }
        if (!case_pass) {
            printf("RESULT: FAIL (TX)\r\n");
            ++failures;
            break; /* DMA may still own a descriptor: stop the test. */
        }

        /* ---------- 4. Poll MMCTXIS ---------- */
        isr = AX_BASE_READ_REG32(EQOS_MAC_BASE, MMCTX_MAC_INTERRUPT_STATUS);
        for (uint32_t t = 0U; t < MMCTX_POLL_TIMEOUT_US && (isr & MMCTX_ISR_MMCTXIS) == 0U; t += 10U) {
            eqos_udelay(10U);
            isr = AX_BASE_READ_REG32(EQOS_MAC_BASE, MMCTX_MAC_INTERRUPT_STATUS);
        }
        printf("After %lu frames: MAC_Interrupt_Status=0x%08lX MMCTXIS=%lu MMCIS=%lu\r\n",
               (unsigned long)MMCTX_FRAME_COUNT, (unsigned long)isr,
               (unsigned long)((isr & MMCTX_ISR_MMCTXIS) != 0U),
               (unsigned long)((isr & MMCTX_ISR_MMCIS) != 0U));
        if ((isr & (MMCTX_ISR_MMCTXIS | MMCTX_ISR_MMCIS)) !=
            (MMCTX_ISR_MMCTXIS | MMCTX_ISR_MMCIS)) {
            printf("RESULT: FAIL (MMCTXIS/MMCIS not set)\r\n");
            ++failures;
            continue;
        }

        /* ---------- 5. First read: interrupt register (case 1) or counter (case 2) ---------- */
        const uint32_t first = AX_BASE_READ_REG32(EQOS_MAC_BASE, ir_first ?
                                                  MMCTX_MMC_TX_INTERRUPT :
                                                  MMCTX_TX_PACKET_COUNT_GOOD);
        isr = AX_BASE_READ_REG32(EQOS_MAC_BASE, MMCTX_MAC_INTERRUPT_STATUS);
        const bool cleared = (isr & MMCTX_ISR_MMCTXIS) == 0U;
        printf("Read %s=0x%08lX -> MMCTXIS=%lu (%s)\r\n",
               ir_first ? "MMC_Tx_Interrupt" : "Tx_Packet_Count_Good",
               (unsigned long)first, (unsigned long)!cleared,
               cleared ? "CLEARED by this read" : "not cleared");
        if (ir_first) {
            cleared_by_ir = cleared;
            if ((first & MMCTX_TXGPKTIS) == 0U) {
                printf("FAIL: TXGPKTIS=0 in MMC_Tx_Interrupt\r\n");
                case_pass = false;
            }
        } else {
            cleared_by_cnt = cleared;
            if (first < 0x80000000U) {
                printf("FAIL: counter below 0x80000000\r\n");
                case_pass = false;
            }
        }

        /* ---------- 6. Second read: the other register, then MMCTXIS/MMCIS must be 0 ---------- */
        const uint32_t second = AX_BASE_READ_REG32(EQOS_MAC_BASE, ir_first ?
                                                   MMCTX_TX_PACKET_COUNT_GOOD :
                                                   MMCTX_MMC_TX_INTERRUPT);
        isr = AX_BASE_READ_REG32(EQOS_MAC_BASE, MMCTX_MAC_INTERRUPT_STATUS);
        printf("Read %s=0x%08lX -> MAC_Interrupt_Status=0x%08lX\r\n",
               ir_first ? "Tx_Packet_Count_Good" : "MMC_Tx_Interrupt",
               (unsigned long)second, (unsigned long)isr);
        if ((isr & (MMCTX_ISR_MMCTXIS | MMCTX_ISR_MMCIS)) != 0U) {
            printf("FAIL: MMCTXIS/MMCIS still set after both reads\r\n");
            case_pass = false;
        }
        printf("RESULT: %s\r\n", case_pass ? "PASS" : "FAIL");
        if (case_pass)
            ++passes;
        else
            ++failures;
    }

    /* ---------- 7. Restore MMC_Control and the masks ---------- */
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, MMCTX_MMC_CONTROL, saved_ctrl & ~MMCTX_CTRL_CNTPRST);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, MMCTX_MMC_RX_INTERRUPT_MASK, saved_rx_mask);
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, MMCTX_MMC_TX_INTERRUPT_MASK, saved_tx_mask);

    printf("\r\nClear by reading MMC_Tx_Interrupt     : %s\r\n", cleared_by_ir ? "YES" : "NO");
    printf("Clear by reading Tx_Packet_Count_Good : %s\r\n", cleared_by_cnt ? "YES" : "NO");
    printf("\r\n========== MMCTXIS test: %u/%u PASS, %u failure(s) ==========\r\n",
           passes, case_count, failures);
    if (passes + failures < case_count)
        printf("%u case(s) did not run; reset before rerunning.\r\n",
               case_count - passes - failures);
    return (int)failures;
}
