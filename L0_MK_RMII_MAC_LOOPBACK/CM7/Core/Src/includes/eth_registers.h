#ifndef ETH_REGISTERS_H
#define ETH_REGISTERS_H

#include <stdint.h>
#include "mac_defines.h"   // register offsets (R_*), bits and field values

/* Unsigned shift so BIT(31) is valid */
#ifndef BIT
#define BIT(n) (1U << (n))
#endif

/* The AX_* accessors and ETH_DSB() are #ifndef-guarded so a PC-side test can supply its own. */

/**
 * @brief Write 32bit value to a given register (base + offset)
 * @param reg - absolute address of the register / port
 * @param val - value to be written
 */
#ifndef AX_WRITE_REG32
#define AX_WRITE_REG32(reg, val) (*(volatile uint32_t *)((uintptr_t)(reg)) = (uint32_t)(val))
#endif
/**
 * @brief Write 32bit value to a given register (base + offset)
 * @param base - base address of the register
 * @param offset - relative address of the register / port
 * @param val - value to be written
 */
#define AX_BASE_WRITE_REG32(base, offset, val) AX_WRITE_REG32((uintptr_t)(base) + (uintptr_t)(offset), (val))

/**
 * @brief Read 32bit value to a given register (base + offset)
 * @param reg - absolute address of the register / port
 * @return 32bit value of the register
 */
#ifndef AX_READ_REG32
#define AX_READ_REG32(reg) (*(volatile uint32_t *)((uintptr_t)(reg)))
#endif

/**
 * @brief Read 32bit value to a given register (base + offset)
 * @param base - base address of the register
 * @param offset - relative address of the register / port
 * @return 32bit value of the register
 */
#define AX_BASE_READ_REG32(base, offset) AX_READ_REG32((uintptr_t)(base) + (uintptr_t)(offset))

/**
 * @brief Memory barrier: descriptor words must reach RAM before OWN is set,
 * and OWN before the tail pointer write that wakes the DMA ("dsb sy" = CMSIS __DSB()).
 */
#ifndef ETH_DSB
#define ETH_DSB() __asm__ volatile ("dsb sy" ::: "memory")
#endif

/* STM32H755 memory map, RM0399 Table 7 p.140: ETHERNET MAC 0x4002 8000 - 0x4002 93FF.
 * One peripheral = Synopsys EQoS MAC (+MMC) at 0x0000, MTL at 0x0C00, DMA at 0x1000.
 * Register offsets (R_*, mac_defines.h) are relative to this base, same as the RM0399 tables. */
#define STM32_BASE_ADRS_ETH1    0x40028000
#define EQOS_BASE               STM32_BASE_ADRS_ETH1

/* Total number of maximum MAC: STM32H755 has one Ethernet MAC (ETH1) -> mac_idx = 0 */
#define MAX_MAC_CNT 1

/* Byte offset between two MAC instances. Only one on STM32H755, so every mac_idx maps to ETH1 */
#define MAC_CONFIG_BYTE_CNT 0x0000

/* Base address of the given MAC (MAC + MTL + DMA registers) */
#define MAC_BASE_ADRS(mac_idx)  (EQOS_BASE + ((mac_idx) * MAC_CONFIG_BYTE_CNT))

/*------------------------------------------------------------------------------*/
/* MAC 100M configuration: values written by mac_init_100m() (eth_functions.c)
 * Bring-up order RM0399 §61.9: DMA p.3031, MTL p.3032, MAC p.3032-3033.
 *
 * 2300-byte frame / RX watchdog test (log COM3_2026_10_20):
 *   ETH_TEST_TRAFFIC_SIZE 2286 (eth_traffic_test.h) -> frame 2300 bytes, 2304 with FCS
 *   mac_basic_init() MAC_WD_JB_TIMEOUT_WTO_2KB -> watchdog cuts RX at 2048 bytes: RDES3.RWT = 1 -> FAIL (expected)
 *                    MAC_WD_JB_TIMEOUT_WTO_3KB -> 3072 bytes                               -> PASS
 *   MAC_Configuration.JE = 1 and TSF = RSF = 0 (mac_mtl_init()) are already set for it (frame > 2 KB MTL FIFO).
 */

/* Descriptor rings: number of descriptors (power of 2) and bytes per buffer.
 * MAC_100M_BUF_SIZE: multiple of 32 and <= 0x3FFF (RBSZ field); 2304 = 2300-byte frame + 4 FCS */
#define MAC_100M_TX_DESC_CNT    4
#define MAC_100M_RX_DESC_CNT    8
#define MAC_100M_BUF_SIZE       2304

/* Built field by field in eth_functions.c:
 *   mac_dma_init()     DMA_CH0_Tx_Control, DMA_CH0_Rx_Control, DMA_CH0_Control, DMA_SysBus_Mode, DMA_CH0_Interrupt_Enable
 *   mac_mtl_init()     MTL_TxQ0_Operation_Mode, MTL_RxQ0_Operation_Mode
 *   mac_basic_init()   MAC_Packet_Filter, MAC_Address0_High/LR (from ETH_TEST_SRC_MAC), MAC_WD_JB_Timeout, MAC_Configuration */

/*------------------------------------------------------------------------------*/
/* MMC statistics counters, offset from MAC_BASE_ADRS (RM0399 p.3156-3165).
 * STM32H755 only has these MMC counters: no RX total, multicast or broadcast counter. */
#define MMC_STATS_TX_SINGLE_COL_GOOD    0x074C  /* Tx_Single_Collision_Good_Packets   p.3162 */
#define MMC_STATS_TX_MULTI_COL_GOOD     0x0750  /* Tx_Multiple_Collision_Good_Packets p.3162 */
#define MMC_STATS_TX_PKT_GOOD           0x0768  /* Tx_Packet_Count_Good               p.3163 */
#define MMC_STATS_RX_CRC_ERR            0x0794  /* Rx_CRC_Error_Packets               p.3163 */
#define MMC_STATS_RX_ALIGN_ERR          0x0798  /* Rx_Alignment_Error_Packets         p.3163 */
#define MMC_STATS_RX_UNICAST_GOOD       0x07C4  /* Rx_Unicast_Packets_Good            p.3164 */
#define MMC_STATS_TX_LPI_USEC           0x07EC  /* Tx_LPI_USEC_Cntr                   p.3164 */
#define MMC_STATS_TX_LPI_TRAN           0x07F0  /* Tx_LPI_Tran_Cntr                   p.3165 */
#define MMC_STATS_RX_LPI_USEC           0x07F4  /* Rx_LPI_USEC_Cntr                   p.3165 */
#define MMC_STATS_RX_LPI_TRAN           0x07F8  /* Rx_LPI_Tran_Cntr                   p.3165 */
#define AX_READ_MAC_REG32(mac, x)  AX_BASE_READ_REG32(MAC_BASE_ADRS(mac), x)

#endif // ETH_REGISTERS_H
