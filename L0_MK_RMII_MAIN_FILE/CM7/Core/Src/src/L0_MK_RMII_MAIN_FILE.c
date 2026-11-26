#include <stdio.h>
#include <stdint.h>
#include "eth_config_test_case.h"
#include "eth_functions_utils.h"
#include "../includes/eth_function_stm32.h"
/**
 * Declare the global variable for tests: a TEST_* ID of eth_config_test_case.h
 * All cases run in MAC loopback (MAC_Configuration.LM = 1), 100M full duplex.
 *  4: TEST_RMII_VLAN_FILTER         - MAC_VLAN_Tag ETV, MAC_Packet_Filter.VTFE
 *  5: TEST_RMII_DROP_NON_TCP_UDP    - MAC_Packet_Filter IPFE / DNTU
 *  6: TEST_RMII_MAC_RX_CRC_ERROR    - TDES3.CPC, MTL_RxQ0_Operation_Mode.FEP, Rx_CRC_Error_Count
 * 18: TEST_MAC_CTRL_SARC            - MAC_Configuration.SARC
 * 20: TEST_MAC_CTRL_GPSLCE          - MAC_Ext_Configuration GPSL / MAC_Configuration.GPSLCE
 * 22: TEST_MAC_PFR_HASH_UC          - MAC_Packet_Filter.HUC, hash table
 * 23: TEST_MAC_WTR_WATCHDOG         - MAC_Watchdog_Timeout PWE / WTO
 * 24: TEST_MAC_TXFC_PAUSE_TIMESTAMP - MAC_Q0_Tx_Flow_Ctrl, pause time from timestamps (ETH_TXFC_TIMESTAMP = 1)
 * 25: TEST_MAC_TXFC_PAUSE           - MAC_Q0_Tx_Flow_Ctrl, pause time from DWT (ETH_TXFC_TIMESTAMP = 0)
 * 26: TEST_MAC_VTR_DVLAN_OUTER      - MAC_VLAN_Tag double VLAN, outer filter
 * 27: TEST_MAC_VTR_DVLAN_INNER      - MAC_VLAN_Tag double VLAN, inner filter (ERIVLT)
 * 28: TEST_MAC_VTR_DVLAN_STRIP      - MAC_VLAN_Tag EIVLS, RDES0 IVT / OVT
 * 29: TEST_MAC_ISR_MMC_TX           - MAC_Interrupt_Status.MMCTXIS
 * 30: TEST_MTL_RXQ0_OVERFLOW        - MTL_RxQ0 overflow, recovery
 * 31: TEST_DMA_CH0_PKT_TXRX         - B0-style eth_egress_packet*() / eth_ingress_packet() on CH0
 * 48: TEST_MAC_VIR_QINQ             - MAC_VLAN_Incl / MAC_Inner_VLAN_Incl QinQ, TX context VT / IVT / IVTIR, cases A-E
 */
int eth_run_test_ops = TEST_MAC_TXFC_PAUSE_TIMESTAMP;

/* Called by main() after the board init, and again on each USER button press */
int do_test(void)
{
    uint8_t mac_idx = 0, test_result;
    printf("RMII[%d] MAC Loopback Test\r\n", mac_idx);
    test_result = eth_rmii_mac_lb(mac_idx);

    if (test_result == 1) {
      printf("======== PASS ========\r\n");
    } else {
      printf("======== FAIL ========\r\n");
    }
    return test_result;
}
