static void eth_queue_test_run(void)
{
    printf("\r\n========== MTL RX flow-control thresholds ==========\r\n");
    uint32_t code = (ETH->MACHWF1R & ETH_MACHWF1R_RXFIFOSIZE) >>
                    ETH_MACHWF1R_RXFIFOSIZE_Pos;
    /* These RFA/RFD settings need an RX FIFO of at least 4 KiB.
     * Skip this check when the FIFO is too small, as on the H755. */
    if (code < 5U || code > 11U) {
        printf("SKIP: RX FIFO capability code=%lu; automatic thresholds need >=4 KiB.\r\n",
               (unsigned long)code);
        return;
    }
    printf("MANUAL: external peer must flood addressed frames for 5 seconds,\r\n"
           "capture PAUSE frames, then stop. Firmware withholds RX replenishment.\r\n");
    /* RFA=0: request PAUSE when only 1 KiB of free space remains.
     * RFD=1: stop requesting PAUSE when free space reaches 1.5 KiB.
     * The different levels prevent repeated on/off switching near one level. */
    ETH->MTLRQOMR = (ETH->MTLRQOMR & ~(ETH_MTLRQOMR_RFA | ETH_MTLRQOMR_RFD)) |
                    ETH_MTLRQOMR_EHFC | (1U << ETH_MTLRQOMR_RFD_Pos);
    ETH->MACTFCR = EQOS_MAC_TXFC_TFE | (0xFFFFU << EQOS_MAC_TXFC_PT_SHIFT);
    qt_check((ETH->MTLRQOMR & ETH_MTLRQOMR_EHFC) != 0U, "EHFC readback");
    uint32_t start = HAL_GetTick();
    uint32_t levels = 0U;
    while ((uint32_t)(HAL_GetTick() - start) < 5000U)
        levels |= 1U << ((ETH->MTLRQDR & ETH_MTLRQDR_RXQSTS) >> ETH_MTLRQDR_RXQSTS_Pos);
    qt_debug();
    /* Return each completed RX descriptor to DMA so reception can continue.
     * Frames come from an external sender. This test shows register values
     * but does not compare the received bytes with the sender's data. */
    start = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - start) < 5000U) {
        if ((rx_desc[eth.rx_head].des3 & EQOS_RDES3_OWN) == 0U) qt_rearm();
    }
    qt_debug();
    printf("Observed RX fill states mask=%lX. MANUAL verdict required: correlate\r\n"
           "PAUSE assertion/release and hysteresis with configured free-space levels.\r\n",
           (unsigned long)levels);
}
