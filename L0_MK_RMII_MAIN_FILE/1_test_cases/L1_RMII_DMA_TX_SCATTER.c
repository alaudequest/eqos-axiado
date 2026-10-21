static void eth_queue_test_run(void)
{
    printf("\r\n========== DMA TX scatter-gather ==========\r\n");
    if (!qt_roundtrip(128U, true)) return;
    /* Send frames until the next TX slot is the last slot in the ring.
     * Do not change tx_head directly: it must stay in step with DMA. */
    while (eth.tx_head != EQOS_TX_DESC_COUNT - 1U)
        if (!qt_roundtrip(60U, false)) return;
    if (!qt_roundtrip(1514U, true)) return; /* last descriptor -> descriptor 0 */
    qt_expect_drop(); /* Two TX descriptors should produce only one RX frame. */
    (void)qt_roundtrip(128U, false);
}
