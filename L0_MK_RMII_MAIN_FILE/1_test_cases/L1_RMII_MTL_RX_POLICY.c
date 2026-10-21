static void eth_queue_test_run(void)
{
    printf("\r\n========== MTL RX forwarding policy ==========\r\n");
    uint32_t saved = ETH->MTLRQOMR;
    ETH->MMCCR &= ~(ETH_MMCCR_RSTONRD | ETH_MMCCR_CNTFREEZ);
    /* Try all four FEP/FUP settings with RX store-and-forward enabled.
     * FEP allows frames with errors; FUP allows short frames with a good FCS. */
    for (uint32_t policy = 0; policy < 4U; ++policy) {
        ETH->MTLRQOMR = (saved & ~(ETH_MTLRQOMR_FEP | ETH_MTLRQOMR_FUP)) |
            ETH_MTLRQOMR_RSF | ((policy & 1U) ? ETH_MTLRQOMR_FEP : 0U) |
            ((policy & 2U) ? ETH_MTLRQOMR_FUP : 0U);
        printf("FEP=%lu FUP=%lu\r\n", (unsigned long)(policy & 1U),
               (unsigned long)((policy >> 1) & 1U));
        if (!qt_roundtrip(60U, false)) return;
        for (uint32_t bad_crc = 0; bad_crc < 2U; ++bad_crc) {
            /* Short frame: 40 bytes plus 4 correct FCS bytes.
             * Bad-FCS frame: 60 bytes plus 4 incorrect FCS bytes.
             * CPC=2 stops hardware from adding padding or generating the FCS. */
            uint32_t data_len = bad_crc ? 60U : 40U;
            qt_frame(data_len);
            uint32_t crc = qt_fcs(frame, data_len) ^ bad_crc;
            for (uint32_t i = 0; i < 4U; ++i)
                frame[data_len + i] = (uint8_t)(crc >> (8U * i));
            frame_len += 4U;
            uint32_t before = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_CRC_ERROR);
            uint32_t status;
            if (!qt_send(EQOS_TDES3_CPC, false, &status)) return;
            qt_check((status & EQOS_TDES3_WB_ES) == 0U, "raw FCS TX completion");
            bool forward = bad_crc ? ((policy & 1U) != 0U) : ((policy & 2U) != 0U);
            if (forward) {
                if (!qt_receive(frame, data_len, bad_crc != 0U)) return;
            } else {
                qt_expect_drop();
            }
            eqos_udelay(100U);
            uint32_t after = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_CRC_ERROR);
            qt_check(after - before == bad_crc, "CRC counter delta");
            if (!qt_roundtrip(128U, false)) return;
        }
    }
    ETH->MTLRQOMR = saved;
}
