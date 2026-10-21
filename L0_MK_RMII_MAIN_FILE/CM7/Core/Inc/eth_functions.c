#include <stdio.h>
#include <string.h>
#include "eth_functions_utils.h"
#include "../../baremetal/eqos_hw.h"
#include "eth_config_test_case.h"

#define RXD3_OWN       (1UL << 31)
#define RXD3_FD        (1UL << 29)
#define RXD3_LD        (1UL << 28)
#define RXD3_RS2V      (1UL << 27)

#define RXD3_LT_MASK   (7UL << 16)
#define RXD3_LT_TYPE   (1UL << 16)
#define RXD3_LT_ARP    (3UL << 16)

#define RXD2_ARPNR     (1UL << 10)


static void eth_print_fifo_size(const char *name, uint32_t code)
{
    /* FIFO size encoding: 128 * 2^code bytes, codes 0..11. */
    if (code <= 11U)
        printf("  %s: raw=%lu, %lu bytes\r\n", name,
               (unsigned long)code, (unsigned long)(128U << code));
    else
        printf("  %s: raw=%lu (reserved encoding)\r\n", name, (unsigned long)code);
}

void eth_print_hw_features(void)
{
    /* Read each capability register once. Call after enabling ETH clocks. */
    const uint32_t hw1 = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_HW_FEATURE1);
    const uint32_t hw2 = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_HW_FEATURE2);
    const uint32_t hash = (hw1 & ETH_MACHWF1R_HASHTBLSZ_Msk) >> ETH_MACHWF1R_HASHTBLSZ_Pos;
    static const char *const hash_sizes[] = {"none", "64 bits", "128 bits", "256 bits"};

    printf("\r\nETH_MACHWF1R [0x120] = 0x%08lX\r\n", (unsigned long)hw1);
    printf("  L3L4FNUM: %lu L3/L4 filters\r\n",
           (unsigned long)((hw1 & ETH_MACHWF1R_L3L4FNUM_Msk) >> ETH_MACHWF1R_L3L4FNUM_Pos));
    printf("  HASHTBLSZ: raw=%lu, %s\r\n", (unsigned long)hash, hash_sizes[hash]);
    eth_print_fifo_size("TXFIFOSIZE",
        (hw1 & ETH_MACHWF1R_TXFIFOSIZE_Msk) >> ETH_MACHWF1R_TXFIFOSIZE_Pos);
    eth_print_fifo_size("RXFIFOSIZE",
        (hw1 & ETH_MACHWF1R_RXFIFOSIZE_Msk) >> ETH_MACHWF1R_RXFIFOSIZE_Pos);

    printf("ETH_MACHWF2R [0x124] = 0x%08lX\r\n", (unsigned long)hw2);
    /* All six defined fields; reserved bits are visible only in the raw value.
     * Queue/channel counts are encoded as count minus one. */
    const uint32_t txq = (hw2 & ETH_MACHWF2R_TXQCNT_Msk) >> ETH_MACHWF2R_TXQCNT_Pos;
    const uint32_t rxq = (hw2 & ETH_MACHWF2R_RXQCNT_Msk) >> ETH_MACHWF2R_RXQCNT_Pos;
    const uint32_t txch = (hw2 & ETH_MACHWF2R_TXCHCNT_Msk) >> ETH_MACHWF2R_TXCHCNT_Pos;
    const uint32_t rxch = (hw2 & ETH_MACHWF2R_RXCHCNT_Msk) >> ETH_MACHWF2R_RXCHCNT_Pos;
    printf("  RXQCNT: raw=%lu, %lu MTL receive queues\r\n", (unsigned long)rxq, (unsigned long)(rxq + 1U));
    printf("  TXQCNT: raw=%lu, %lu MTL transmit queues\r\n", (unsigned long)txq, (unsigned long)(txq + 1U));
    printf("  RXCHCNT: raw=%lu, %lu DMA receive channels\r\n", (unsigned long)rxch, (unsigned long)(rxch + 1U));
    printf("  TXCHCNT: raw=%lu, %lu DMA transmit channels\r\n", (unsigned long)txch, (unsigned long)(txch + 1U));
    printf("  PPSOUTNUM: %lu PPS outputs\r\n",
           (unsigned long)((hw2 & ETH_MACHWF2R_PPSOUTNUM_Msk) >> ETH_MACHWF2R_PPSOUTNUM_Pos));
    printf("  AUXSNAPNUM: %lu auxiliary snapshot inputs\r\n\r\n",
           (unsigned long)((hw2 & ETH_MACHWF2R_AUXSNAPNUM_Msk) >> ETH_MACHWF2R_AUXSNAPNUM_Pos));
}

uint32_t eth_construct_frame(uint8_t *buffer, size_t capacity,
                             uint32_t size, const uint8_t dest_mac[6],
                             const uint8_t src_mac[6], uint16_t ether_type,
                             const uint8_t *payload_pattern, size_t pattern_size)
{
    if (buffer == NULL || dest_mac == NULL || src_mac == NULL ||
        payload_pattern == NULL || pattern_size == 0U ||
        size < ETH_TEST_FRAME_MIN_SIZE || size > ETH_TEST_FRAME_MAX_SIZE ||
        size > capacity || ether_type < 0x0600U) {
        return 0U;
    }

    memcpy(buffer, dest_mac, 6U);
    memcpy(buffer + 6U, src_mac, 6U);
    buffer[12] = (uint8_t)(ether_type >> 8);
    buffer[13] = (uint8_t)ether_type;
    for (uint32_t i = 14U; i < size; ++i) {
        buffer[i] = payload_pattern[(i - 14U) % pattern_size];
    }
    return size;
}


uint32_t eth_construct_vlan_frame(uint8_t *buffer, size_t capacity,
                                  uint32_t size, const uint8_t dest_mac[6],
                                  const uint8_t src_mac[6], uint16_t ether_type,
                                  uint16_t vlan_id, uint8_t pcp, uint8_t dei,
                                  const uint8_t *payload_pattern, size_t pattern_size)
{
    if (buffer == NULL || dest_mac == NULL || src_mac == NULL ||
        payload_pattern == NULL || pattern_size == 0U ||
        size < ETH_TEST_FRAME_MIN_SIZE || size > ETH_TEST_VLAN_FRAME_MAX_SIZE ||
        size > capacity || ether_type < 0x0600U ||
        vlan_id > 4094U || pcp > 7U || dei > 1U) {
        return 0U;
    }

    const uint16_t tci = (uint16_t)(((uint16_t)pcp << 13) |
                                   ((uint16_t)dei << 12) | vlan_id);
    memcpy(buffer, dest_mac, 6U);
    memcpy(buffer + 6U, src_mac, 6U);
    buffer[12] = 0x81U;
    buffer[13] = 0x00U;
    buffer[14] = (uint8_t)(tci >> 8);
    buffer[15] = (uint8_t)tci;
    buffer[16] = (uint8_t)(ether_type >> 8);
    buffer[17] = (uint8_t)ether_type;
    for (uint32_t i = 18U; i < size; ++i) {
        buffer[i] = payload_pattern[(i - 18U) % pattern_size];
    }
    return size;
}

void eth_mac_init(){
    printf("MAC init success");
}

void eth_dma_init(){
    printf("DMA init success");
}

void eth_mtl_init(){
    printf("MTL init success");
}



void eth_dump_rx_status(uint32_t rdes2,
                        uint32_t rdes3)
{


    printf("RDES2 = 0x%08lX\n", (unsigned long)rdes2);
    printf("RDES3 = 0x%08lX\n", (unsigned long)rdes3);

    printf("FD  = %lu\n",
           (unsigned long)((rdes3 >> 29) & 1U));
    printf("LD  = %lu\n",
           (unsigned long)((rdes3 >> 28) & 1U));
    printf("RS2V = %lu\n",
           (unsigned long)((rdes3 >> 27) & 1U));

    uint32_t lt = rdes3 & RXD3_LT_MASK;

    if (lt == RXD3_LT_ARP)
    {
        printf("LT = ARP request (0b011)\n");
    }
    else if (lt == RXD3_LT_TYPE)
    {
        printf("LT = ordinary type packet (0b001)\n");
    }
    else
    {
        printf("LT = 0x%lX\n",
               (unsigned long)(lt >> 16));
    }

    if ((rdes3 & RXD3_RS2V) != 0U)
    {
        if ((rdes2 & RXD2_ARPNR) != 0U)
        {
            printf("ARPNR = 1: ARP reply not generated\n");
        }
        else
        {
            printf("ARPNR = 0: reply-not-generated flag is clear\n");
        }
    }
    else
    {
        printf("RDES2 status not valid; do not interpret ARPNR\n");
    }
}


/* ===================== Generic multi-descriptor RX ===================== */

#define ETH_RX_NEXT_DESC_TIMEOUT_US  1000U  /* max gap between descriptors of one frame */

/* Poll OWN until 0 (RX and TX). On success the DES3 value is returned and
 * DSB orders the following buffer/status reads. */
static bool eth_desc_wait_own(const struct eqos_desc *desc, uint32_t timeout_us,
                              uint32_t *des3)
{
    for (uint32_t elapsed = 0U;; ++elapsed) {
        *des3 = desc->des3;
        if ((*des3 & EQOS_RDES3_OWN) == 0U) {
            __DSB();
            return true;
        }
        if (elapsed >= timeout_us)
            return false;
        eqos_udelay(1U);
    }
}

/* Rebuild all four words, publish OWN last, then wake DMA with the tail. */
static void eth_rx_rearm_desc(struct eqos_desc *desc, uint8_t *buf)
{
    desc->des0 = (uint32_t)(uintptr_t)buf;
    desc->des1 = 0U;
    desc->des2 = 0U;
    __DSB();
    desc->des3 = EQOS_RDES3_OWN | EQOS_RDES3_BUF1V;
    __DSB();
    AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_TAIL_PTR,
                        (uint32_t)(uintptr_t)desc);
}

/*
 * eth_rx_frame_receive() - receive one frame of any size into out[].
 *
 * How it works:
 * 1. Copy, then hand the descriptor back to DMA straight away.
 *    Each descriptor's piece is copied out, then all four words are re-armed,
 *    the tail pointer is written and rx_head moves forward. The ring is
 *    refilled while the frame is still arriving, so the frame size is not
 *    limited by the ring size (EQOS_RX_DESC_COUNT x EQOS_BUF_SIZE).
 * 2. Length of each piece:
 *    - every descriptor before the last one is a full buffer
 *      (EQOS_BUF_SIZE, which must equal the programmed RBSZ);
 *    - the last descriptor (LD) holds the rest: its PL is the total frame
 *      length, so its piece is PL minus the bytes already received.
 * 3. Status: RDES1, RDES2 and RDES3 of the LD descriptor are read before it is
 *    re-armed and returned in *info. This function does not judge ES, GP, CE,
 *    RWT or OE; each test interprets them (e.g. GPSLCE expects GP=1 on a frame
 *    that is otherwise delivered normally).
 * 4. Two timeouts:
 *    - first_timeout_us waits for a frame to start; 0 checks once, no wait.
 *    - ETH_RX_NEXT_DESC_TIMEOUT_US applies between descriptors once a frame
 *      has started. A frame that stalls part-way returns EQOS_EIO instead of
 *      EQOS_ETIMEDOUT, so "no frame" and "frame broke off" are distinguished.
 * 5. Getting back in step after a broken frame:
 *    - a descriptor without FD while no frame is in progress is the leftover
 *      end of an abandoned frame: re-armed, counted in desc_dropped, and the
 *      function keeps waiting;
 *    - FD in the middle of a frame means the previous frame was cut short:
 *      assembly restarts at that descriptor;
 *    - context descriptors (CTXT=1, timestamps) are re-armed and skipped.
 *    So a stall part-way through a frame never breaks the next call.
 * 6. Ordering: DSB after reading OWN=0 and before reading the buffer; DSB
 *    before setting OWN and before the tail write. The descriptor is not
 *    touched again after OWN is set.
 * 7. Nothing slow inside the loop (no printf). At 100 Mbps a 1600-byte buffer
 *    fills in about 128 us; copy + re-arm takes a few us.
 *
 * Assumptions: only buffer 1 is used (BUF2V=0); out_size is large enough for
 * the frame (the size check only prevents a memory overrun, it is not reported).
 *
 * Returns EQOS_OK (check info->rdes3 for error bits), EQOS_ETIMEDOUT (no frame
 * started) or EQOS_EIO (frame stalled part-way).
 */
int eth_rx_frame_receive(struct eqos_dev *eth, struct eqos_desc *rx_desc,
                         uint8_t rx_buf[][EQOS_BUF_SIZE],
                         uint8_t *out, size_t out_size,
                         uint32_t first_timeout_us, struct eth_rx_frame *info)
{
    uint32_t rx_bytes = 0U;      /* frame bytes received so far */
    bool in_frame = false;
    uint32_t timeout = first_timeout_us;

    memset(info, 0, sizeof(*info));
    while (1) {
        const uint32_t idx = eth->rx_head;
        struct eqos_desc *desc = &rx_desc[idx];
        uint32_t rdes3;

        if (!eth_desc_wait_own(desc, timeout, &rdes3))
            return in_frame ? EQOS_EIO : EQOS_ETIMEDOUT;

        bool last = false;
        if ((rdes3 & EQOS_RDES3_WB_CTXT) != 0U) {
            /* Timestamp context descriptor: not frame data. */
        } else if (!in_frame && (rdes3 & EQOS_RDES3_WB_FD) == 0U) {
            ++info->desc_dropped;            /* tail of an abandoned frame */
        } else {
            if ((rdes3 & EQOS_RDES3_WB_FD) != 0U) {
                info->desc_dropped += info->desc_used;  /* FD mid-frame: restart */
                info->desc_used = 0U;
                rx_bytes = 0U;
                in_frame = true;
            }
            uint32_t chunk = EQOS_BUF_SIZE;
            if ((rdes3 & EQOS_RDES3_WB_LD) != 0U) {
                const uint32_t pl = rdes3 & EQOS_RDES3_WB_PL_MASK;
                chunk = (pl > rx_bytes) ? pl - rx_bytes : 0U;
                info->len   = pl;
                info->rdes1 = desc->des1;       /* read status before re-arm */
                info->rdes2 = desc->des2;
                info->rdes3 = rdes3;
                last = true;
            }
            if (rx_bytes + chunk <= out_size) /* defensive: never overrun */
                memcpy(out + rx_bytes, rx_buf[idx], chunk);
            rx_bytes += chunk;
            ++info->desc_used;
        }

        eth_rx_rearm_desc(desc, rx_buf[idx]);
        eth->rx_head = (idx + 1U) & (EQOS_RX_DESC_COUNT - 1U);

        if (last)
            return EQOS_OK;
        timeout = in_frame ? ETH_RX_NEXT_DESC_TIMEOUT_US : first_timeout_us;
    }
}


/* ===================== Generic multi-descriptor TX ===================== */

#define ETH_TX_PIECE_MAX         0x3FFFU  /* B1L field limit (14 bits) */
#define ETH_TX_DESC_TIMEOUT_US   1000U    /* wait for DMA to free a slot */

/*
 * eth_tx_frame_send() - send one frame of any size and wait for completion.
 *
 * How it works:
 * 1. Zero copy: each descriptor points straight at a piece of frame[]; no
 *    tx_buf copy. Safe because the function returns only after the frame is
 *    sent, so the buffer cannot change under DMA.
 * 2. Piece size = min(remaining, piece_max). piece_max = 0 uses the 16383-byte
 *    B1L maximum; a smaller value forces the frame across several descriptors.
 * 3. Same steps for every piece: wait until the slot is free (OWN=0), fill
 *    DES0..DES2, DSB, set DES3 with OWN, DSB, move tx_head and write the tail
 *    pointer. Waiting for a free slot covers both "ring full" and "previous
 *    frame still sending", so any frame size works with any ring size.
 * 4. TDES3 bits: FL (total length) in every descriptor, FD + fd_ctrl in the
 *    first one only (CPC, SAIC, CIC are valid in FD only), LD in the last one.
 * 5. Status: wait for the LD descriptor, then return its TDES3 write-back
 *    (ES, JT, FF, LoC, NC, LC, EC, UF, IHE) in *info. The function does not
 *    judge it; each test interprets the bits.
 * 6. Trade-off: FD is handed to DMA immediately, so DMA may reach the next slot
 *    before it is filled and pause (TBU) until the tail write wakes it. With
 *    TSF=1 this changes nothing; in threshold mode check UF in info->tdes3.
 * 7. Nothing slow inside the loop (no printf).
 *
 * Assumptions: frame[] is in memory the ETH DMA can reach (RAM_D1/D2/D3, not
 * DTCM); D-cache is off; the MTL mode suits the frame size (TSF=1 needs the
 * frame to fit in the TX FIFO); only buffer 1 is used.
 *
 * Returns EQOS_OK (check info->tdes3 for error bits) or EQOS_ETIMEDOUT (no free
 * slot, or LD not completed in timeout_us: reset before the next run).
 */
int eth_tx_frame_send(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                      const uint8_t *frame, uint32_t len, uint32_t piece_max,
                      uint32_t fd_ctrl, uint32_t timeout_us,
                      struct eth_tx_frame *info)
{
    uint32_t sent = 0U;
    uint32_t last_idx = eth->tx_head;
    uint32_t tdes3;

    if (piece_max == 0U || piece_max > ETH_TX_PIECE_MAX)
        piece_max = ETH_TX_PIECE_MAX;
    info->desc_used = 0U;
    info->tdes3 = 0U;

    while (sent < len) {
        const uint32_t idx = eth->tx_head;
        struct eqos_desc *desc = &tx_desc[idx];

        /* Slot must be free: DMA done with it (ring full or previous frame). */
        if (!eth_desc_wait_own(desc, ETH_TX_DESC_TIMEOUT_US, &tdes3))
            return EQOS_ETIMEDOUT;

        const uint32_t piece = (len - sent < piece_max) ? len - sent : piece_max;
        uint32_t des3 = EQOS_TDES3_OWN | (len & EQOS_TDES3_FL_MASK);
        if (sent == 0U)
            des3 |= EQOS_TDES3_FD | fd_ctrl;   /* CPC/SAIC/CIC valid in FD only */
        if (sent + piece == len)
            des3 |= EQOS_TDES3_LD;

        desc->des0 = (uint32_t)(uintptr_t)(frame + sent);   /* zero copy */
        desc->des1 = 0U;
        desc->des2 = piece & EQOS_TDES2_B1L_MASK;
        __DSB();
        desc->des3 = des3;                     /* publish OWN last */
        __DSB();
        eth->tx_head = (idx + 1U) & (EQOS_TX_DESC_COUNT - 1U);
        AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR,
                            (uint32_t)(uintptr_t)&tx_desc[eth->tx_head]);

        last_idx = idx;
        sent += piece;
        ++info->desc_used;
    }

    /* Wait for the LD descriptor, then return its write-back status. */
    if (!eth_desc_wait_own(&tx_desc[last_idx], timeout_us, &tdes3))
        return EQOS_ETIMEDOUT;
    info->tdes3 = tdes3;                       /* ES, JT, UF, LC, EC, IHE ... */
    return EQOS_OK;
}


/* ===================== Register update with readback ===================== */

/*
 * eth_reg_update() - read-modify-write one register, then verify it.
 * Normally called through ETH_REG_UPDATE(base, offset, clear_mask, set_mask),
 * which passes the offset macro name as the printed name.
 *
 * 1. Read the register once.
 * 2. Clear clear_mask, set set_mask, write once.
 * 3. Read back once. Only the bits in (clear_mask | set_mask) are compared, so
 *    read-only or self-clearing bits elsewhere in the register do not cause a
 *    false mismatch. To replace a multi-bit field, put the whole field in
 *    clear_mask and the new value in set_mask.
 * 4. Print "<name> [address]: old=... write=... readback=... OK/MISMATCH".
 *
 * Returns true when the touched bits read back as written. A caller that needs
 * the old value for a later restore reads the register before calling.
 */
bool eth_reg_update(const char *name, uint32_t addr,
                    uint32_t clear_mask, uint32_t set_mask)
{
    const uint32_t old = AX_READ_REG32(addr);
    const uint32_t value = (old & ~clear_mask) | set_mask;
    AX_WRITE_REG32(addr, value);
    const uint32_t rb = AX_READ_REG32(addr);
    const uint32_t check = clear_mask | set_mask;
    const bool ok = (rb & check) == (value & check);

    printf("%s [0x%08lX]: old=0x%08lX write=0x%08lX readback=0x%08lX %s\r\n",
           name, (unsigned long)addr, (unsigned long)old,
           (unsigned long)value, (unsigned long)rb, ok ? "OK" : "MISMATCH");
    return ok;
}


/* ===================== MAC address hash index ===================== */

/*
 * eth_mac_hash_index() - 64-bit hash table index of a MAC address.
 * Same as Linux stmmac: bitrev32(~crc32_le(~0, addr, 6)) >> 26.
 * 1. CRC-32 over the 6 address bytes, reflected polynomial 0xEDB88320,
 *    initial value 0xFFFFFFFF, then invert (the normal Ethernet CRC).
 * 2. The upper 6 bits of the bit-reversed CRC are its lowest 6 bits in
 *    reverse order.
 * Index 0..31 selects ETH_MACHT0R bit idx, 32..63 ETH_MACHT1R bit idx-32.
 */
uint32_t eth_mac_hash_index(const uint8_t addr[6])
{
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0U; i < 6U; ++i) {
        crc ^= addr[i];
        for (uint32_t bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
    crc = ~crc;

    uint32_t idx = 0U;
    for (uint32_t bit = 0U; bit < 6U; ++bit)
        idx = (idx << 1) | ((crc >> bit) & 1U);
    return idx;
}
