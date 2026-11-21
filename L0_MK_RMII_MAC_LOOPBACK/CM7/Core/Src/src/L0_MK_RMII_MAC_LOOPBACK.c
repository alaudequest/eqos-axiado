#include <stdio.h>
#include <stdint.h>
#include "../includes/mac_defines.h"
#include "../includes/eth_registers.h"
#include "../includes/eth_utils.h"
#include "../includes/eth_printf.h"
#include "../includes/eth_functions_utils.h"
#include "../includes/eth_function_stm32.h"
/**
 * Declare the global variable for tests
 * 0: MAC Loopback - traffic test (ETH_TEST_DFLT_FRAMES frames, compare TX/RX)
 * 1: MAC Loopback - MAC address filter test (MAC_Packet_Filter cases)
 * 2: MAC Loopback - DMA burst test (DMA_SysBus_Mode.FB / AAL: bytes written after the frame in rx_buf)
 * 3: MAC Loopback - PTP timestamp test (MAC_Timestamp_Control system time, TDES2.TTSE, RX context descriptor, one-step OSTC)

 * 4: MAC Loopback - VLAN insertion test (TX context descriptor, TDES2.VTIR, MAC_VLAN_Incl, bad context -> CDE)
 * 5: MAC Loopback - QinQ test (context with outer + inner tag, MAC_VLAN_Incl.CSVL, MAC_Inner_VLAN_Incl, IVTIR insert/replace/remove)
 * 6: MAC Loopback - TSO test (MSS context descriptor or DMA_CH0_Control.MSS, TDES3.TSE: one TCP frame cut into segments)
 
  
Mode    New case  What is checked                                                                  Expected on board
4 VLAN  F         deliberately wrong context: CTXT + FD + LD (RM0399 p.3060 lists it as an error)  ctx TDES3 write-back has bit 23 CDE = 1, OWN = 0; DMA_CH0_Status CDE [13] = 1, AIS [14] = 1. Result 6/6
5 QinQ  C         MAC_Inner_VLAN_Incl.VLP = 0, context IVTIR = insert                                          68 bytes, 88 A8 07 D0 81 00 00 64 same as A but decided by the descriptor
5 QinQ  D         sent frame already has 2 tags (S 07D0 + C E0C8), IVTIR = replace                 68 bytes, inner tag changed to 81 00 00 64
5 QinQ  E         same frame, IVTIR = remove                                                       64 bytes, 88 A8 07 D0 followed by EtherType 88 B5. Result 5/5
6 TSO   D         MSS = 1000 written to DMA_CH0_Control.MSS instead of context, after B/C stored 1460   3 frames of 1054 bytes, "last write wins". Result 4/4
3 PTP   one-step  context OSTC + TCMSSV before a PTP Sync frame (0x88F7); MAC_Timestamp_Control in Master mode   bytes 48..57 of received frame (originTimestamp, sent as 0) hold system time, seconds match that frame's own TX timestamp


 
 * 7: MAC Loopback - checksum offload test (TDES3.CIC inserts on TX, MAC_Configuration.IPC checks on RX -> RDES1)
 * 8: MAC Loopback - VLAN filter test (RX: MAC_VLAN_Tag VL/ETV/VTIM/VTHM/ERSVLM/ERIVLT/DOVLTC, MAC_Packet_Filter.VTFE drops)
 * 9: MAC Loopback - VLAN strip test (RX: MAC_VLAN_Tag EVLS/EIVLS remove the tag(s), RDES0 reports them, RDES2.VF)
 * 10: MAC Loopback - SA insertion/replacement + CRC-pad test (TX: TDES3 SAIC/CPC, MAC_Configuration.SARC; RX: MTL_RxQ0_Operation_Mode FUP/FEP, RDES3 CE)
 * 11: MAC Loopback - MAC address filter test (RX: MAC_Packet_Filter PR = 0, MAC_Address1..3 AE/SA/MBC, hash table, DBF/PM/HUC/HPF/SAF/DAIF)
 * 12: MAC Loopback - interrupt STATUS test, nothing enabled (DMA_CH0_Status TI/RI with IOC, TBU/RBU, RWT, TPS/RPS, FBE; DMA_Interrupt_Status, MTL_Q0_Interrupt_Control_Status, MAC_Interrupt_Status)
 * 13: MAC Loopback - DMA start order test, same init as 0 (A: DMA_CH0_TxDesc_Tail_Pointer written before DMA_CH0_Tx_Control.ST = 1;
 *     B: DMA_CH0_Rx_Control.SR = 0, 1 frame held 10 s in the MTL RX FIFO (MTL_RxQ0_Debug.PRXQ), then SR = 1 delivers it)
 */
int eth_run_test_ops = 13;

/* Called by main() after the board init, and again on each USER button press */
int do_test()
{
    uint8_t mac_idx = 0, test_result;
    eth_printf("RMII[%d] MAC Loopback Test\n", mac_idx);
    test_result = eth_rmii_mac_lb(mac_idx);

    if (test_result == 1) {
      ETH_PRINTF_L1("======== PASS ========\n");
    } else {
      ETH_PRINTF_L1("======== FAIL ========\n");
    }
    return test_result;
}
