/* main.c includes this file once, after declaring the TX and RX rings. */
#include "eth_queue_test_common.h"
_Static_assert(EQOS_TX_DESC_COUNT >= 2U && EQOS_RX_DESC_COUNT >= EQOS_TX_DESC_COUNT,
               "Queue tests need at least two TX slots and RX >= TX");
_Static_assert(QUEUE_TEST_WRAPS > 0U, "Wrap count must be positive");
_Static_assert(QUEUE_TEST_PBL == 1U || QUEUE_TEST_PBL == 2U || QUEUE_TEST_PBL == 4U ||
               QUEUE_TEST_PBL == 8U || QUEUE_TEST_PBL == 16U || QUEUE_TEST_PBL == 32U,
               "Unsupported DMA burst length");

#if ACTIVE_TEST == TEST_RMII_MTL_TX_STORE_FORWARD
#include "L1_RMII_MTL_TX_STORE_FORWARD.c"
#elif ACTIVE_TEST == TEST_RMII_MTL_TX_THRESHOLD
#include "L1_RMII_MTL_TX_THRESHOLD.c"
#elif ACTIVE_TEST == TEST_RMII_MTL_RX_POLICY
#include "L1_RMII_MTL_RX_POLICY.c"
#elif ACTIVE_TEST == TEST_RMII_MTL_RX_OVERFLOW
#include "L1_RMII_MTL_RX_OVERFLOW.c"
#elif ACTIVE_TEST == TEST_RMII_MTL_RX_FLOW_CONTROL
#include "L1_RMII_MTL_RX_FLOW_CONTROL.c"
#elif ACTIVE_TEST == TEST_RMII_DMA_RING_WRAP
#include "L1_RMII_DMA_RING_WRAP.c"
#elif ACTIVE_TEST == TEST_RMII_DMA_TX_SCATTER
#include "L1_RMII_DMA_TX_SCATTER.c"
#elif ACTIVE_TEST == TEST_RMII_DMA_BURST
#include "L1_RMII_DMA_BURST.c"
#elif ACTIVE_TEST == TEST_RMII_DMA_TX_STARVATION
#include "L1_RMII_DMA_TX_STARVATION.c"
#elif ACTIVE_TEST == TEST_RMII_DMA_RX_STARVATION
#include "L1_RMII_DMA_RX_STARVATION.c"
#endif

/* Set up the test before starting the MAC and DMA. */
static void eth_queue_test_configure(void)
{
    for (uint32_t i = 0U; i < 8U; ++i) {
        qt_tx_ring.before[i] = qt_tx_ring.after[i] = 0xA55A6996U;
        qt_rx_ring.before[i] = qt_rx_ring.after[i] = 0xA55A6996U;
    }
    ETH->MACPFR = 0U;
    ETH->MTLTQOMR |= ETH_MTLTQOMR_TSF;
    ETH->MTLRQOMR = (ETH->MTLRQOMR & ~(ETH_MTLRQOMR_FEP | ETH_MTLRQOMR_FUP |
                                     ETH_MTLRQOMR_EHFC)) | ETH_MTLRQOMR_RSF;
    ETH->MACTFCR = 0U;
    ETH->MACRFCR = 0U;
    ETH->DMACCR &= ~EQOS_DMA_CH_CTRL_PBLX8;
    ETH->DMACTCR = (ETH->DMACTCR & ~ETH_DMACTCR_TPBL) |
                   (QUEUE_TEST_PBL << ETH_DMACTCR_TPBL_Pos);
    ETH->DMACRCR = (ETH->DMACRCR & ~ETH_DMACRCR_RPBL) |
                   (QUEUE_TEST_PBL << ETH_DMACRCR_RPBL_Pos);
    #if ACTIVE_TEST == TEST_RMII_MTL_TX_THRESHOLD
    /* Use the smallest TX burst to make DMA transfer less data per burst.
     * This may not cause underflow. Run another DMA transfer at the same
     * time to make the Ethernet DMA wait longer for the bus. */
    ETH->DMACTCR = (ETH->DMACTCR & ~ETH_DMACTCR_TPBL) | ETH_DMACTCR_TPBL_1PBL;
    #endif
}
