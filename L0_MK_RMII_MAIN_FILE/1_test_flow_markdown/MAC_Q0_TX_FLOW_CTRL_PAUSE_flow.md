# MAC_Q0_Tx_Flow_Ctrl — pause frame measured with the CPU cycle counter: test code flow

Scope: the MAC sends a pause frame (FCB_BPA), receives it in internal loopback and pauses
its own TX. The pause is measured with DWT->CYCCNT from the reception of the pause frame (T0)
to the end of sending a data frame queued during the pause (T1). No printf between T0 and T1.
Timestamp version: MAC_Q0_TX_FLOW_CTRL_PAUSE_TIMESTAMP_flow.md.

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| MAC_Q0_Tx_Flow_Ctrl.PT [31:16] | Pause time in quanta (1 quantum = 512 bit times = 5.12 µs at 100 Mbps). |
| MAC_Q0_Tx_Flow_Ctrl.TFE [1] | TX flow control enable. |
| MAC_Q0_Tx_Flow_Ctrl.FCB_BPA [0] | Write 1: send one pause frame; self-clears when sent. |
| MAC_Rx_Flow_Ctrl.RFE [0] | Decode received pause frames, hold TX for PT quanta. |
| MAC_Packet_Filter.PCF [7:6] | 10: forward pause frames to software (needed to take T0). |

## Setup
1. MAC_Rx_Flow_Ctrl: RFE=1.
2. MAC_Packet_Filter: PCF=10.
Path: internal MAC loopback, 100 Mbps full duplex (set by main.c).
Save MAC_Q0_Tx_Flow_Ctrl, MAC_Rx_Flow_Ctrl, MAC_Packet_Filter; restore at the end (FCB_BPA cleared).

## Cases

Expected pause = PT × 5.12 µs.

| # | PT | Expected | PASS |
|---|---|---|---|
| 1 | 0xFFFF | 335 539 200 ns | Core clock error (T1 − T0 vs expected) within ±2 %; kept for case 2 |
| 2 | 0x0100 | 1 310 720 ns × (1 + case 1 error) | Within −20 / +50 µs |

Case 1 measures a clock error: the pause timer runs on the RMII clock (PHY crystal),
DWT on the core clock (HSI). The +50 µs margin in case 2 covers RX/TX polling; a pause off
by more than ~10 quanta fails. The PT value itself is checked in the pause frame (step 5).

## Flow per case
1. MAC_Q0_Tx_Flow_Ctrl: PT, TFE=1 (readback; mismatch → FAIL, next case).
2. Write FCB_BPA=1, poll until 0 (timeout 10 ms → FAIL, next case).
3. Receive the pause frame (timeout 10 ms) → T0 = DWT->CYCCNT. Copy the first 18 B.
4. Send a 64 B data frame (EtherType 0x88B5) → T1 = DWT->CYCCNT when the send returns.
   TX timeout (1 s) → stop the test.
5. Print and check the pause frame: DA 01-80-C2-00-00-01, SA=MAC0, type 0x8808, opcode 0x0001, PT.
6. Receive the data frame: PL=64, all bytes equal.
7. Check T1 − T0 (table).
`RESULT: PASS/FAIL` per case, then `N/2 PASS`.

## Open points
- T1 is taken when eth_tx_frame_send() returns, so it includes the data frame TX time and polling.
- Setup RFE / PCF updates are not checked (ETH_REG_UPDATE return ignored).
- Code comment in case 2 says "-20/+200 us"; the window used is −20/+50 µs (TXFC_WIN_HIGH_NS).
- Case 2 last changed to PT=0x0100, +50 µs: not re-run on HW.
