#include "eth_functions_utils.h"
#include "../CM7/baremetal/eqos_hw.h"
#include <stdio.h>

static uint32_t crc_test_previous_count;

uint32_t eth_test_mac_rx_crc_error(struct eqos_dev *eth, uint8_t *buffer,
                                  size_t capacity)
{
    printf("\n ========== Begin MAC RX CRC Error test ==========\n");
    if (eth == NULL)
        return 0U;
    const uint8_t pattern[] = {0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
    uint32_t length = eth_construct_frame(buffer, capacity, 64U,
        eth->cfg.mac_addr, eth->cfg.mac_addr, 0x88B5U, pattern, sizeof(pattern));
    if (length == 0U)
        return 0U;

    /* Compute the Ethernet FCS over 60 bytes, then flip one bit so failure
     * is guaranteed. Bytes 60..63 contain the deliberately corrupted FCS. */
    uint32_t crc = 0xFFFFFFFFU;
    for (unsigned int i = 0; i < 60U; ++i) {
        crc ^= buffer[i];
        for (unsigned int bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
    crc = (~crc) ^ 1U;
    for (unsigned int i = 0; i < 4U; ++i)
        buffer[60U + i] = (uint8_t)(crc >> (8U * i));

    eth->cfg.loopback = true;
    eth->cfg.promiscuous = false;
    AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_PACKET_FILTER, 0U);
    /* Preserve the requested FEP=0: bad frames are counted but not delivered. */
    uint32_t rx_mode = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE);
    rx_mode &= ~ETH_MTLRQOMR_FEP;
    rx_mode |= EQOS_MTL_RXQ_RSF;
    AX_BASE_WRITE_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_OPERATION_MODE, rx_mode);
    /* Use cumulative, unfrozen counters so the report can compute a delta. */
    ETH->MMCCR &= ~(ETH_MMCCR_RSTONRD | ETH_MMCCR_CNTFREEZ);
    crc_test_previous_count = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_CRC_ERROR);
    printf("64 bytes including bad FCS; FEP=0. Press USER; expect CRC count +1, no RX descriptor.\r\n");
    return length;
}

uint32_t eth_test_crc_tx_control(void)
{
    return EQOS_TDES3_CPC; /* Disable CRC insertion before publishing OWN. */
}

void eth_test_crc_report(void)
{
    /* Called after TX, independently of RX descriptor ownership. */
    eqos_udelay(1000U);
    uint32_t count = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_RX_CRC_ERROR);
    printf("Rx_CRC_Error_Count [0x794]=%lu, delta=%lu\r\n",
           (unsigned long)count, (unsigned long)(count - crc_test_previous_count));
    crc_test_previous_count = count;
}
