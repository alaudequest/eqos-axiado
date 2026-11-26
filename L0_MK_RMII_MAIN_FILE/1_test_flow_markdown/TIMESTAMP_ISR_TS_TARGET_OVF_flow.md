# MAC_Timestamp_Status — PPS0 target time and seconds overflow (TSIS): test code flow

Test plan rows: #244 SGMII_MAC_LOCAL_TS_SECOND_OVF_ITR, #252 SGMII_MAC_TS_TARGET_TIME_ERR_PPS0_ITR,
#253 SGMII_MAC_TS_TARGET_TIME_REACH_PPS0_ITR, #254 SGMII_MAC_TS_SECOND_OVF_ITR.
Not applicable (chip: fixed PPS, PPS0 only): #246–#251 (PPS1–3).
Replaces MAC_ISR_TSIS_flow.md. #245 (TX timestamp): MAC_ISR_TS_TXTSSIS_flow.md.
PPS output pin: MAC_PPS_OUTPUT_flow.md.

Scope: cause each timestamp event, check the event bit and MAC_Interrupt_Status.TSIS [12],
read MAC_Timestamp_Status, check TSIS clears. Polling only (MAC_Interrupt_Enable.TSIE=1,
CPU interrupt off). No frames.
Fixed PPS mode (databook 8.5.1): MAC_PPS0_Target_Time_* only generate the target time interrupt,
they are not used for the PPS output.

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| MAC_Interrupt_Status.TSIS [12] | Set when an event bit in MAC_Timestamp_Status is set. Cleared by reading MAC_Timestamp_Status (RCWE=0). |
| MAC_Timestamp_Status.TSSOVF [0] | System time seconds wrapped 0xFFFFFFFF → 0. |
| MAC_Timestamp_Status.TSTARGT0 [1] | System time >= MAC_PPS0_Target_Time. |
| MAC_Timestamp_Status.TSTRGTERR0 [3] | Target time programmed in MAC_PPS0_Target_Time was already in the past. |
| MAC_Timestamp_Status.LTSSOVF [31] | Local system time (Dual Time Source, DWC_EQOS_DSTS) seconds wrapped. |
| MAC_PPS0_Target_Time_Seconds (0xB80) / _Nanoseconds (0xB84) | Target time. |

## Setup
1. System time, coarse update (same as MAC_ISR_TSIS_flow.md Setup 1–3):
   MAC_Timestamp_Control TSENA=1, TSCTRLSSR=1, TSCFUPDT=0; MAC_Sub_Second_Increment SSINC/SNSINC
   from HCLK; MAC_System_Time_Seconds_Update = 100, _Nanoseconds_Update = 0; TSINIT=1, poll until 0.
2. MAC_Interrupt_Enable.TSIE=1.

Save MAC_Timestamp_Control, MAC_Sub_Second_Increment, MAC_Interrupt_Enable; restore at the end.

## Cases

Per case: read MAC_Timestamp_Status (clear old) → check TSIS=0 → program → poll TSIS (timeout 100 ms)
→ print MAC_Interrupt_Status, MAC_Timestamp_Status, MAC_System_Time_Seconds/_Nanoseconds
→ read MAC_Timestamp_Status again → TSIS=0. `RESULT: PASS/FAIL` per case.

| # | Event | Program | Expected |
|---|---|---|---|
| 1 | Target reached | MAC_PPS0_Target_Time = now + 1 ms (carry ns ≥ 1e9 into seconds) | TSIS=1; TSTARGT0=1; TSTRGTERR0=0 |
| 2 | Target error | MAC_PPS0_Target_Time = now − 1 s | TSIS=1; TSTRGTERR0=1 (TSTARGT0 logged) |
| 3 | System time seconds overflow | MAC_System_Time_*_Update = 0xFFFFFFFF s, 999 000 000 ns; TSINIT=1, poll | TSIS=1; TSSOVF=1; MAC_System_Time_Seconds ≈ 0 |
| 4 | Local time seconds overflow | Only if DSTS exists (see below), else print `SKIP (no Dual Time Source)` | TSIS=1; LTSSOVF=1 |

Case 4 program:
1. DSTS check: write MAC_Local_Timestamp_Control (0x280).LTSCTRLSSR [9]=1, read back.
   Reads 0 → no Dual Time Source → SKIP. STM32H755: SKIP.
2. MAC_Local_Timestamp_Control: LTSENA [0]=1, LTSCTRLSSR [9]=1.
   MAC_Local_Sub_Second_Increment (0x284).LSSINC [23:16] = SSINC (no sub-ns field).
3. MAC_Local_System_Time_Seconds_Update (0x290) = 0xFFFFFFFF,
   MAC_Local_System_Time_Nanoseconds_Update (0x294) = 999 000 000; LTSINIT [2]=1, poll until 0.
4. Restore: LTSENA=0.

Order: cases 1, 2, then 3 (it moves the system time to ~0), then 4.
Total 4 cases: `N_pass/4 PASS` (STM32H755: 3 run, case 4 SKIP).

## Open points
- Case 2: some versions also set TSTARGT0 with TSTRGTERR0 — log both, pass on TSTRGTERR0.
- Test plan: strike #246–#251 (PPS1–3). #253 description (MCGREN0 / mcgr_dma_req_o) needs
  flexible PPS: not applicable. #244 and #254 have the same description; #244 is the local
  (Dual Time Source) clock. ERR rows: set when the programmed target time is already in the past.
