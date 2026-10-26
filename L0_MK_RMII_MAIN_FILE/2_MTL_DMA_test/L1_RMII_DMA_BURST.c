static void eth_queue_test_run(void)
{
    printf("\r\n========== DMA burst length / bus contention ==========\r\n");
    qt_check(((ETH->DMACTCR & ETH_DMACTCR_TPBL) >> ETH_DMACTCR_TPBL_Pos) == QUEUE_TEST_PBL,
             "TX PBL readback");
    qt_check(((ETH->DMACRCR & ETH_DMACRCR_RPBL) >> ETH_DMACRCR_RPBL_Pos) == QUEUE_TEST_PBL,
             "RX PBL readback");
    uint32_t start = DWT->CYCCNT;
    uint32_t max_cycles = 0U;
    for (uint32_t i = 0U; i < 128U; ++i) {
        uint32_t before = DWT->CYCCNT;
        if (!qt_roundtrip(1514U, false)) return;
        uint32_t cycles = DWT->CYCCNT - before;
        if (cycles > max_cycles) max_cycles = cycles;
    }
    uint32_t cycles = DWT->CYCCNT - start;
    printf("PBL=%u, 128 frames, total cycles=%lu max roundtrip cycles=%lu CPU Hz=%lu\r\n",
           (unsigned int)QUEUE_TEST_PBL, (unsigned long)cycles,
           (unsigned long)max_cycles, (unsigned long)SystemCoreClock);
    printf("MANUAL: repeat PBL=1,2,4,8,16,32 under independent DMA/MDMA bus load.\r\n"
           "Compare application throughput/latency with your system limits.\r\n"
           "Polling/construct/compare overhead is included; this is not wire throughput.\r\n");
}
