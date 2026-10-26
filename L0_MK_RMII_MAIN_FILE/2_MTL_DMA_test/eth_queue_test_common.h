#ifndef ETH_QUEUE_TEST_COMMON_H
#define ETH_QUEUE_TEST_COMMON_H

/* main.c includes this file after declaring the rings and buffers.
 * Do not compile the test .c files separately. Each descriptor is 16 bytes,
 * and the data cache is disabled. Every wait has a time limit.
 * After a timeout, do not change memory that DMA may still be using. */
#define QT_TIMEOUT_US 100000U
static unsigned int qt_failures;
static uint32_t qt_sequence;

static inline void qt_check_guards(void)
{
    for (uint32_t i = 0U; i < 8U; ++i) {
        if (qt_tx_ring.before[i] != 0xA55A6996U || qt_tx_ring.after[i] != 0xA55A6996U ||
            qt_rx_ring.before[i] != 0xA55A6996U || qt_rx_ring.after[i] != 0xA55A6996U) {
            ++qt_failures;
            printf("FAIL: descriptor ring guard overwritten\r\n");
            return;
        }
    }
}

static inline void qt_check(bool ok, const char *what)
{
    if (!ok) {
        ++qt_failures;
        printf("FAIL: %s\r\n", what);
    }
}

static inline void qt_debug(void)
{
    const uint32_t tx = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_DEBUG);
    const uint32_t rx = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_DEBUG);
    const uint32_t dma = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_DEBUG_STATUS);
    printf("MTL TX[0x0D08]=%08lX RX[0x0D38]=%08lX DMA[0x100C]=%08lX CSR=%08lX\r\n",
           (unsigned long)tx, (unsigned long)rx, (unsigned long)dma,
           (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
    /* Debug registers show the state at the moment they are read.
     * Check descriptors to confirm that a transfer finished.
     * Do not read DMACMFCR here: reading it clears the missed-frame count. */
}

static inline void qt_frame(uint32_t length)
{
    const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    frame_len = eth_construct_frame(frame, sizeof(frame), length,
        eth.cfg.mac_addr, eth.cfg.mac_addr, 0x88B5U, pattern, sizeof(pattern));
    ++qt_sequence;
    /* Give each frame a different number to detect old or repeated RX data. */
    for (uint32_t i = 0; i < 4U; ++i)
        frame[14U + i] = (uint8_t)(qt_sequence >> (8U * i));
}

static inline bool qt_wait(struct eqos_desc *desc, uint32_t *status)
{
    for (uint32_t us = 0; us < QT_TIMEOUT_US; ++us) {
        *status = desc->des3;
        if ((*status & EQOS_TDES3_OWN) == 0U) {
            __DSB();
            return true;
        }
        eqos_udelay(1U);
    }
    qt_check(false, "descriptor OWN timeout; reset before rerunning");
    for (uint32_t i = 0U; i < EQOS_TX_DESC_COUNT; ++i)
        if (desc == &tx_desc[i])
            printf("Waiting for TX[%lu] @ %p DES3=%08lX\r\n",
                   (unsigned long)i, (void *)desc, (unsigned long)desc->des3);
    for (uint32_t i = 0U; i < EQOS_RX_DESC_COUNT; ++i)
        if (desc == &rx_desc[i])
            printf("Waiting for RX[%lu] @ %p DES3=%08lX\r\n",
                   (unsigned long)i, (void *)desc, (unsigned long)desc->des3);
    qt_debug();
    printf("TX current=%08lX tail=%08lX; RX current=%08lX tail=%08lX\r\n",
           (unsigned long)ETH->DMACCATDR, (unsigned long)ETH->DMACTDTPR,
           (unsigned long)ETH->DMACCARDR, (unsigned long)ETH->DMACRDTPR);
    printf("MACCR=%08lX DMACTCR=%08lX DMACRCR=%08lX\r\n",
           (unsigned long)ETH->MACCR, (unsigned long)ETH->DMACTCR,
           (unsigned long)ETH->DMACRCR);
    return false;
}

static inline void qt_rearm(void)
{
    struct eqos_desc *rx = &rx_desc[eth.rx_head];
    rx->des0 = (uint32_t)(uintptr_t)rx_buf[eth.rx_head];
    rx->des1 = 0U;
    rx->des2 = 0U;
    __DSB();
    rx->des3 = EQOS_RDES3_OWN | EQOS_RDES3_BUF1V;
    __DSB();
    AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_TAIL_PTR,
                        (uint32_t)(uintptr_t)rx);
    eth.rx_head = (eth.rx_head + 1U) & (EQOS_RX_DESC_COUNT - 1U);
}

static inline bool qt_receive(const uint8_t *expected, uint32_t length, bool crc_error)
{
    uint32_t status;
    if (!qt_wait(&rx_desc[eth.rx_head], &status))
        return false;
    const uint32_t actual = status & EQOS_RDES3_WB_PL_MASK;
    bool ok = (status & (EQOS_RDES3_WB_CTXT | EQOS_RDES3_WB_FD | EQOS_RDES3_WB_LD)) ==
              (EQOS_RDES3_WB_FD | EQOS_RDES3_WB_LD);
    ok = ok && (((status & EQOS_RDES3_WB_ES) != 0U) == crc_error);
    if (crc_error)
        ok = ok && ((status & ETH_DMARXNDESCWBF_CE) != 0U);
    /* CST removes the four FCS bytes, so the RX length should not include them. */
    ok = ok && actual == length && actual <= EQOS_BUF_SIZE;
    if (ok)
        ok = memcmp(rx_buf[eth.rx_head], expected, length) == 0;
    qt_check(ok, "RX length, FD/LD, error status or complete payload mismatch");
    if (!ok)
        printf("RDES3=%08lX expected length=%lu\r\n", (unsigned long)status,
               (unsigned long)length);
    qt_rearm();
    return ok;
}

static inline bool qt_send(uint32_t control, bool split, uint32_t *result)
{
    const uint32_t first = eth.tx_head;
    const uint32_t last = split ? ((first + 1U) & (EQOS_TX_DESC_COUNT - 1U)) : first;
    if ((tx_desc[first].des3 & EQOS_TDES3_OWN) != 0U ||
        (tx_desc[last].des3 & EQOS_TDES3_OWN) != 0U) {
        qt_check(false, "attempted reuse of DMA-owned TX descriptor");
        return false;
    }
    const uint32_t count = split ? 2U : 1U;
    const uint32_t part = split ? 31U : frame_len;
    for (uint32_t n = 0; n < count; ++n) {
        uint32_t idx = (first + n) & (EQOS_TX_DESC_COUNT - 1U);
        uint32_t offset = n == 0U ? 0U : part;
        uint32_t length = n == 0U ? part : frame_len - part;
        memcpy(tx_buf[idx], frame + offset, length);
        tx_desc[idx].des0 = (uint32_t)(uintptr_t)tx_buf[idx];
        tx_desc[idx].des1 = 0U;
        tx_desc[idx].des2 = length;
        tx_desc[idx].des3 = (n == 0U ? EQOS_TDES3_FD | frame_len | control : 0U) |
                              (idx == last ? EQOS_TDES3_LD : 0U);
    }
    __DSB();
    /* Give DMA the second descriptor before the first one.
     * Both parts must be ready before DMA starts reading the frame. */
    tx_desc[last].des3 |= EQOS_TDES3_OWN;
    __DSB();
    if (split)
        tx_desc[first].des3 |= EQOS_TDES3_OWN;
    __DSB();
    eth.tx_head = (last + 1U) & (EQOS_TX_DESC_COUNT - 1U);
    AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR,
                        (uint32_t)(uintptr_t)&tx_desc[eth.tx_head]);
    if (!qt_wait(&tx_desc[last], result))
        return false;
    uint32_t first_result;
    if (split && !qt_wait(&tx_desc[first], &first_result))
        return false;
    return true;
}

static inline bool qt_roundtrip(uint32_t length, bool split)
{
    qt_frame(length);
    uint32_t status;
    if (!qt_send(0U, split, &status))
        return false;
    qt_check((status & EQOS_TDES3_WB_ES) == 0U, "TX error summary");
    return qt_receive(frame, length, false) && (status & EQOS_TDES3_WB_ES) == 0U;
}

static inline void qt_expect_drop(void)
{
    /* Wait up to 2 ms and check that no frame arrives.
     * The test also sends a good frame to check that RX still works. */
    for (uint32_t us = 0; us < 2000U; ++us) {
        if ((rx_desc[eth.rx_head].des3 & EQOS_RDES3_OWN) == 0U) {
            qt_check(false, "frame was delivered despite discard policy");
            qt_rearm();
            return;
        }
        eqos_udelay(1U);
    }
}

static inline uint32_t qt_fcs(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
    return ~crc;
}

/* Stop returning RX descriptors to DMA so the RX ring and FIFO fill up.
 * Internal loopback supplies the frames; no external sender is needed. */
static inline bool qt_starve_rx(void)
{
    static uint8_t held[EQOS_RX_DESC_COUNT][EQOS_BUF_SIZE];
    uint32_t held_status[EQOS_RX_DESC_COUNT];
    uint32_t status;
    (void)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_MISSED_FRAME_COUNT);
    (void)ETH->MTLRQMPOCR; /* Reading clears the old overflow/missed counts. */
    /* Clear old RBU once, before filling the ring. Keep any RBU event
     * raised during this test until it has been checked below. */
    AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS, ETH_DMACSR_RBU);
    qt_frame(512U);
    printf("Step 1: fill RX ring, keeping completed descriptors with the CPU.\r\n");
    for (uint32_t i = 0; i < EQOS_RX_DESC_COUNT; ++i) {
        if (i == EQOS_RX_DESC_COUNT - 1U) {
            /* The initial tail points to the last RX slot. DMA can stop
             * there even though that slot still has OWN=1. Move the tail
             * to slot 0 so DMA can fill the last slot and then wrap.
             * Slots 0..N-2 already have OWN=0: do not rearm them yet. */
            __DSB();
            AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_TAIL_PTR,
                                (uint32_t)(uintptr_t)&rx_desc[0]);
        }
        if (!qt_send(0U, false, &status)) {
            printf("Stopped during fill TX for RX slot %lu.\r\n", (unsigned long)i);
            return false;
        }
        if (!qt_wait(&rx_desc[i], &held_status[i])) {
            printf("Stopped while filling RX slot %lu.\r\n", (unsigned long)i);
            return false;
        }
        qt_check((status & EQOS_TDES3_WB_ES) == 0U, "fill TX failed");
        qt_check((held_status[i] & (EQOS_RDES3_WB_ES | EQOS_RDES3_WB_PL_MASK)) == frame_len,
                 "initial RX fill status/length");
        qt_check(memcmp(rx_buf[i], frame, frame_len) == 0, "initial RX fill data");
        memcpy(held[i], rx_buf[i], EQOS_BUF_SIZE);
    }
    printf("Step 1 complete: %lu RX descriptors held by CPU.\r\n",
           (unsigned long)EQOS_RX_DESC_COUNT);
    /* DMA may already have reported RBU after filling the last slot.
     * Do not clear it here: clearing the flag does not restart DMA,
     * and more incoming frames need not set it again while DMA is suspended. */
    printf("CSR after ring fill=%08lX (RBU preserved for the final check)\r\n",
           (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS));
    /* Send 64 more 512-byte frames, enough to overflow the H755 RX FIFO.
     * Keep the MAC receiver running during this step. */
    printf("Step 2: send 64 more frames without returning RX descriptors.\r\n");
    for (uint32_t i = 0; i < 64U; ++i) {
        if (!qt_send(0U, false, &status)) {
            printf("Stopped during overflow TX frame %lu.\r\n", (unsigned long)i);
            return false;
        }
        qt_check((status & EQOS_TDES3_WB_ES) == 0U, "overflow stimulus TX");
    }
    eqos_udelay(2000U);
    qt_debug();
    uint32_t missed = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_MISSED_FRAME_COUNT);
    uint32_t fifo = ETH->MTLRQMPOCR;
    printf("DMA missed[0x116C]=%08lX MTL overflow[0x0D34]=%08lX (read once)\r\n",
           (unsigned long)missed, (unsigned long)fifo);
    qt_check((AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS) & ETH_DMACSR_RBU) != 0U,
             "RX buffer unavailable not observed");
    qt_check((missed & (ETH_DMACMFCR_MFC | ETH_DMACMFCR_MFCO)) != 0U ||
             (fifo & (ETH_MTLRQMPOCR_OVFPKTCNT | ETH_MTLRQMPOCR_OVFCNTOVF)) != 0U,
             "no DMA/MTL loss accounting under overflow");
    #if ACTIVE_TEST == TEST_RMII_MTL_RX_OVERFLOW
    qt_check((fifo & (ETH_MTLRQMPOCR_OVFPKTCNT | ETH_MTLRQMPOCR_OVFCNTOVF)) != 0U,
             "MTL FIFO overflow not observed");
    #endif
    for (uint32_t i = 0; i < EQOS_RX_DESC_COUNT; ++i) {
        qt_check(rx_desc[i].des3 == held_status[i] &&
                 memcmp(held[i], rx_buf[i], EQOS_BUF_SIZE) == 0,
                 "DMA changed a CPU-owned RX buffer/descriptor");
    }
    /* Step 3: let RX run again.
     * The CPU still holds all RX descriptors from Step 1. Their data was
     * checked above, so we can now return each descriptor to DMA.
     * qt_rearm() restores the buffer address, sets OWN=1, updates the tail,
     * and moves rx_head to the next slot. After one full ring, rx_head
     * comes back to the first slot. Do not send any new frames yet. */
    printf("Step 3: return RX descriptors and receive the frames left in the FIFO.\r\n");
    for (uint32_t i = 0; i < EQOS_RX_DESC_COUNT; ++i)
        qt_rearm();

    /* DMA now copies the frames left in the FIFO into the RX buffers.
     * OWN=0 means a frame is ready for the CPU to check.
     * qt_receive() checks its length, error flags and all data bytes,
     * then returns that descriptor to DMA. These frames still contain
     * the 512-byte pattern sent in Step 2.
     * Finish after 2 ms with no new frame. Stop with an error if the
     * whole drain step takes 100 ms, so this loop cannot wait forever. */
    uint32_t received = 0U;
    uint32_t start_ms = HAL_GetTick();
    uint32_t last_rx_ms = start_ms;
    while ((uint32_t)(HAL_GetTick() - last_rx_ms) < 2U) {
        if ((uint32_t)(HAL_GetTick() - start_ms) >= 100U) {
            qt_check(false, "Step 3: RX did not finish draining within 100 ms");
            return false;
        }
        if ((rx_desc[eth.rx_head].des3 & EQOS_RDES3_OWN) == 0U) {
            if (!qt_receive(frame, frame_len, false)) {
                printf("Step 3 FAIL: a queued frame failed its RX checks.\r\n");
                return false;
            }
            ++received;
            last_rx_ms = HAL_GetTick();
        } else {
            eqos_udelay(1U);
        }
    }
    printf("Step 3 complete: checked %lu queued frames.\r\n", (unsigned long)received);

    /* Step 4: use a new sequence number and a different frame length.
     * qt_roundtrip() checks TX completion, RX status, length and all
     * 128 received bytes. Any failed check also increases qt_failures.
     * This PASS/FAIL message applies to Step 4 only; main prints the
     * failure count for the whole test afterward. */
    printf("Step 4: send and check a new 128-byte frame after recovery.\r\n");
    if (!qt_roundtrip(128U, false)) {
        printf("Step 4 FAIL: the new frame failed TX or RX checks.\r\n");
        return false;
    }
    printf("Step 4 PASS: TX completed; RX status, length and all 128 bytes match.\r\n");
    return true;
}

#endif
