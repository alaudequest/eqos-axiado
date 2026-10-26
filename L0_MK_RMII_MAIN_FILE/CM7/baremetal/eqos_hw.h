/* SPDX-License-Identifier: GPL-2.0 */
/*
 * eqos_hw.h - Synopsys DesignWare Ethernet QoS: register map & bit definitions
 *
 * Chi la dinh nghia thanh ghi. Khong phu thuoc vao bat ky OS/framework nao.
 *
 * Offset lay tu dwc_eth_qos.h cua U-Boot. Cac thanh ghi duoc danh dau
 * [THEM] khong co trong file goc, lay tu Synopsys databook - hay doi chieu
 * lai voi TRM cua SoC ban dung truoc khi tin dung.
 */

#ifndef EQOS_HW_H
#define EQOS_HW_H

#include <stdint.h>

/*
 * STM32H755 CMSIS mapping:
 *   ETH_BASE = ETH_MAC_BASE = 0x40028000 (absolute peripheral address).
 *   EQOS_MAC_BASE / EQOS_MTL_BASE / EQOS_DMA_BASE are absolute addresses.
 *   Each register constant is a byte offset relative to its matching block.
 *   AX_BASE_*_REG32 adds that block base and relative offset once.
 */
#if defined(STM32H755xx)
#include "stm32h7xx.h"
#define EQOS_BASE_ADDR                  ETH_BASE
#endif

/* Unsigned shift supports bit 31; n must be in the range 0..31. */
#ifndef BIT
#define BIT(n) (1U << (n))
#endif

/** @brief Write a 32-bit value to an absolute register address. */
#define AX_WRITE_REG32(reg, val) \
    (*(volatile uint32_t *)((uintptr_t)(reg)) = (uint32_t)(val))

/** @brief Write a 32-bit value to a register at base + byte offset. */
#define AX_BASE_WRITE_REG32(base, offset, val) \
    AX_WRITE_REG32((uintptr_t)(base) + (uintptr_t)(offset), (val))

/** @brief Read a 32-bit value from an absolute register address. */
#define AX_READ_REG32(reg) \
    (*(volatile uint32_t *)((uintptr_t)(reg)))

/** @brief Read a 32-bit value from a register at base + byte offset. */
#define AX_BASE_READ_REG32(base, offset) \
    AX_READ_REG32((uintptr_t)(base) + (uintptr_t)(offset))

/* ------------------------------------------------------------------ */
/* MAC block: absolute base; register offsets below are MAC-relative. */
/* ------------------------------------------------------------------ */
#define EQOS_MAC_BASE                   (EQOS_BASE_ADDR + 0x0000u)

#define EQOS_MAC_CONFIGURATION          0x000u
#define EQOS_MAC_EXT_CONFIGURATION      0x004u
#define EQOS_MAC_PACKET_FILTER          0x008u
#define EQOS_MAC_WATCHDOG_TIMEOUT       0x00cu
#define EQOS_MAC_Q0_TX_FLOW_CTRL        0x070u
#define EQOS_MAC_RX_FLOW_CTRL           0x090u
#define EQOS_MAC_RXQ_CTRL4              0x094u
#define EQOS_MAC_TXQ_PRTY_MAP0          0x098u
#define EQOS_MAC_TXQ_PRTY_MAP1          0x09cu
#define EQOS_MAC_RXQ_CTRL0              0x0a0u
#define EQOS_MAC_RXQ_CTRL1              0x0a4u
#define EQOS_MAC_RXQ_CTRL2              0x0a8u
#define EQOS_MAC_RXQ_CTRL3              0x0acu
#define EQOS_MAC_RX_TX_STATUS           0x0b8u      /* MAC_Rx_Tx_Status, clear on read */
#define EQOS_MAC_US_TIC_COUNTER         0x0dcu
#define EQOS_MAC_VERSION                0x110u
#define EQOS_MAC_DEBUG                  0x114u
#define EQOS_MAC_HW_FEATURE0            0x11cu
#define EQOS_MAC_HW_FEATURE1            0x120u
#define EQOS_MAC_HW_FEATURE2            0x124u
#define EQOS_MAC_HW_FEATURE3            0x128u
#define EQOS_MAC_TS_CONTROL             0xb00u      /* MAC_Timestamp_Control */
#define EQOS_MAC_SUB_SEC_INCR           0xb04u      /* MAC_Sub_Second_Increment */
#define EQOS_MAC_TS_ADDEND              0xb18u      /* MAC_Timestamp_Addend */

/* MAC_TS_CONTROL */
#define EQOS_MAC_TSC_TSENA              (1u << 0)   /* timestamp enable */
#define EQOS_MAC_TSC_TSCFUPDT           (1u << 1)   /* fine update */
#define EQOS_MAC_TSC_TSINIT             (1u << 2)   /* init system time, self-clears */
#define EQOS_MAC_TSC_TSADDREG           (1u << 5)   /* load addend, self-clears */
#define EQOS_MAC_TSC_TSENALL            (1u << 8)   /* timestamp all RX packets */
#define EQOS_MAC_TSC_TSCTRLSSR          (1u << 9)   /* sub-second rolls over at 1e9 (ns) */
#define EQOS_MAC_SSIR_SSINC_SHIFT       16          /* sub-second increment [23:16] */
#define EQOS_MAC_MDIO_ADDRESS           0x200u
#define EQOS_MAC_MDIO_DATA              0x204u
#define EQOS_MAC_ARP_ADDRESS            0x210u
#define EQOS_MAC_ADDRESS0_HIGH          0x300u
#define EQOS_MAC_ADDRESS0_LOW           0x304u
#define EQOS_MAC_HASH_TABLE0            0x010u      /* ETH_MACHT0R */
#define EQOS_MAC_HASH_TABLE1            0x014u      /* ETH_MACHT1R */
#define EQOS_MAC_ADDRESS1_HIGH          0x308u
#define EQOS_MAC_ADDRESS1_LOW           0x30Cu
#define EQOS_MAC_ADDR_HIGH_AE           (1u << 31)  /* address enable */
#define EQOS_MAC_RX_CRC_ERROR           0x794u
#define EQOS_MAC_RX_UNI_GOOD            0x7C4u

/* MAC_CONFIGURATION */
#define EQOS_MAC_CFG_ARPEN                (1u << 31)     /*ARP offload enable*/
#define EQOS_MAC_CFG_SARC_SHIFT         28u         /* SA insertion/replacement ctrl */
#define EQOS_MAC_CFG_SARC_MASK          (7u << 28)
#define EQOS_MAC_CFG_SARC_INS_ADDR0     2u          /* 010: insert MAC Address0 */
#define EQOS_MAC_CFG_SARC_REP_ADDR0     3u          /* 011: replace with MAC Address0 */
#define EQOS_MAC_CFG_SARC_INS_ADDR1     6u          /* 110: insert MAC Address1 */
#define EQOS_MAC_CFG_SARC_REP_ADDR1     7u          /* 111: replace with MAC Address1 */
#define EQOS_MAC_CFG_IPC                (1u << 27)     /*Checksum offload of TCP/UDP payload */
#define EQOS_MAC_CFG_GPSLCE             (1u << 23)  /* giant packet size limit ctrl */
#define EQOS_MAC_CFG_S2KP               (1u << 22)  /* IEEE 802.3as 2K packets */
#define EQOS_MAC_CFG_CST                (1u << 21)  /* CRC stripping for Type frames */
#define EQOS_MAC_CFG_ACS                (1u << 20)  /* auto pad/CRC strip */
#define EQOS_MAC_CFG_WD                 (1u << 19)  /* watchdog disable */
#define EQOS_MAC_CFG_JD                 (1u << 17)  /* jabber disable */
#define EQOS_MAC_CFG_JE                 (1u << 16)  /* jumbo enable */
#define EQOS_MAC_CFG_PS                 (1u << 15)  /* port select: 1 = MII (10/100) */
#define EQOS_MAC_CFG_FES                (1u << 14)  /* fast eth speed: 1 = 100M */
#define EQOS_MAC_CFG_DM                 (1u << 13)  /* duplex mode: 1 = full */
#define EQOS_MAC_CFG_LM                 (1u << 12)  /* loopback */
#define EQOS_MAC_CFG_TE                 (1u << 1)   /* transmitter enable */
#define EQOS_MAC_CFG_RE                 (1u << 0)   /* receiver enable */

/* MAC_EXT_CONFIGURATION (ETH_MACECR) */
#define EQOS_MAC_EXT_CFG_GPSL_MASK      0x3fffu     /* giant packet size limit, bytes */
#define EQOS_MAC_EXT_CFG_SPEN           (1u << 17)  /* slow protocol detection enable */
#define EQOS_MAC_EXT_CFG_USP            (1u << 18)  /* unicast slow protocol detect */

/* MAC_WATCHDOG_TIMEOUT (MAC_Watchdog_Timeout, ETH_MACWTR) */
#define EQOS_MAC_WTO_MASK               0xfu        /* limit = (WTO + 2) x 1024 bytes */
#define EQOS_MAC_WTO_PWE                (1u << 8)   /* programmable watchdog enable */

/* MAC_RX_TX_STATUS (MAC_Rx_Tx_Status, ETH_MACRXTXSR) */
#define EQOS_MAC_RXTX_STS_RWT           (1u << 8)   /* receive watchdog timeout */
#define EQOS_MAC_RXTX_STS_TJT           (1u << 0)   /* transmit jabber timeout */

/* MAC_PACKET_FILTER */
#define EQOS_MAC_PKT_FILTER_VTFE        (1u << 16)  /* VLAN tag filter enable */
#define EQOS_MAC_PKT_FILTER_PCF_MASK    (3u << 6)   /* pass control packets */
#define EQOS_MAC_PKT_FILTER_PCF_ALL     (2u << 6)   /* 10: forward all control packets */
#define EQOS_MAC_PKT_FILTER_PR          (1u << 0)   /* promiscuous */
#define EQOS_MAC_PKT_FILTER_HUC         (1u << 1)   /* hash unicast */
#define EQOS_MAC_PKT_FILTER_HMC         (1u << 2)   /* hash multicast */
#define EQOS_MAC_PKT_FILTER_DAIF        (1u << 3)   /* DA inverse filtering */
#define EQOS_MAC_PKT_FILTER_PM          (1u << 4)   /* pass all multicast */
#define EQOS_MAC_PKT_FILTER_HPF         (1u << 10)  /* hash or perfect filter */
#define EQOS_MAC_PKT_FILTER_RA          (1u << 31)  /* receive all */
#define EQOS_MAC_PKT_FILTER_DNTU          (1u << 21)   /* Drop Non-TCP or UDP packets */
#define EQOS_MAC_PKT_FILTER_IPFE          (1u << 20)   /* Layer 3 and 4 filter enable */

/* STM32H755 MAC VLAN Tag Register (single outer C-VLAN). */
#define EQOS_MAC_VLAN_TAG               0x050u
#define EQOS_MAC_VLAN_TAG_VL_MASK       0xffffu
#define EQOS_MAC_VLAN_TAG_ETV           (1u << 16)
#define EQOS_MAC_VLAN_TAG_VTIM          (1u << 17)
#define EQOS_MAC_VLAN_TAG_EVLS_MASK     (3u << 21)
#define EQOS_MAC_VLAN_TAG_EVLRXS        (1u << 24)

/* MAC_Q0_TX_FLOW_CTRL */
#define EQOS_MAC_TXFC_PT_SHIFT          16          /* pause time */
#define EQOS_MAC_TXFC_PT_MASK           0xffffu
#define EQOS_MAC_TXFC_TFE               (1u << 1)   /* tx flow control enable */
#define EQOS_MAC_TXFC_FCB_BPA           (1u << 0)   /* send pause frame (FD), self-clears */

/* MAC_RX_FLOW_CTRL */
#define EQOS_MAC_RXFC_RFE               (1u << 0)   /* rx flow control enable */

/* MAC_TXQ_PRTY_MAP0 */
#define EQOS_MAC_TXQ_PRTY_PSTQ0_SHIFT   0
#define EQOS_MAC_TXQ_PRTY_PSTQ0_MASK    0xffu

/* MAC_RXQ_CTRL0 */
#define EQOS_MAC_RXQ0EN_SHIFT           0
#define EQOS_MAC_RXQ0EN_MASK            0x3u
#define EQOS_MAC_RXQ0EN_DISABLED        0u
#define EQOS_MAC_RXQ0EN_AV              1u          /* AV (802.1Qav) mode */
#define EQOS_MAC_RXQ0EN_DCB             2u          /* DCB/generic mode */

/* MAC_RXQ_CTRL1 */
#define EQOS_MAC_RXQ_CTRL1_MCBCQEN      (1u << 20)  /* mcast/bcast -> queue */

/* MAC_RXQ_CTRL2 */
#define EQOS_MAC_RXQ_CTRL2_PSRQ0_SHIFT  0
#define EQOS_MAC_RXQ_CTRL2_PSRQ0_MASK   0xffu

/* MAC_HW_FEATURE1 - FIFO size, ma hoa log2(n/128) */
#define EQOS_HWF1_TXFIFOSIZE_SHIFT      6
#define EQOS_HWF1_TXFIFOSIZE_MASK       0x1fu
#define EQOS_HWF1_RXFIFOSIZE_SHIFT      0
#define EQOS_HWF1_RXFIFOSIZE_MASK       0x1fu

/* MAC_MDIO_ADDRESS */
#define EQOS_MDIO_ADDR_PA_SHIFT         21          /* PHY address  [25:21] */
#define EQOS_MDIO_ADDR_PA_MASK          0x1fu
#define EQOS_MDIO_ADDR_RDA_SHIFT        16          /* reg / devad  [20:16] */
#define EQOS_MDIO_ADDR_RDA_MASK         0x1fu
#define EQOS_MDIO_ADDR_CR_SHIFT         8           /* CSR clk range [11:8] */
#define EQOS_MDIO_ADDR_CR_MASK          0xfu
#define EQOS_MDIO_ADDR_SKAP             (1u << 4)   /* skip address packet */
#define EQOS_MDIO_ADDR_GOC_SHIFT        2           /* [3:2] */
#define EQOS_MDIO_ADDR_GOC_MASK         0x3u
#define EQOS_MDIO_ADDR_GOC_WRITE        1u
#define EQOS_MDIO_ADDR_GOC_READ         3u
#define EQOS_MDIO_ADDR_C45E             (1u << 1)   /* clause 45 enable */
#define EQOS_MDIO_ADDR_GB               (1u << 0)   /* GMII busy */

/* CSR clock range (CR field). 4 gia tri duoi day co trong U-Boot, phan con
 * lai lay tu databook. */
#define EQOS_MDIO_CR_60_100             0u
#define EQOS_MDIO_CR_100_150            1u
#define EQOS_MDIO_CR_20_35              2u
#define EQOS_MDIO_CR_35_60              3u   /* [THEM] */
#define EQOS_MDIO_CR_150_250            4u
#define EQOS_MDIO_CR_250_300            5u
#define EQOS_MDIO_CR_300_500            6u   /* [THEM] */
#define EQOS_MDIO_CR_500_800            7u   /* [THEM] */

/* MAC_MDIO_DATA */
#define EQOS_MDIO_DATA_RA_SHIFT         16          /* register addr (C45) */
#define EQOS_MDIO_DATA_GD_MASK          0xffffu

/* ------------------------------------------------------------------ */
/* MTL queue 0 window: absolute base; offsets below are queue-relative. */
/* ------------------------------------------------------------------ */
#define EQOS_MTL_BASE                   (EQOS_BASE_ADDR + 0x0d00u)

#define EQOS_MTL_TXQ0_OPERATION_MODE    0x00u
#define EQOS_MTL_TXQ0_DEBUG             0x08u
#define EQOS_MTL_TXQ0_QUANTUM_WEIGHT    0x18u
#define EQOS_MTL_RXQ0_OPERATION_MODE    0x30u
#define EQOS_MTL_RXQ0_DEBUG             0x38u

/* MTL_TXQ0_OPERATION_MODE */
#define EQOS_MTL_TXQ_TQS_SHIFT          16          /* tx queue size */
#define EQOS_MTL_TXQ_TQS_MASK           0x1ffu
#define EQOS_MTL_TXQ_TXQEN_SHIFT        2
#define EQOS_MTL_TXQ_TXQEN_MASK         0x3u
#define EQOS_MTL_TXQ_TXQEN_ENABLED      2u
#define EQOS_MTL_TXQ_TTC_SHIFT          4           /* tx threshold control */
#define EQOS_MTL_TXQ_TTC_MASK           (7u << 4)
#define EQOS_MTL_TXQ_TTC_64             (1u << 4)   /* 001: 64 bytes */
#define EQOS_MTL_TXQ_TSF                (1u << 1)   /* tx store & forward */
#define EQOS_MTL_TXQ_FTQ                (1u << 0)   /* flush tx queue */

/* MTL_TXQ0_DEBUG */
#define EQOS_MTL_TXQ_DEBUG_TXQSTS       (1u << 4)   /* tx queue not empty */
#define EQOS_MTL_TXQ_DEBUG_TRCSTS_SHIFT 1
#define EQOS_MTL_TXQ_DEBUG_TRCSTS_MASK  0x3u

/* MTL_RXQ0_OPERATION_MODE */
#define EQOS_MTL_RXQ_RQS_SHIFT          20          /* rx queue size */
#define EQOS_MTL_RXQ_RQS_MASK           0x3ffu
#define EQOS_MTL_RXQ_RFD_SHIFT          14          /* threshold tat flow ctrl */
#define EQOS_MTL_RXQ_RFD_MASK           0x3fu
#define EQOS_MTL_RXQ_RFA_SHIFT          8           /* threshold bat flow ctrl */
#define EQOS_MTL_RXQ_RFA_MASK           0x3fu
#define EQOS_MTL_RXQ_EHFC               (1u << 7)   /* enable hw flow control */
#define EQOS_MTL_RXQ_RSF                (1u << 5)   /* rx store & forward */
#define EQOS_MTL_RXQ_RTC_MASK           3u          /* rx threshold control */
#define EQOS_MTL_RXQ_RTC_64             0u          /* 00: 64 bytes */
#define EQOS_MTL_RXQ_FEP                (1u << 4)   /* forward error packets */
#define EQOS_MTL_RXQ_DISTCPEF           (1u << 6)   /* H755: keep TCP/IP checksum-error packets */

/* MTL_RXQ0_DEBUG */
#define EQOS_MTL_RXQ_DEBUG_PRXQ_SHIFT   16          /* so packet trong rx queue */
#define EQOS_MTL_RXQ_DEBUG_PRXQ_MASK    0x7fffu
#define EQOS_MTL_RXQ_DEBUG_RXQSTS_SHIFT 4
#define EQOS_MTL_RXQ_DEBUG_RXQSTS_MASK  0x3u

/* ------------------------------------------------------------------ */
/* DMA block: absolute base; register offsets below are DMA-relative. */
/* ------------------------------------------------------------------ */
#define EQOS_DMA_BASE                   (EQOS_BASE_ADDR + 0x1000u)

#define EQOS_DMA_MODE                   0x000u
#define EQOS_DMA_SYSBUS_MODE            0x004u
#define EQOS_DMA_DEBUG_STATUS           0x00cu /* absolute offset 0x100C */
#define EQOS_DMA_CH0_MISSED_FRAME_COUNT 0x16cu /* absolute offset 0x116C */
#define EQOS_DMA_CH0_CONTROL            0x100u
#define EQOS_DMA_CH0_TX_CONTROL         0x104u
#define EQOS_DMA_CH0_RX_CONTROL         0x108u
#define EQOS_DMA_CH0_TXDESC_LIST_HADDR  0x110u
#define EQOS_DMA_CH0_TXDESC_LIST_ADDR   0x114u
#define EQOS_DMA_CH0_RXDESC_LIST_HADDR  0x118u
#define EQOS_DMA_CH0_RXDESC_LIST_ADDR   0x11cu
#define EQOS_DMA_CH0_TXDESC_TAIL_PTR    0x120u
#define EQOS_DMA_CH0_RXDESC_TAIL_PTR    0x128u
#define EQOS_DMA_CH0_TXDESC_RING_LEN    0x12cu
#define EQOS_DMA_CH0_RXDESC_RING_LEN    0x130u
#define EQOS_DMA_CH0_INTERRUPT_ENABLE  0x134u
#define EQOS_DMA_CH0_STATUS             0x160u  /* [THEM] debug */
/* DMA_CH0_STATUS bits (write 1 to clear) */
#define EQOS_DMA_CH_STATUS_RWT          (1u << 9)   /* receive watchdog timeout */
#define EQOS_DMA_CH_STATUS_AIS          (1u << 14)  /* abnormal interrupt summary */

/* DMA_MODE */
#define EQOS_DMA_MODE_SWR               (1u << 0)   /* software reset */

/* DMA_SYSBUS_MODE */
/* STM32H755 AHB variant; do not substitute AXI BLEN/EAME fields below. */
#define EQOS_DMA_SYSBUS_AHB_FB          (1u << 0)   /* fixed burst length */
#define EQOS_DMA_SYSBUS_AHB_AAL         (1u << 12)  /* address-aligned beats */
#define EQOS_DMA_SYSBUS_RD_OSR_LMT_SHIFT 16
#define EQOS_DMA_SYSBUS_RD_OSR_LMT_MASK 0xfu
#define EQOS_DMA_SYSBUS_EAME            (1u << 11)  /* enable 40-bit addressing */
#define EQOS_DMA_SYSBUS_BLEN16          (1u << 3)
#define EQOS_DMA_SYSBUS_BLEN8           (1u << 2)
#define EQOS_DMA_SYSBUS_BLEN4           (1u << 1)

/* DMA_CH0_CONTROL */
#define EQOS_DMA_CH_CTRL_DSL_SHIFT      18          /* descriptor skip length */
#define EQOS_DMA_CH_CTRL_DSL_MASK       0x7u
#define EQOS_DMA_CH_CTRL_PBLX8          (1u << 16)

/* DMA_CH0_TX_CONTROL */
#define EQOS_DMA_CH_TX_TXPBL_SHIFT      16
#define EQOS_DMA_CH_TX_TXPBL_MASK       0x3fu
#define EQOS_DMA_CH_TX_OSP              (1u << 4)   /* operate on second packet */
#define EQOS_DMA_CH_TX_ST               (1u << 0)   /* start transmit */

/* DMA_CH0_RX_CONTROL */
#define EQOS_DMA_CH_RX_RXPBL_SHIFT      16
#define EQOS_DMA_CH_RX_RXPBL_MASK       0x3fu
#define EQOS_DMA_CH_RX_RBSZ_SHIFT       1           /* buffer size [14:1] */
#define EQOS_DMA_CH_RX_RBSZ_MASK        0x3fffu
#define EQOS_DMA_CH_RX_SR               (1u << 0)   /* start receive */

/* ------------------------------------------------------------------ */
/* Descriptor                                                          */
/* ------------------------------------------------------------------ */

/* --- TX, read format (driver ghi) --- */
#define EQOS_TDES2_B1L_MASK             0x3fffu     /* buffer 1 length */
#define EQOS_TDES2_TTSE                 (1u << 30)  /* TX timestamp enable (first desc) */
#define EQOS_TDES3_WB_TTSS              (1u << 17)  /* write-back: TX timestamp in TDES0/1 */
#define EQOS_TDES2_IOC                  (1u << 31)  /* interrupt on completion */
#define EQOS_TDES3_OWN                  (1u << 31)
#define EQOS_TDES3_CTXT                 (1u << 30)  /* 0 = normal descriptor */
#define EQOS_TDES3_FD                   (1u << 29)  /* first descriptor */
#define EQOS_TDES3_LD                   (1u << 28)  /* last descriptor */
#define EQOS_TDES3_CPC                  (2u << 26)  /* disable CRC Insertion */
#define EQOS_TDES3_FL_MASK              0x7fffu     /* frame length */

/* --- TX, write-back format (HW ghi lai) --- */
#define EQOS_TDES3_WB_ES                (1u << 15)  /* error summary */

/* --- RX, read format (driver ghi) --- */
#define EQOS_RDES3_OWN                  (1u << 31)
#define EQOS_RDES3_IOC                  (1u << 30)
#define EQOS_RDES3_BUF2V                (1u << 25)
#define EQOS_RDES3_BUF1V                (1u << 24)

/* --- RX, write-back format (HW ghi lai) --- */
#define EQOS_RDES3_WB_CTXT              (1u << 30)  /* context descriptor */
#define EQOS_RDES3_WB_FD                (1u << 29)
#define EQOS_RDES3_WB_LD                (1u << 28)
#define EQOS_RDES3_WB_CE                (1u << 24)  /* CRC error */
#define EQOS_RDES3_WB_GP                (1u << 23)  /* giant packet */
#define EQOS_RDES3_WB_RWT               (1u << 22)  /* receive watchdog timeout */
#define EQOS_RDES3_WB_OE                (1u << 21)  /* overflow error */
#define EQOS_RDES3_WB_LT_MASK           (7u << 16)  /* length/type field */
#define EQOS_RDES3_WB_LT_TYPE           (1u << 16)  /* 001: type packet */
#define EQOS_RDES3_WB_LT_OAM            (7u << 16)  /* 111: OAM / slow protocol */
#define EQOS_RDES1_WB_OPC_SHIFT         16          /* RDES1[31:16] OAM subtype/code */
#define EQOS_RDES1_WB_TSA               (1u << 14)  /* RX timestamp in next context desc */
#define EQOS_RDES2_WB_HF                (1u << 18)  /* hash filter pass */
#define EQOS_RDES2_WB_MADRM_SHIFT       19          /* RDES2[26:19] MAC addr match / hash value */
#define EQOS_RDES2_WB_MADRM_MASK        (0xffu << 19)
#define EQOS_RDES3_WB_RS2V              (1u << 27)  /* RDES2 status valid */
#define EQOS_RDES3_WB_ES                (1u << 15)  /* error summary */
#define EQOS_RDES3_WB_PL_MASK           0x7fffu     /* packet length */

#endif /* EQOS_HW_H */
