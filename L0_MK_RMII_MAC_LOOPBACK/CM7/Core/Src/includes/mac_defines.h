#ifndef MAC_DEFINES_H
#define MAC_DEFINES_H

#include <stdint.h>

//
// MAC Register MAP Definitions
// Synopsys DWC Ethernet QoS (EQoS) of STM32H755, MAC_Version = 0x3142 - RM0399 Rev 4 chapter 61
// Offsets are relative to MAC_BASE_ADRS(mac_idx) and equal the "Address offset" in RM0399.
// Names follow the DWC EQoS reference manual (5.50a); offsets are the STM32H755 ones (core 4.20). Differences:
//   MAC_WD_JB_Timeout = RM0399 ETH_MACWTR, DMA_CH0_Rx_Control2 = ETH_DMACRXRLR (RDRL field),
//   DMA_CH0_Miss_Frame_Cnt is at 0x116C here (0x1164 in the 5.50a manual).
//

// Register offsets

        // MAC (RM0399 §61.11.4)
#define R_MAC_CONFIGURATION     0x0000  // Operating mode configuration         p.3112
#define R_MAC_EXT_CONFIGURATION 0x0004  // Extended operating mode configuration p.3117
#define R_MAC_PACKET_FILTER     0x0008  // Packet filtering control              p.3118
#define R_MAC_WD_JB_TIMEOUT     0x000C  // Watchdog timeout                      p.3120
#define R_MAC_INTERRUPT_STATUS  0x00B0  // Interrupt status                      p.3131
#define R_MAC_INTERRUPT_ENABLE  0x00B4  // Interrupt enable                      p.3133
#define R_MAC_RX_TX_STATUS      0x00B8  // Rx Tx status (RWT, collisions)        p.3134
#define R_MAC_HASH_TABLE_REG0   0x0010  // Hash Table 0                          p.3121
#define R_MAC_HASH_TABLE_REG1   0x0014  // Hash Table 1                          p.3122
#define R_MAC_VLAN_INCL         0x0060  // VLAN inclusion (TX tag insertion)     p.3126
#define R_MAC_INNER_VLAN_INCL   0x0064  // Inner VLAN inclusion (TX, QinQ)       p.3127
#define R_MAC_VLAN_TAG          0x0050  // VLAN tag (RX VLAN processing/status) p.3122
#define R_MAC_VLAN_HASH_TABLE   0x0058  // VLAN hash table (RX VLAN filter, VTHM) p.3124
#define R_MAC_1US_TIC_COUNTER   0x00DC  // 1-microsecond-tick counter            p.3140
#define R_MAC_VERSION           0x0110  // Version (reset 0x00003142)            p.3141
#define R_MAC_DEBUG             0x0114  // Debug                                 p.3141
#define R_MAC_HW_FEATURE0       0x011C  // HW feature 0                          p.3142
#define R_MAC_HW_FEATURE1       0x0120  // HW feature 1                          p.3144
#define R_MAC_HW_FEATURE2       0x0124  // HW feature 2                          p.3147
#define R_MAC_MDIO_ADDRESS      0x0200  // MDIO address                          p.3150
#define R_MAC_MDIO_DATA         0x0204  // MDIO data                             p.3152
#define R_MAC_ADDRESS0_HIGH     0x0300  // MAC Address 0 high (byte 5:4)         p.3153
#define R_MAC_ADDRESS0_LOW      0x0304  // MAC Address 0 low  (byte 3:0)         p.3154
#define R_MAC_ADDRESS1_HIGH     0x0308  // MAC Address 1 high                    p.3155
#define R_MAC_ADDRESS1_LOW      0x030C  // MAC Address 1 low                     p.3154

        // Timestamp / PTP (RM0399 §61.11.4; programming guide §61.9.9 p.3036)
#define R_MAC_TIMESTAMP_CONTROL 0x0B00  // Timestamp control                     p.3176
#define R_MAC_SUB_SECOND_INCREMENT 0x0B04  // Sub-second increment                  p.3178
#define R_MAC_SYSTEM_TIME_SECONDS 0x0B08  // System time seconds                   p.3180
#define R_MAC_SYSTEM_TIME_NANOSECONDS 0x0B0C  // System time nanoseconds               p.3180
#define R_MAC_SYSTEM_TIME_SECONDS_UPDATE 0x0B10  // System time seconds update            p.3181
#define R_MAC_SYSTEM_TIME_NANOSECONDS_UPDATE 0x0B14  // System time nanoseconds update        p.3181
#define R_MAC_TIMESTAMP_ADDEND  0x0B18  // Timestamp addend                      p.3182
#define R_MAC_TIMESTAMP_STATUS  0x0B20  // Timestamp status                      p.3183

        // MMC (RM0399 p.3156): statistics counters are in eth_registers.h (MMC_STATS_*)
#define R_MMC_CONTROL           0x0700  // MMC control                           p.3156

        // MTL (RM0399 §61.11.3)
#define R_MTL_OPERATION_MODE    0x0C00  // Operating mode                        p.3100
#define R_MTL_TXQ0_OPERATION_MODE 0x0D00  // Tx queue operating mode               p.3101
#define R_MTL_TXQ0_UNDERFLOW    0x0D04  // Tx queue underflow                    p.3102
#define R_MTL_TXQ0_DEBUG        0x0D08  // Tx queue debug                        p.3103
#define R_MTL_INTERRUPT_STATUS  0x0C20  // MTL interrupt status (Q0IS)           p.3101
#define R_MTL_Q0_INTERRUPT_CONTROL_STATUS 0x0D2C  // Queue interrupt control status        p.3104
#define R_MTL_RXQ0_OPERATION_MODE 0x0D30  // Rx queue operating mode               p.3106
#define R_MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT 0x0D34  // Rx queue missed packet/overflow count p.3108
#define R_MTL_RXQ0_DEBUG        0x0D38  // Rx queue debug                        p.3109

        // DMA (RM0399 §61.11.2)
#define R_DMA_MODE              0x1000  // DMA mode                              p.3074
#define R_DMA_SYSBUS_MODE       0x1004  // System bus mode                       p.3076
#define R_DMA_INTERRUPT_STATUS  0x1008  // Interrupt status                      p.3077
#define R_DMA_DEBUG_STATUS0     0x100C  // Debug status                          p.3077
#define R_DMA_CH0_CONTROL       0x1100  // Channel control                       p.3078
#define R_DMA_CH0_TX_CONTROL    0x1104  // Channel transmit control              p.3079
#define R_DMA_CH0_RX_CONTROL    0x1108  // Channel receive control               p.3081
#define R_DMA_CH0_TXDESC_LIST_ADDRESS 0x1114  // Channel Tx descriptor list address    p.3083
#define R_DMA_CH0_RXDESC_LIST_ADDRESS 0x111C  // Channel Rx descriptor list address    p.3084
#define R_DMA_CH0_TXDESC_TAIL_POINTER 0x1120  // Channel Tx descriptor tail pointer    p.3085
#define R_DMA_CH0_RXDESC_TAIL_POINTER 0x1128  // Channel Rx descriptor tail pointer    p.3086
#define R_DMA_CH0_TXDESC_RING_LENGTH 0x112C  // Channel Tx descriptor ring length     p.3086
#define R_DMA_CH0_RX_CONTROL2   0x1130  // Channel Rx descriptor ring length     p.3087
#define R_DMA_CH0_INTERRUPT_ENABLE 0x1134  // Channel interrupt enable              p.3087
#define R_DMA_CH0_CURRENT_APP_TXDESC 0x1144  // Channel current app. Tx descriptor    p.3092
#define R_DMA_CH0_CURRENT_APP_RXDESC 0x114C  // Channel current app. Rx descriptor    p.3092
#define R_DMA_CH0_STATUS        0x1160  // Channel status                        p.3093
#define R_DMA_CH0_MISS_FRAME_CNT 0x116C  // Channel missed frame count            p.3096

// MAC_Configuration Register Bits (p.3112)

        // bit numbers
#define MAC_CONFIGURATION_RE_bit 0       // Receiver enable
#define MAC_CONFIGURATION_TE_bit 1       // Transmitter enable
#define MAC_CONFIGURATION_PRELEN_bit 2       // Preamble length [3:2]
#define MAC_CONFIGURATION_DC_bit 4       // Deferral check
#define MAC_CONFIGURATION_BL_bit 5       // Back-off limit [6:5]
#define MAC_CONFIGURATION_DR_bit 8       // Disable retry
#define MAC_CONFIGURATION_DCRS_bit 9       // Disable carrier sense during transmission
#define MAC_CONFIGURATION_DO_bit 10      // Disable receive own
#define MAC_CONFIGURATION_ECRSFD_bit 11      // Enable carrier sense before transmission in full-duplex
#define MAC_CONFIGURATION_LM_bit 12      // Loopback mode
#define MAC_CONFIGURATION_DM_bit 13      // Duplex mode: 1 = full
#define MAC_CONFIGURATION_FES_bit 14      // Speed: 1 = 100 Mbit/s, 0 = 10 Mbit/s
#define MAC_CONFIGURATION_PS_bit 15      // Reads 1 (not described in RM0399)
#define MAC_CONFIGURATION_JE_bit 16      // Jumbo packet enable
#define MAC_CONFIGURATION_JD_bit 17      // Jabber disable
#define MAC_CONFIGURATION_WD_bit 19      // Watchdog disable
#define MAC_CONFIGURATION_ACS_bit 20      // Automatic pad or CRC stripping
#define MAC_CONFIGURATION_CST_bit 21      // CRC stripping for Type packets
#define MAC_CONFIGURATION_S2KP_bit 22      // IEEE 802.3as support for 2K packets
#define MAC_CONFIGURATION_GPSLCE_bit 23      // Giant packet size limit control enable
#define MAC_CONFIGURATION_IPG_bit 24      // Inter-packet gap [26:24]
#define MAC_CONFIGURATION_IPC_bit 27      // Checksum offload
#define MAC_CONFIGURATION_SARC_bit 28      // Source address insertion or replacement control [30:28]
#define MAC_CONFIGURATION_ARPEN_bit 31      // ARP offload enable

        // bit masks
#define MAC_CONFIGURATION_RE    (1U << 0)
#define MAC_CONFIGURATION_TE    (1U << 1)
#define MAC_CONFIGURATION_LM    (1U << 12)
#define MAC_CONFIGURATION_DM    (1U << 13)
#define MAC_CONFIGURATION_FES   (1U << 14)
#define MAC_CONFIGURATION_PS    (1U << 15)
#define MAC_CONFIGURATION_JE    (1U << 16)
#define MAC_CONFIGURATION_JD    (1U << 17)
#define MAC_CONFIGURATION_WD    (1U << 19)
#define MAC_CONFIGURATION_ACS   (1U << 20)
#define MAC_CONFIGURATION_CST   (1U << 21)
#define MAC_CONFIGURATION_S2KP  (1U << 22)
#define MAC_CONFIGURATION_GPSLCE (1U << 23)
#define MAC_CONFIGURATION_IPC   (1U << 27)      // Checksum offload: RX COE checks IP / TCP / UDP checksums (RDES1)
#define MAC_CONFIGURATION_SARC_MASK 0x7U            // SARC [30:28] (p.3113): [29:28] action, [30] MAC_Address1 instead of MAC_Address0
#define MAC_CONFIGURATION_SARC_NONE 0U              // SARC: off
#define MAC_CONFIGURATION_SARC_INSERT_A0 2U              // SARC: insert MAC_Address0 in every frame sent
#define MAC_CONFIGURATION_SARC_REPLACE_A0 3U              // SARC: replace the SA of every frame sent with MAC_Address0
#define MAC_CONFIGURATION_SARC_INSERT_A1 6U              // SARC: insert MAC_Address1
#define MAC_CONFIGURATION_SARC_REPLACE_A1 7U              // SARC: replace with MAC_Address1
#define MAC_CONFIGURATION_SARC_MACA1 4U              // SARC bit 30: MAC_Address1 instead of MAC_Address0 (the board ignores MAC_Address1_High.AE here, 2026-11-18)

// MAC_Packet_Filter Register Bits (p.3118)
#define MAC_PACKET_FILTER_PR    (1U << 0)       // Promiscuous mode
#define MAC_PACKET_FILTER_HUC   (1U << 1)       // Hash unicast
#define MAC_PACKET_FILTER_HMC   (1U << 2)       // Hash multicast
#define MAC_PACKET_FILTER_DAIF  (1U << 3)       // DA inverse filtering
#define MAC_PACKET_FILTER_PM    (1U << 4)       // Pass all multicast
#define MAC_PACKET_FILTER_DBF   (1U << 5)       // Disable broadcast packets
#define MAC_PACKET_FILTER_PCF_bit 6               // Pass control packets [7:6]
#define MAC_PACKET_FILTER_SAIF  (1U << 8)       // SA inverse filtering
#define MAC_PACKET_FILTER_SAF   (1U << 9)       // Source address filter enable
#define MAC_PACKET_FILTER_HPF   (1U << 10)      // Hash or perfect filter
#define MAC_PACKET_FILTER_VTFE  (1U << 16)      // VLAN tag filter enable
#define MAC_PACKET_FILTER_IPFE  (1U << 20)      // Layer 3 and layer 4 filter enable
#define MAC_PACKET_FILTER_DNTU  (1U << 21)      // Drop non-TCP/UDP over IP packets
#define MAC_PACKET_FILTER_RA    (1U << 31)      // Receive all

// MAC_WD_JB_Timeout Register Bits (p.3120)
#define MAC_WD_JB_TIMEOUT_WTO_bit 0               // Watchdog timeout [3:0]: 0 = 2 KB, 1 = 3 KB ... 0xE = 16383 bytes
#define MAC_WD_JB_TIMEOUT_WTO_MASK 0xF
#define MAC_WD_JB_TIMEOUT_PWE   (1U << 8)       // Programmable watchdog enable

        // field values
#define MAC_WD_JB_TIMEOUT_WTO_2KB 0x0U            // WTO: cut RX frames after 2048 bytes
#define MAC_WD_JB_TIMEOUT_WTO_3KB 0x1U            // WTO: cut RX frames after 3072 bytes

// MAC_Interrupt_Status Register Bits (p.3131), read-only status; MAC_Interrupt_Enable (p.3133) has the enables at the same positions
#define MAC_INTERRUPT_STATUS_PHYIS (1U << 3)       // PHY interrupt (cleared by reading MAC_Interrupt_Status)
#define MAC_INTERRUPT_STATUS_MMCIS (1U << 8)       // MMC interrupt (MMCRXIS or MMCTXIS)
#define MAC_INTERRUPT_STATUS_TSIS (1U << 12)      // Timestamp interrupt (cleared by reading MAC_Timestamp_Status)
#define MAC_INTERRUPT_STATUS_TXSTSIS (1U << 13)      // Transmit status: a TX error bit of MAC_Rx_Tx_Status is set
#define MAC_INTERRUPT_STATUS_RXSTSIS (1U << 14)      // Receive status: MAC_Rx_Tx_Status.RWT is set

// MAC_Rx_Tx_Status Register Bits (p.3134), cleared on read
#define MAC_RX_TX_STATUS_TJT    (1U << 0)       // Jabber timeout
#define MAC_RX_TX_STATUS_RWT    (1U << 8)       // Receive watchdog timeout: a frame longer than the watchdog was cut

// MAC_VLAN_Incl Register Bits (p.3126)
#define MAC_VLAN_INCL_VLT_MASK  0xFFFF          // VLAN tag for TX [15:0], used when VLTI = 0
#define MAC_VLAN_INCL_VLC_bit   16              // VLAN tag control [17:16]: 0 none, 1 delete, 2 insert, 3 replace
#define MAC_VLAN_INCL_VLP       (1U << 18)      // VLAN priority control: 1 = VLC applies to every packet, 0 = per descriptor (VTIR)
#define MAC_VLAN_INCL_CSVL      (1U << 19)      // 1 = S-VLAN type 0x88A8, 0 = C-VLAN type 0x8100
#define MAC_VLAN_INCL_VLTI      (1U << 20)      // VLAN tag input: 1 = tag from the TX context descriptor

// MAC_Inner_VLAN_Incl Register Bits (p.3127): same layout as MAC_VLAN_Incl, for the inner tag. No per-descriptor
// control for the inner tag: VLP = 1 and VLC = insert apply to every frame until changed
#define MAC_INNER_VLAN_INCL_VLC_bit 16              // inner VLAN tag control [17:16]: 0 none, 1 delete, 2 insert, 3 replace
#define MAC_INNER_VLAN_INCL_VLP (1U << 18)      // 1 = VLC applies to every packet
#define MAC_INNER_VLAN_INCL_CSVL (1U << 19)      // 1 = inner type 0x88A8, 0 = 0x8100
#define MAC_INNER_VLAN_INCL_VLTI (1U << 20)      // 1 = inner tag from the TX context descriptor (TDES2.IVT)

// MAC_VLAN_Tag Register Bits (p.3122-3124): RX side
#define MAC_VLAN_TAG_VL_MASK    0xFFFFU         // VL [15:0]: the tag (or VID with ETV) the RX filter compares with
#define MAC_VLAN_TAG_ETV        (1U << 16)      // compare the 12-bit VID only (0: the whole 16-bit tag)
#define MAC_VLAN_TAG_VTIM       (1U << 17)      // inverse match: a frame that does NOT match counts as matched
#define MAC_VLAN_TAG_ESVL       (1U << 18)      // S-VLAN type 0x88A8 counts as a VLAN packet on RX
#define MAC_VLAN_TAG_ERSVLM     (1U << 19)      // the filter matches S-tags 0x88A8 (0: C-tags 0x8100)
#define MAC_VLAN_TAG_DOVLTC     (1U << 20)      // disable the VLAN type check of ERSVLM
#define MAC_VLAN_TAG_EVLS_bit   21              // outer tag stripping on RX [22:21]
#define MAC_VLAN_TAG_EVLS_NONE  0U              // EVLS / EIVLS: do not strip
#define MAC_VLAN_TAG_EVLS_PASS  1U              // EVLS / EIVLS: strip if the VLAN filter passes
#define MAC_VLAN_TAG_EVLS_FAIL  2U              // EVLS / EIVLS: strip if the VLAN filter fails
#define MAC_VLAN_TAG_EVLS_ALWAYS 3U              // EVLS / EIVLS: always strip
#define MAC_VLAN_TAG_VTHM       (1U << 25)      // VLAN hash table match (MAC_VLAN_Hash_Table) instead of VL
#define MAC_VLAN_TAG_EIVLS_bit  28              // inner tag stripping on RX [29:28], same coding as EVLS
#define MAC_VLAN_TAG_EVLRXS     (1U << 24)      // outer VLAN tag reported in RDES0 (OVT) + RS0V
#define MAC_VLAN_TAG_EDVLP      (1U << 26)      // process up to two VLAN tags on TX and RX
#define MAC_VLAN_TAG_ERIVLT     (1U << 27)      // RX works on the inner tag (with EDVLP)
#define MAC_VLAN_TAG_EIVLRXS    (1U << 31)      // inner VLAN tag reported in RDES0 [31:16] (IVT)

        // field values
#define MAC_VLAN_INCL_VLC_INSERT 2U              // VLC: insert

// MAC_Address(#i)_High Register Bits (p.3153, p.3155)
#define MAC_ADDRESS_HIGH_AE     (1U << 31)      // Address enable

// MAC_HW_Feature0 Register Bits (p.3142-3143): read-only, what this core has
#define MAC_HW_FEATURE0_TXCOESEL (1U << 14)      // TX checksum offload engine available
#define MAC_HW_FEATURE0_RXCOESEL (1U << 16)      // RX checksum offload engine available
// MAC_HW_Feature1 Register Bits (p.3144-3146): read-only, what this core has
#define MAC_HW_FEATURE1_TSOEN   (1U << 18)      // TCP segmentation offload available
#define MAC_HW_FEATURE1_OSTEN   (1U << 11)      // one-step timestamp (PTP over Ethernet) available, p.3145
#define MAC_HW_FEATURE1_POUOST  (1U << 23)      // one-step timestamp for PTP over UDP/IP available (H755: 0)
// MAC_Timestamp_Control Register Bits (p.3176-3178)
#define MAC_TIMESTAMP_CONTROL_TSENA (1U << 0)       // Enable timestamp: system time runs, frames can be stamped
#define MAC_TIMESTAMP_CONTROL_TSCFUPDT (1U << 1)       // Fine update: system time advances by SSINC on each addend overflow
#define MAC_TIMESTAMP_CONTROL_TSINIT (1U << 2)       // Initialize system time from MAC_System_Time_Seconds_Update/STNUR (self-clearing)
#define MAC_TIMESTAMP_CONTROL_TSUPDT (1U << 3)       // Add/subtract MAC_System_Time_Seconds_Update/STNUR to the system time (self-clearing)
#define MAC_TIMESTAMP_CONTROL_TSADDREG (1U << 5)       // Load MAC_Timestamp_Addend into the addend accumulator (self-clearing)
#define MAC_TIMESTAMP_CONTROL_TSENALL (1U << 8)       // Timestamp every received packet (RX context descriptor)
#define MAC_TIMESTAMP_CONTROL_TSVER2ENA (1U << 10)      // PTP version 2 header format (p.3177)
#define MAC_TIMESTAMP_CONTROL_TSIPENA (1U << 11)      // process PTP over Ethernet (EtherType 0x88F7)
#define MAC_TIMESTAMP_CONTROL_TSEVNTENA (1U << 14)      // snapshot / one-step only for event messages (Sync, Delay_Req, ...)
#define MAC_TIMESTAMP_CONTROL_TSMSTRENA (1U << 15)      // messages relevant to a master (Sync) instead of a slave (Delay_Req)
#define MAC_TIMESTAMP_CONTROL_SNAPTYPSEL_bit 16              // PTP mode [17:16] with TSMSTRENA / TSEVNTENA: Table 564 p.2983
#define MAC_TIMESTAMP_CONTROL_TSCTRLSSR (1U << 9)       // Digital rollover: nanoseconds field counts 0..999 999 999
#define MAC_TIMESTAMP_CONTROL_TXTSSTSM (1U << 24)      // Overwrite an unread TX timestamp status

// MAC_Sub_Second_Increment Register Bits (p.3178)
#define MAC_SUB_SECOND_INCREMENT_SSINC_bit 16              // Sub-second increment [23:16]: added to the system time per tick

        // field values
#define MAC_SUB_SECOND_INCREMENT_SSINC_TICK 1U              // SSINC: + 1 per PTP clock tick, sub-second = raw tick counter
#define MAC_ADDRESS_HIGH_SA     (1U << 30)      // 1 = compare with SA, 0 = with DA (x = 1..3)
#define MAC_ADDRESS_HIGH_MBC_bit 24              // Mask byte control [29:24]: bit k masks address byte k

// MAC_Debug Register Bits (p.3141)
#define MAC_DEBUG_RPESTS        (1U << 0)       // MAC receive protocol engine active
#define MAC_DEBUG_RFCFCSTS_bit  1               // MAC receive packet controller FIFO status [2:1]
#define MAC_DEBUG_TPESTS        (1U << 16)      // MAC transmit protocol engine active
#define MAC_DEBUG_TFCSTS_bit    17              // MAC transmit packet controller status [18:17]

// MTL_TxQ0_Operation_Mode Register Bits (p.3101)
#define MTL_TXQ0_OPERATION_MODE_FTQ (1U << 0)       // Flush transmit queue
#define MTL_TXQ0_OPERATION_MODE_TSF (1U << 1)       // Transmit store and forward
#define MTL_TXQ0_OPERATION_MODE_TXQEN_bit 2               // Transmit queue enable [3:2], 2 = enabled
#define MTL_TXQ0_OPERATION_MODE_TTC_bit 4               // Transmit threshold control [6:4], 0 = 32 bytes
#define MTL_TXQ0_OPERATION_MODE_TQS_bit 16              // Transmit queue size [18:16], 7 = 2048 bytes

        // field values
#define MTL_TXQ0_OPERATION_MODE_TXQEN_EN 2U              // TXQEN: queue enabled (read-only on a single-queue core)
#define MTL_TXQ0_OPERATION_MODE_TTC_32 0U              // TTC: send when 32 bytes are in the queue (TSF = 0)
#define MTL_TXQ0_OPERATION_MODE_TQS_2KB 7U              // TQS: (7 + 1) x 256 = 2048 bytes

// MTL_TxQ0_Debug Register Bits (p.3103)
#define MTL_TXQ0_DEBUG_TRCSTS_bit 1               // MTL Tx queue read controller status [2:1]
#define MTL_TXQ0_DEBUG_TXQSTS   (1U << 4)       // MTL Tx queue not empty

// MTL_RxQ0_Operation_Mode Register Bits (p.3106)
#define MTL_RXQ0_OPERATION_MODE_RTC_bit 0               // Receive queue threshold control [1:0], 0 = 64 bytes
#define MTL_RXQ0_OPERATION_MODE_FUP (1U << 3)       // Forward undersized good packets
#define MTL_RXQ0_OPERATION_MODE_FEP (1U << 4)       // Forward error packets
#define MTL_RXQ0_OPERATION_MODE_RSF (1U << 5)       // Receive queue store and forward
#define MTL_RXQ0_OPERATION_MODE_DIS_TCP_EF (1U << 6)       // Disable dropping of TCP/IP checksum error packets
#define MTL_RXQ0_OPERATION_MODE_RQS_bit 20              // Receive queue size [22:20] (read-only, 7 = 2048 bytes)

        // field values
#define MTL_RXQ0_OPERATION_MODE_RTC_64 0U              // RTC: DMA reads when 64 bytes are in the queue (RSF = 0)
#define MTL_RXQ0_OPERATION_MODE_RQS_2KB 7U              // RQS: (7 + 1) x 256 = 2048 bytes

// MTL_RxQ0_Debug Register Bits (p.3109)
#define MTL_RXQ0_DEBUG_RRCSTS_bit 1               // MTL Rx queue read controller state [2:1]
#define MTL_RXQ0_DEBUG_RXQSTS_bit 4               // MTL Rx queue fill-level status [5:4]
#define MTL_RXQ0_DEBUG_PRXQ_bit 16              // Number of packets in Rx queue [29:16]
#define MTL_RXQ0_DEBUG_PRXQ_MASK 0x3FFF

// MTL_Interrupt_Status Register Bits (p.3101)
#define MTL_INTERRUPT_STATUS_Q0IS (1U << 0)       // Queue 0 interrupt status: see MTL_Q0_Interrupt_Control_Status

// MTL_Q0_Interrupt_Control_Status Register Bits (p.3104): status rc_w1, enables rw
#define MTL_Q0_INTERRUPT_CONTROL_STATUS_TXUNFIS (1U << 0)       // TX queue underflow status
#define MTL_Q0_INTERRUPT_CONTROL_STATUS_TXUIE (1U << 8)       // TX queue underflow interrupt enable
#define MTL_Q0_INTERRUPT_CONTROL_STATUS_RXOVFIS (1U << 16)      // RX queue overflow status (partial frame delivered: RDES3.OE too)
#define MTL_Q0_INTERRUPT_CONTROL_STATUS_RXOIE (1U << 24)      // RX queue overflow interrupt enable

// MTL_RxQ0_Missed_Packet_Overflow_Cnt Register Fields (p.3108), both counters clear on read
#define MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT_CNT_MASK 0x7FF           // OVFPKTCNT [10:0] (overflow) and MISPKTCNT [26:16] (missed / flushed)
#define MTL_RXQ0_MISSED_PACKET_OVERFLOW_CNT_MISPKTCNT_bit 16

// DMA_Mode Register Bits (p.3074)
#define DMA_MODE_SWR            (1U << 0)       // Software reset (self-clearing)

// DMA_SysBus_Mode Register Bits (p.3076)
#define DMA_SYSBUS_MODE_FB      (1U << 0)       // Fixed burst length
#define DMA_SYSBUS_MODE_AAL     (1U << 12)      // Address-aligned beats

// DMA_Debug_Status0 Register Bits (p.3077)
#define DMA_DEBUG_STATUS0_RPS0_bit 8               // DMA channel 0 receive process state [11:8]
#define DMA_DEBUG_STATUS0_TPS0_bit 12              // DMA channel 0 transmit process state [15:12]
#define DMA_DEBUG_STATUS0_PS_MASK 0x7             // process state field (p.3078)
#define DMA_DEBUG_STATUS0_PS_STOPPED 0U              // TPS0 / RPS0: stopped (reset or stop command)
#define DMA_DEBUG_STATUS0_PS_WAITING 3U              // RPS0: running, waiting for a packet
#define DMA_DEBUG_STATUS0_PS_RX_SUSPENDED 4U              // RPS0: suspended, no RX descriptor
#define DMA_DEBUG_STATUS0_PS_TX_SUSPENDED 6U              // TPS0: suspended, no TX descriptor or underflow

// DMA_Interrupt_Status Register Bits (p.3077), read-only summaries
#define DMA_INTERRUPT_STATUS_DC0IS (1U << 0)       // DMA channel 0: a bit of DMA_CH0_Status is set
#define DMA_INTERRUPT_STATUS_MTLIS (1U << 16)      // MTL: MTL_Interrupt_Status
#define DMA_INTERRUPT_STATUS_MACIS (1U << 17)      // MAC: MAC_Interrupt_Status

// DMA_CH0_Miss_Frame_Cnt Register Fields (p.3096), clear on read
#define DMA_CH0_MISS_FRAME_CNT_MFC_MASK 0x7FF           // MFC [10:0]: frames dropped by the DMA (bus error or RPF)

// DMA_CH0_Control Register Bits (p.3078)
#define DMA_CH0_CONTROL_PBLX8   (1U << 16)      // 8xPBL mode
#define DMA_CH0_CONTROL_DSL_bit 18              // Descriptor skip length [20:18]
#define DMA_CH0_CONTROL_MSS_MASK 0x3FFF          // MSS [13:0]: segment size for TSO, alternative to the context descriptor

        // field values
#define DMA_CH0_CONTROL_DSL_0   0U              // DSL: no gap, descriptors are contiguous (16 bytes)

// DMA_CH0_Tx_Control Register Bits (p.3079)
#define DMA_CH0_TX_CONTROL_ST   (1U << 0)       // Start or stop transmission command
#define DMA_CH0_TX_CONTROL_OSF  (1U << 4)       // Operate on second packet
#define DMA_CH0_TX_CONTROL_TSE  (1U << 12)      // TCP segmentation enabled for the frames with TDES3.TSE = 1 (TXPBL >= 4)
#define DMA_CH0_TX_CONTROL_TXPBL_bit 16              // Transmit programmable burst length [21:16]

        // field values
#define DMA_CH0_TX_CONTROL_TXPBL_8 8U              // TXPBL: 8 beats (x 8 with DMA_CH0_Control.PBLX8 = 64 beats)

// DMA_CH0_Rx_Control Register Bits (p.3081)
#define DMA_CH0_RX_CONTROL_SR   (1U << 0)       // Start or stop receive
#define DMA_CH0_RX_CONTROL_RBSZ_bit 1               // Receive buffer size in bytes [14:1]
#define DMA_CH0_RX_CONTROL_RXPBL_bit 16              // Receive programmable burst length [21:16]

        // field values
#define DMA_CH0_RX_CONTROL_RXPBL_8 8U              // RXPBL: 8 beats (x 8 with DMA_CH0_Control.PBLX8 = 64 beats)

// DMA_CH0_Status Register Bits (p.3093)
#define DMA_CH0_STATUS_TI       (1U << 0)       // Transmit interrupt
#define DMA_CH0_STATUS_TPS      (1U << 1)       // Transmit process stopped
#define DMA_CH0_STATUS_TBU      (1U << 2)       // Transmit buffer unavailable
#define DMA_CH0_STATUS_RI       (1U << 6)       // Receive interrupt
#define DMA_CH0_STATUS_RBU      (1U << 7)       // Receive buffer unavailable
#define DMA_CH0_STATUS_RPS      (1U << 8)       // Receive process stopped
#define DMA_CH0_STATUS_RWT      (1U << 9)       // Receive watchdog timeout (access column says rw, the others rc_w1)
#define DMA_CH0_STATUS_ETI      (1U << 10)      // Early transmit interrupt: the frame is in the MTL TX FIFO
#define DMA_CH0_STATUS_ERI      (1U << 11)      // Early receive interrupt: the DMA filled the first RX buffer
#define DMA_CH0_STATUS_FBE      (1U << 12)      // Fatal bus error
#define DMA_CH0_STATUS_CDE      (1U << 13)      // Context descriptor error: context in the middle of a frame, or all 1s (p.3095)
#define DMA_CH0_STATUS_AIS      (1U << 14)      // Abnormal interrupt summary (TPS, RBU, RPS, FBE, CDE, ...), enabled bits only (p.3094)
#define DMA_CH0_STATUS_NIS      (1U << 15)      // Normal interrupt summary (TI, TBU, RI, ERI), enabled bits only
#define DMA_CH0_STATUS_TEB_bit  16              // TX DMA error bits [18:16], valid with FBE
#define DMA_CH0_STATUS_REB_bit  19              // RX DMA error bits [21:19], valid with FBE
#define DMA_CH0_STATUS_EB_MASK  0x7             // EB: bit 0 read (1) / write (0), bit 1 descriptor (1) / data (0), bit 2 data transfer

//
// DMA descriptors (RM0399 §61.10 p.3046): 4 x 32-bit words, 16 bytes, DSL = 0
//
struct eth_desc {
	volatile uint32_t des0;
	volatile uint32_t des1;
	volatile uint32_t des2;
	volatile uint32_t des3;
};

// TX normal descriptor, read format (p.3049-3053): written by the CPU
#define TDES2_B1L_MASK          0x3FFF          // Buffer 1 length [13:0]
#define TDES2_HL_MASK           0x3FF           // TSE = 1: header length [9:0] in the B1L field, Ethernet + IP + TCP (p.3051)
#define TDES2_B2L_bit           16              // Buffer 2 length [29:16]
#define TDES2_B2L_MASK          0x3FFF          // Buffer 2 length, 14 bits
#define TDES2_VTIR_bit          14              // VLAN tag insertion/replacement [15:14] (p.3050)
#define TDES2_TTSE              (1U << 30)      // Transmit timestamp enable: write-back TDES0/1 = timestamp
#define TDES2_IOC               (1U << 31)      // Interrupt on completion
#define TDES3_FL_MASK           0x7FFF          // Packet length [14:0]
#define TDES3_CIC_bit           16              // Checksum insertion control [17:16] (p.3052), TSE = 0 only
#define TDES3_TPL_MASK          0x3FFFF         // TSE = 1: TCP payload length [17:0], replaces FL and CIC (p.3053)
#define TDES3_TSE               (1U << 18)      // TCP segmentation enable (first descriptor, with THL and TPL)
#define TDES3_THL_bit           19              // TSE = 1: TCP header length [22:19] in 32-bit words, >= 5 (p.3052)
#define TDES3_LD                (1U << 28)      // Last descriptor
#define TDES3_FD                (1U << 29)      // First descriptor
#define TDES3_CTXT              (1U << 30)      // Context type (0 = normal descriptor)
#define TDES3_OWN               (1U << 31)      // Own bit: 1 = DMA, 0 = CPU

        // field values
#define TDES2_VTIR_NONE         0U              // VTIR: send the frame as it is
#define TDES2_VTIR_REMOVE       1U              // VTIR: remove the VLAN tag
#define TDES2_VTIR_INSERT       2U              // VTIR: insert the VLAN tag (context descriptor or MAC_VLAN_Incl.VLT)
#define TDES2_VTIR_REPLACE      3U              // VTIR: replace the VLAN tag
#define TDES3_CIC_NONE          0U              // CIC: no checksum inserted, the frame goes out as it is
#define TDES3_CIC_IP            1U              // CIC: IPv4 header checksum only
#define TDES3_CIC_IP_PAYLOAD    2U              // CIC: IP + TCP/UDP checksum, pseudo-header sum given by the CPU in the field
#define TDES3_CIC_FULL          3U              // CIC: IP + TCP/UDP checksum, pseudo header computed by the MAC
#define TDES3_SAIC_bit          23              // SA insertion control [25:23] (p.3052): [24:23] action, [25] MAC_Address1 instead of MAC_Address0
#define TDES3_SAIC_NONE         0U              // SAIC: the SA bytes go out as they are
#define TDES3_SAIC_INSERT       1U              // SAIC: insert the MAC address after the DA (frame given without SA)
#define TDES3_SAIC_REPLACE      2U              // SAIC: overwrite bytes 6..11 with the MAC address
#define TDES3_SAIC_MACA1        4U              // SAIC bit 25: use MAC_Address1 (only if MAC_Address1_High.AE = 1, else MAC_Address0)
#define TDES3_CPC_bit           26              // CRC pad control [27:26] (p.3051)
#define TDES3_CPC_CRC_PAD       0U              // CPC: pad to 60 bytes and append the FCS (default)
#define TDES3_CPC_CRC_NOPAD     1U              // CPC: append the FCS, no pad (a short frame goes out as a runt)
#define TDES3_CPC_NONE          2U              // CPC: neither pad nor FCS (the buffer must hold them)
#define TDES3_CPC_CRC_REPLACE   3U              // CPC: recompute the FCS over the last 4 bytes of the buffer

// TX context descriptor (p.3057-3061): CPU writes it before the frames it applies to
#define TDES3_CTXT_VT_MASK      0xFFFF          // VT [15:0]: VLAN tag to insert (priority [15:13], CFI [12], VID [11:0])
#define TDES2_CTXT_IVT_bit      16              // context TDES2 [31:16]: inner VLAN tag (with TDES3.IVLTV)
#define TDES2_CTXT_MSS_MASK     0x3FFF          // context TDES2 [13:0]: maximum segment size (with TDES3.TCMSSV, OSTC = 0)
#define TDES3_CTXT_VLTV         (1U << 16)      // VT valid
#define TDES3_CTXT_IVLTV        (1U << 17)      // inner VLAN tag (TDES2 [31:16]) valid
#define TDES3_CTXT_CDE          (1U << 23)      // write-back: context descriptor error (wrong place in the ring)
#define TDES3_CTXT_TCMSSV       (1U << 26)      // MSS (TDES2 [13:0]) or one-step timestamp correction valid
#define TDES3_CTXT_OSTC         (1U << 27)      // one-step timestamp correction
#define TDES3_CTXT_IVTIR_bit    18              // inner VLAN tag insert/replace [19:18] (p.3060), used when MAC_Inner_VLAN_Incl.VLP = 0
#define TDES3_CTXT_IVTIR_NONE   0U              // IVTIR: inner tag untouched
#define TDES3_CTXT_IVTIR_REMOVE 1U              // IVTIR: remove the inner tag (frame must carry one)
#define TDES3_CTXT_IVTIR_INSERT 2U              // IVTIR: insert the inner tag (TDES2.IVT or MAC_Inner_VLAN_Incl.VLT)
#define TDES3_CTXT_IVTIR_REPLACE 3U             // IVTIR: replace the inner tag (frame must carry one)

// TX normal descriptor, write-back format (p.3055-3057): written by the DMA
#define TDES3_IHE               (1U << 0)       // IP header error: TX COE could not do the IP checksum (CIC != 0)
#define TDES3_PCE               (1U << 12)      // Payload checksum error: TX COE could not do the TCP/UDP checksum
#define TDES3_ES                (1U << 15)      // Error summary
#define TDES3_DE                (1U << 23)      // Descriptor error (same bit as CDE of a context descriptor, p.3056)
#define TDES3_TTSS              (1U << 17)      // TX timestamp status: TDES0 = TTSL (ns), TDES1 = TTSH (s)

// RX normal descriptor, read format (p.3063-3064): written by the CPU
#define RDES3_BUF1V             (1U << 24)      // Buffer 1 address valid
#define RDES3_BUF2V             (1U << 25)      // Buffer 2 address valid
#define RDES3_IOC               (1U << 30)      // Interrupt enabled on completion
#define RDES3_OWN               (1U << 31)      // Own bit: 1 = DMA, 0 = CPU

// RX normal descriptor, write-back format (p.3065-3071): written by the DMA
#define RDES3_PL_MASK           0x7FFF          // Packet length [14:0] (FCS removed when CST/ACS strip it)
#define RDES3_ES                (1U << 15)      // Error summary
#define RDES3_LT_bit            16              // Length/type field [18:16]
#define RDES3_LT_MASK           0x7
#define RDES0_OVT_MASK          0xFFFF          // RDES0 write-back [15:0]: outer VLAN tag, valid when RS0V = 1 (p.3065)
#define RDES0_IVT_bit           16              // RDES0 write-back [31:16]: inner VLAN tag (MAC_VLAN_Tag.EIVLRXS, p.3065)

        // field values
#define RDES3_LT_LENGTH         0U              // LT: length packet (EtherType < 0x600)
#define RDES3_LT_TYPE           1U              // LT: type packet
#define RDES3_LT_ARP            3U              // LT: ARP
#define RDES3_LT_VLAN           4U              // LT: type packet with one VLAN tag
#define RDES3_LT_DVLAN          5U              // LT: type packet with two VLAN tags
#define RDES3_LT_CONTROL        6U              // LT: MAC control packet
#define RDES3_DE                (1U << 19)      // Dribble bit error
#define RDES3_RE                (1U << 20)      // Receive error
#define RDES3_OE                (1U << 21)      // Overflow error
#define RDES3_RWT               (1U << 22)      // Receive watchdog timeout
#define RDES3_GP                (1U << 23)      // Giant packet
#define RDES3_CE                (1U << 24)      // CRC error
#define RDES3_RS0V              (1U << 25)      // RDES0 valid
#define RDES3_RS1V              (1U << 26)      // RDES1 valid
#define RDES3_RS2V              (1U << 27)      // RDES2 valid
#define RDES3_LD                (1U << 28)      // Last descriptor
#define RDES3_FD                (1U << 29)      // First descriptor
#define RDES3_CTXT              (1U << 30)      // Receive context descriptor

// RX normal descriptor RDES1, write-back format (p.3066): valid when RDES3.RS1V = 1
#define RDES1_PT_MASK           0x7             // Payload type [2:0] seen by the RX COE: 0 not processed
#define RDES1_PT_UDP            1               // PT: UDP
#define RDES1_PT_TCP            2               // PT: TCP
#define RDES1_PT_ICMP           3               // PT: ICMP
#define RDES1_IPHE              (1U << 3)       // IP header error: IPv4 header checksum wrong (or bad version / length)
#define RDES1_IPV4              (1U << 4)       // IPv4 header present
#define RDES1_IPV6              (1U << 5)       // IPv6 header present
#define RDES1_IPCB              (1U << 6)       // IP checksum bypassed: the RX COE did not process this frame
#define RDES1_IPCE              (1U << 7)       // IP payload error: TCP/UDP/ICMP checksum wrong (or bad length)
#define RDES1_TSA               (1U << 14)      // Timestamp available: a context descriptor follows this packet
#define RDES1_TD                (1U << 15)      // Timestamp dropped (MTL RX FIFO overflow)
// RX context descriptor (p.3072-3073): RDES0 = RTSL (ns), RDES1 = RTSH (s), RDES3 = OWN | CTXT

// RX write-back RDES2: filter status (p.3067-3068), valid when RDES3.RS2V = 1
#define RDES2_VF                (1U << 15)      // VLAN filter status: the tag passed the VLAN filter (p.3068, with RS2V)
#define RDES2_SAF               (1U << 16)      // SA filter fail
#define RDES2_DAF               (1U << 17)      // DA filter fail (seen only with MAC_Packet_Filter.RA = 1)
#define RDES2_HF                (1U << 18)      // Hash filter status: passed the hash filter
#define RDES2_MADRM_bit         19              // [26:19] HF = 0: matching MAC_Address(#i) index, HF = 1: hash value
#define RDES2_MADRM_MASK        0xFF

#endif // MAC_DEFINES_H
