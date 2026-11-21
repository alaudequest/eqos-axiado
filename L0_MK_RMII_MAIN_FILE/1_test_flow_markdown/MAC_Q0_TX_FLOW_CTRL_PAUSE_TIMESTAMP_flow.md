# MAC_Q0_Tx_Flow_Ctrl — pause frame measured with timestamps: test code flow

Scope: the MAC sends a pause frame (FCB_BPA), receives it in internal loopback and pauses
its own TX. The pause is measured from the RX timestamp of the pause frame (T0) to the
TX timestamp of a data frame queued during the pause (T1). No printf between T0 and T1.

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| MAC_Q0_Tx_Flow_Ctrl.PT [31:16] | Pause time in quanta (1 quantum = 512 bit times = 5.12 µs at 100 Mbps). |
| MAC_Q0_Tx_Flow_Ctrl.TFE [1] | TX flow control enable. |
| MAC_Q0_Tx_Flow_Ctrl.FCB_BPA [0] | Write 1: send one pause frame; self-clears when sent. |
| MAC_Rx_Flow_Ctrl.RFE [0] | Decode received pause frames, hold TX for PT quanta. |
| TDES3.TTSS [17] (write-back) | TX timestamp valid (needs TDES2.TTSE [30]=1): TDES0 = ns, TDES1 = s. |
| RDES1.TSA [14] (last descriptor) | RX timestamp in the next descriptor: context descriptor (RDES3.CTXT [30]=1), RDES0 = ns, RDES1 = s. |

## Setup
1. MAC_Sub_Second_Increment.SSINC = 10 (10 ns tick); MAC_Timestamp_Addend = 2^32 × 100 MHz / HCLK.
2. MAC_Timestamp_Control: TSENA, TSCFUPDT, TSCTRLSSR, TSENALL, TSADDREG = 1; wait 10 µs.
   Then TSINIT=1; wait 10 µs.
3. MAC_Rx_Flow_Ctrl.RFE=1; MAC_Packet_Filter.PCF=10 (forward the pause frame to software).

Save MAC_Q0_Tx_Flow_Ctrl, MAC_Rx_Flow_Ctrl, MAC_Packet_Filter, MAC_Timestamp_Control,
MAC_Sub_Second_Increment; restore at the end (addend not restored).

## Cases

Expected T1 − T0 = (PT × 512 + 512) × 10 ns (+512: the 64 B pause frame from its SFD).

| # | PT | Expected | PASS |
|---|---|---|---|
| 1 | 0xFFFF | 335 544 320 ns | Clock error (T1 − T0 vs expected) within ±2 %; kept for case 2 |
| 2 | 0x0100 | 1 315 840 ns × (1 + case 1 error) | Within ±2 µs |

Case 1 measures a clock error: the pause timer runs on the RMII clock (PHY crystal),
the timestamp on HCLK (HSI).

## Flow per case
1. MAC_Q0_Tx_Flow_Ctrl: PT, TFE=1 (readback). Write FCB_BPA=1, poll until 0 (timeout 10 ms).
2. Receive the pause frame with its RX timestamp → T0.
3. Send a 64 B data frame with TTSE=1 → T1 (TTSS=1). TX timeout → stop.
4. Print and check the pause frame: DA 01-80-C2-00-00-01, SA=MAC0, type 0x8808, opcode 0x0001, PT.
5. Receive the data frame: PL=64, all bytes equal.
6. Check T1 − T0 (table). `RESULT: PASS/FAIL` per case, then `N/2 PASS`.

## Open points
- TSADDREG / TSINIT use a fixed 10 µs wait instead of polling the self-clearing bits.
- Board result: PASS; case 1 clock error +0.34 % (HSI).
