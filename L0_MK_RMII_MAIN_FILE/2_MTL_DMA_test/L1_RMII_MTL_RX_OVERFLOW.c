static void eth_queue_test_run(void)
{
    printf("\r\n========== MTL RX FIFO overflow / recovery ==========\r\n");
    /* Stop returning RX descriptors. DMA runs out of free buffers,
     * then the RX FIFO fills up. EHFC=0 disables automatic flow control
     * so it cannot pause the sender before the FIFO overflows. */
    (void)qt_starve_rx();
}
