# MAC_Timestamp_Status.TXTSSIS — TX timestamp status interrupt: test code flow

Test plan row: #245 SGMII_MAC_TX_TS_ITR_STS_ITR.
Separate from MAC_ISR_TS_TARGET_OVF_flow.md: needs TX frames and MTL drop TX status.

Scope: with MTL_Operation_Mode.DTXSTS=1 the TX timestamp is not written back to the descriptor;
the MAC stores it in MAC_Tx_Timestamp_Status_Nanoseconds/_Seconds and sets TXTSSIS [15] and TSIS.
MAC internal loopback. Polling only (MAC_Interrupt_Enable.TSIE=1, CPU interrupt off).

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| MTL_Operation_Mode.DTXSTS [1] | Drop TX status in MTL: TX timestamp goes to MAC_Tx_Timestamp_Status_*, not to TDES0/TDES1 (TDES3.TTSS stays 0). |
| TDES2.TTSE [30] | Capture the TX timestamp of this frame. |
| MAC_Timestamp_Status.TXTSSIS [15] | TX timestamp captured in MAC_Tx_Timestamp_Status_*. Cleared by reading MAC_Tx_Timestamp_Status_Seconds. |
| MAC_Interrupt_Status.TSIS [12] | Set with TXTSSIS. |
| MAC_Tx_Timestamp_Status_Nanoseconds (0xB30) | TXTSSLO [30:0] = ns; TXTSSMIS [31] = timestamp missed (clears on read). |
| MAC_Tx_Timestamp_Status_Seconds (0xB34) | TXTSSHI = seconds. |
| MAC_Timestamp_Control.TXTSSTSM [24] | Previous timestamp not read: 0 = keep old, drop new; 1 = overwrite with new. Both set TXTSSMIS. |

## Setup
1. MAC reset, 100M, MAC internal loopback (MAC_Configuration.LM=1), MAC started.
2. System time, coarse update (same as MAC_ISR_TSIS_flow.md Setup 1–3), start 100 s.
3. MTL_Operation_Mode.DTXSTS=1; MAC_Interrupt_Enable.TSIE=1.

Save MTL_Operation_Mode, MAC_Timestamp_Control, MAC_Sub_Second_Increment, MAC_Interrupt_Enable;
restore at the end.

## Cases

Before each case: read MAC_Tx_Timestamp_Status_Nanoseconds then _Seconds, then
MAC_Timestamp_Status (clear old) → check TXTSSIS=0, TSIS=0.
Frame: 64 B, TDES2.TTSE=1 (eth_tx_frame_send_timestamp: returns OK with no TTSS — expected).
Receive and discard each loopback frame. Read the system time (MAC_System_Time_Seconds/_Nanoseconds)
before, between and after the sends: T_a, T_b, T_c.

| # | TXTSSTSM | Program | Expected |
|---|---|---|---|
| 1 | 0 | T_a, send 1 frame, T_b; poll TXTSSIS (timeout 10 ms) | TXTSSIS=1, TSIS=1; TDES3.TTSS=0; TXTSSMIS=0; T_a ≤ TX timestamp ≤ T_b |
| 2 | 0 | T_a, send frame A, T_b, send frame B, T_c; no read between | TXTSSMIS=1; TX timestamp in [T_a, T_b] (frame A kept) |
| 3 | 1 | Same as case 2 | TXTSSMIS=1; TX timestamp in [T_b, T_c] (frame B overwrote A) |

Per case: print MAC_Interrupt_Status, MAC_Timestamp_Status, MAC_Tx_Timestamp_Status_Nanoseconds/
_Seconds, T_a/T_b/T_c. Read MAC_Tx_Timestamp_Status_Seconds → TXTSSIS=0 and TSIS=0.
`RESULT: PASS/FAIL` per case, then `N/3 PASS`.

## Open points
- Read order: Nanoseconds first, then Seconds (reading Seconds releases the pair and clears TXTSSIS).
- DTXSTS does not exist when the core is configured with TX status always dropped (DWC_EQOS_TX_STS_DROP):
  then it reads 1 after reset — print the readback.
