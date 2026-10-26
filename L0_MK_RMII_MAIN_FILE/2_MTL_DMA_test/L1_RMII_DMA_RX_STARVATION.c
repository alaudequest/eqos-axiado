static void eth_queue_test_run(void)
{
    printf("\r\n========== DMA RX descriptor starvation / resume ==========\r\n");
    (void)qt_starve_rx();
}
