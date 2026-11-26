# MAC_Interrupt_Status TSIS — timestamp interrupt: test code flow

Scope: cause each timestamp event, check TSIS [12] sets, read MAC_Timestamp_Status,
check TSIS clears. Polling only (MAC_Interrupt_Enable.TSIE=1, CPU interrupt off).
No frames needed.

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| MAC_Interrupt_Status.TSIS [12] | Timestamp interrupt status. Set when any event bit in MAC_Timestamp_Status is set (TSSOVF, TSTARGT0, AUXTSTRIG, TSTRGTERR0, TXTSSIS). Cleared by reading MAC_Timestamp_Status (RCWE=0). |
| MAC_Timestamp_Status.TSSOVF [0] | Timestamp seconds overflow: the seconds counter wrapped from 0xFFFFFFFF to 0. |
| MAC_Timestamp_Status.TSTARGT0 [1] | Target time reached: system time >= MAC_PPS0_Target_Time_Seconds/_Nanoseconds. |
| MAC_Timestamp_Status.AUXTSTRIG [2] | Auxiliary timestamp trigger snapshot (needs an external input — not tested). |
| MAC_Timestamp_Status.TSTRGTERR0 [3] | Target time error: the programmed target time was already in the past. |

## Setup (system time running, coarse update)
PTP clock = HCLK (eth_csr_clock_hz(), 224 MHz on the Nucleo). Order: EQOS databook 18.12.1.
1. MAC_Timestamp_Control: TSENA=1, TSCTRLSSR=1 (nanosecond rollover), TSCFUPDT=0 (coarse update).
2. MAC_Sub_Second_Increment (one tick per HCLK cycle): HCLK period in ns × 256, rounded →
   SSINC [23:16] = integer ns, SNSINC [15:8] = fraction × 256.
   HCLK 224 MHz: 4.4643 ns → SSINC=4, SNSINC=119 (0x77) → 4.4648 ns/tick, time +125 ppm fast.
   SSINC alone (SNSINC=0) would be 4 ns/tick: time 10.4 % slow.
   Print HCLK, MAC_Sub_Second_Increment readback and the expected rate error (ppm).
   If SNSINC reads back 0 (no sub-ns support), print the SSINC-only error and continue:
   the cases only need the time to move forward.
3. MAC_System_Time_Seconds_Update / _Nanoseconds_Update = start value, then TSINIT=1;
   poll until TSINIT reads 0 (timeout 1 ms).
4. MAC_PPS_Control.TRGTMODSEL0 = 00 (target time → interrupt only, no PPS output).
5. Before each target write: MAC_PPS0_Target_Time_Nanoseconds.TRGTBUSY0 = 0.

## Cases

| # | Event | Program | Expected |
|---|---|---|---|
| 1 | Target time reached | Target = now + 1 ms | TSIS=1 within 100 ms; TSTARGT0=1; TSIS=0 after read |
| 2 | Target time error | Target = now − 1 s (already past) | TSIS=1; TSTRGTERR0=1 (TSTARGT0 logged); TSIS=0 after read |
| 3 | Seconds overflow | Init time = 0xFFFFFFFF s, 999 000 000 ns (Setup step 3, TSINIT) | TSIS=1 within 100 ms; TSSOVF=1; seconds read ≈ 0; TSIS=0 after read |

Per case: read MAC_Timestamp_Status first (clear old) → TSIS=0, program, poll, print
MAC_Interrupt_Status, MAC_Timestamp_Status, MAC_System_Time_Seconds/_Nanoseconds.
`RESULT: PASS/FAIL` per case, then `N/3 PASS`.

## Open points
- Case 2: some versions set TSTARGT0 together with TSTRGTERR0 — log both, pass on TSTRGTERR0.
- SNSINC support on STM32H7: check the MAC_Sub_Second_Increment readback on the board.
- Same timestamp init as MAC_Q0_TX_FLOW_CTRL_PAUSE_TIMESTAMP (fine update there): later move
  both into one shared init in eth_functions.c (fine/coarse + start time), not coded yet.
