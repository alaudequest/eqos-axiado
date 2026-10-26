#ifndef ETH_CONFIG_TEST_CASE_H
#define ETH_CONFIG_TEST_CASE_H

#define TEST_RMII_PROMISCUOUS_EN    1
#define TEST_RMII_TX_PAD            2
#define TEST_RMII_EX_PHY_LB         3
#define TEST_RMII_VLAN_FILTER       4
#define TEST_RMII_DROP_NON_TCP_UDP  5
#define TEST_RMII_MAC_RX_CRC_ERROR  6
#define TEST_RMII_MTL_TX_STORE_FORWARD  7
#define TEST_RMII_MTL_TX_THRESHOLD      8
#define TEST_RMII_MTL_RX_POLICY         9
#define TEST_RMII_MTL_RX_OVERFLOW      10
#define TEST_RMII_MTL_RX_FLOW_CONTROL  11
#define TEST_RMII_DMA_RING_WRAP       12
#define TEST_RMII_DMA_TX_SCATTER      13
#define TEST_RMII_DMA_BURST           14
#define TEST_RMII_DMA_TX_STARVATION   15
#define TEST_RMII_DMA_RX_STARVATION   16
#define TEST_MAC_CTRL_ARPEN           17
#define TEST_MAC_CTRL_SARC            18
#define TEST_MAC_CTRL_IPC             19
#define TEST_MAC_CTRL_GPSLCE          20
#define TEST_MAC_ECR_USP              21
#define TEST_MAC_PFR_HASH_UC          22
#define TEST_MAC_WTR_WATCHDOG         23
#define TEST_MAC_TXFC_PAUSE_TIMESTAMP 24
#define TEST_MAC_TXFC_PAUSE           25
#define TEST_MAC_VTR_DVLAN_OUTER      26
#define TEST_MAC_VTR_DVLAN_INNER      27
#define TEST_MAC_VTR_DVLAN_STRIP      28
#define TEST_MAC_ISR_MMC_TX           29

/* ARPEN runs through the normal main.c RX flow, not the MAC control runner. */
#define ETH_MAC_CTRL_TEST_SELECTED (ACTIVE_TEST >= TEST_MAC_CTRL_SARC && \
                                    ACTIVE_TEST <= TEST_MAC_ISR_MMC_TX)

/* Queue cases use the queue runner. MAC control cases use eth_functions.c. */
#define ETH_QUEUE_TEST_SELECTED (ACTIVE_TEST >= TEST_RMII_MTL_TX_STORE_FORWARD && \
                                 ACTIVE_TEST <= TEST_RMII_DMA_RX_STARVATION)
#ifndef QUEUE_TEST_WRAPS
#define QUEUE_TEST_WRAPS 2000U
#endif
/* For the burst test, try each value: 1, 2, 4, 8, 16 and 32.
 * PBLX8=0 means DMA uses this value without multiplying it by eight. */
#ifndef QUEUE_TEST_PBL
#define QUEUE_TEST_PBL 8U
#endif

/* DNTU: ICMP (1) should be dropped; UDP (17) is the control packet. */
#define DNTU_TEST_PROTOCOL         1U
#define DNTU_TEST_ENABLE           1U

/* VLAN filter always expects VID 100. Use 101 here for a mismatch test. */
#define VLAN_FILTER_TX_VID         100U

/*Select the test to execute*/
#ifndef ACTIVE_TEST
#define ACTIVE_TEST     29
#endif

#endif
