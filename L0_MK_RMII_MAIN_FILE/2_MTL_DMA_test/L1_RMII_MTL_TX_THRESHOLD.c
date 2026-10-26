static void eth_queue_test_run(void)
{
    printf("\r\n========== MTL TX threshold / underflow ==========\r\n");
    uint32_t saved = ETH->MTLTQOMR;
    uint32_t underflows = 0U;
    /* Change the threshold after TX finishes, while the TX queue is empty. */
    for (uint32_t threshold = 0; threshold < 8U; ++threshold) {
        ETH->MTLTQOMR = (saved & ~(ETH_MTLTQOMR_TSF | ETH_MTLTQOMR_TTC)) |
                         (threshold << ETH_MTLTQOMR_TTC_Pos);
        qt_check((ETH->MTLTQOMR & ETH_MTLTQOMR_TTC) ==
                 (threshold << ETH_MTLTQOMR_TTC_Pos), "TTC readback");
        qt_frame(1514U);
        uint32_t status;
        if (!qt_send(0U, false, &status)) return;
        printf("TTC=%lu TDES3=%08lX\r\n", (unsigned long)threshold, (unsigned long)status);
        if ((status & ETH_DMATXNDESCWBF_UF) != 0U) {
            ++underflows;
            qt_check((status & EQOS_TDES3_WB_ES) != 0U, "UF without error summary");
            qt_expect_drop(); /* FEP=0: RX should drop the failed frame. */
            qt_debug();
        } else {
            qt_check((status & EQOS_TDES3_WB_ES) == 0U, "unexpected TX error");
            if (!qt_receive(frame, frame_len, false)) return;
        }
        /* Send a small frame to check that TX and RX still work at this threshold. */
        if (!qt_roundtrip(60U, false)) return;
    }
    ETH->MTLTQOMR = saved;
    printf("Underflows observed=%lu. Repeat with external bus contention.\r\n",
           (unsigned long)underflows);
    if (underflows == 0U)
        printf("INCOMPLETE: underrun injection/recovery criterion not exercised.\r\n");
}
