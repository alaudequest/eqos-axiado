static void eth_queue_test_run(void)
{
    printf("\r\n========== DMA ownership / ring wrap ==========\r\n");
    /* Send fewer frames per group than the number of TX descriptors.
     * This keeps the tail and head different. Wait until the group is sent
     * before checking RX and returning its descriptors to DMA. */
    const uint32_t batch = EQOS_TX_DESC_COUNT - 1U;
    static uint8_t expected[EQOS_TX_DESC_COUNT][128];
    const uint32_t packets = QUEUE_TEST_WRAPS * EQOS_RX_DESC_COUNT;
    for (uint32_t sent = 0U; sent < packets;) {
        uint32_t n = packets - sent < batch ? packets - sent : batch;
        for (uint32_t i = 0U; i < n; ++i) {
            qt_frame(128U);
            memcpy(expected[i], frame, frame_len);
            uint32_t status;
            if (!qt_send(0U, false, &status)) return;
            qt_check((status & EQOS_TDES3_WB_ES) == 0U, "wrap TX error");
        }
        eqos_udelay(100U);
        for (uint32_t i = 0U; i < n; ++i)
            if (!qt_receive(expected[i], 128U, false)) return;
        sent += n;
        if ((sent % 3000U) == 0U) printf("Verified %lu packets\r\n", (unsigned long)sent);
    }
    qt_expect_drop(); /* No extra frame should arrive after the last group. */
    printf("Verified %lu packets, %lu RX wraps, %lu TX wraps.\r\n",
           (unsigned long)packets, (unsigned long)QUEUE_TEST_WRAPS,
           (unsigned long)(packets / EQOS_TX_DESC_COUNT));
    printf("MANUAL: use MPU/watchpoints for accesses beyond ring storage.\r\n");
}
