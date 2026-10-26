static void eth_queue_test_run(void)
{
    printf("\r\n========== DMA TX descriptor starvation / resume ==========\r\n");
    if (!qt_roundtrip(128U, false)) return;
    AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS, ETH_DMACSR_TBU);
    /* Move the tail forward so DMA tries to read a descriptor with OWN=0.
     * This makes DMA report that no TX descriptor is ready. */
    uint32_t next = (eth.tx_head + 1U) & (EQOS_TX_DESC_COUNT - 1U);
    AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR,
                       (uint32_t)(uintptr_t)&tx_desc[next]);
    bool unavailable = false;
    for (uint32_t us = 0; us < QT_TIMEOUT_US; ++us) {
        if ((ETH->DMACSR & ETH_DMACSR_TBU) != 0U) { unavailable = true; break; }
        eqos_udelay(1U);
    }
    qt_check(unavailable, "TX unavailable not observed");
    qt_debug();
    qt_check((tx_desc[eth.tx_head].des3 & EQOS_TDES3_OWN) == 0U,
             "starved TX descriptor ownership changed");
    /* Set OWN and update the tail to restart TX without resetting DMA. */
    for (uint32_t i = 0U; i < EQOS_TX_DESC_COUNT * 2U; ++i)
        if (!qt_roundtrip(128U, false)) return;
}
