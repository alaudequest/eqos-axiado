/* This test checks received frame data automatically.
 * Checking when TX starts needs a separate tool that can delay DMA bus reads. */
static void eth_queue_test_run(void)
{
    printf("\r\n========== MTL TX store-and-forward ==========\r\n");
    qt_check((ETH->MTLTQOMR & ETH_MTLTQOMR_TSF) != 0U, "TSF readback");
    const uint32_t sizes[] = {60U, 128U, 512U, 1514U};
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
        if (!qt_roundtrip(sizes[i], true)) return;
    qt_debug();
    printf("MANUAL: delay DMA bus delivery mid-frame with a bus exerciser; correlate\r\n"
           "MTL TX read state [0x0D08] and wire capture with final data arrival.\r\n"
           "Transmission must start only after the complete frame is buffered.\r\n"
           "This timing criterion remains unverified by the basic loopback run.\r\n");
}
