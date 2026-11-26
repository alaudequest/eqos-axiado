# MAC PPS output with IEEE 1588v2 (PTP) sync: test code flow

Not in the test plan PDF. Company chip: fixed PPS, PPS0 only (dut_ptp_pps_o).
STM32H755: ETH_PPS_OUT alternate function (PB5 or PG8, AF11 — check which is free on the Nucleo).
Goal: the board is a PTP slave (PTPv2, Layer 2, two-step, E2E delay request-response) synchronized
to a PTP master; ptp_pps_o must line up with the master's PPS.

## Test bench

| Item | Setup |
|---|---|
| PTP master | Linux PC, NIC with HW timestamping and a PPS pin (e.g. Intel I210 SDP0). `ptp4l -i <if> -2 -E -H -m --serverOnly 1` (older linuxptp: `--masterOnly 1`) |
| Master PPS | `testptp -d /dev/ptp0 -L 0,2 -p 1000000000` (pin index / function: check for your NIC) |
| Link | PC NIC ↔ board, direct cable, 100 Mbit/s full duplex |
| Scope | CH1 = master PPS (trigger), CH2 = board PPS |

## PTP messages (Layer 2: EtherType 0x88F7, DA 01:1B:19:00:00:00)

| Message (messageType) | Direction | Board uses |
|---|---|---|
| Sync (0x0) | master → board | t2 = RX timestamp (board) |
| Follow_Up (0x8) | master → board | t1 = preciseOriginTimestamp |
| Delay_Req (0x1) | board → master | t3 = TX timestamp (board) |
| Delay_Resp (0x9) | master → board | t4 = receiveTimestamp (check sequenceId and requestingPortIdentity) |
| Announce (0xB) | master → board | ignored (no BMCA, master is fixed) |

## Registers (EQOS databook)

| Register.field | Value |
|---|---|
| MAC_Timestamp_Control: TSENA [0], TSCFUPDT [1], TSCTRLSSR [9] | 1, 1 (fine update), 1 (digital rollover) |
| MAC_Timestamp_Control: TSVER2ENA [10], TSIPENA [11] | 1 (PTPv2), 1 (PTP over Ethernet) |
| MAC_Timestamp_Control: TSEVNTENA [14], TSMSTRENA [15], SNAPTYPSEL [17:16], TSENALL [8] | 1, 0, 00, 0 → RX timestamp on Sync only (databook Table 8-8) |
| TDES2.TTSE [30] | 1 for Delay_Req (TX timestamp) |
| MAC_Packet_Filter.PM [4] | 1 (receive multicast 01:1B:19:00:00:00) |
| MAC_PPS_Control.PPSCTRL0 [3:0] | 0 (1 pulse per second) |

## Setup
1. MAC reset, 100M full duplex, PHY link up, MAC start (normal mode, no loopback).
2. System time, fine update (same as MAC_Q0_TX_FLOW_CTRL_PAUSE_TIMESTAMP_flow.md Setup 0–5), start 0 s.
   Save the base addend A0.
3. MAC_Timestamp_Control PTP bits and MAC_Packet_Filter.PM as in the table.
4. MAC_PPS_Control.PPSCTRL0 = 0.

## Sync loop (one pass per Sync, 1 per second with ptp4l defaults)
1. eth_rx_frame_receive_timestamp: EtherType 0x88F7, messageType Sync → t2 (ts_valid=1), keep sequenceId.
2. Follow_Up with the same sequenceId → t1.
3. Send Delay_Req (eth_tx_frame_send_timestamp, TDES3.TTSS=1) → t3.
4. Delay_Resp with the matching sequenceId → t4.
5. offset = ((t2 − t1) − (t4 − t3)) / 2; delay = ((t2 − t1) + (t4 − t3)) / 2.
   Print sequenceId, offset, delay, addend.
6. Correction:
   - |offset| > 1 ms: coarse step, system time − offset (TSUPDT, ADDSUB / complement rules);
     reset the servo sum.
   - Otherwise fine: ppb = 0.7 × offset + 0.3 × Σoffset (offset in ns per 1 s Sync interval);
     MAC_Timestamp_Addend = A0 × (1 − ppb × 1e-9); TSADDREG=1, poll until 0.
     offset > 0 = board ahead → smaller addend → board clock slower.

## Cases

| # | Action | Expected |
|---|---|---|
| 1 | Lock: run 60 Sync | One coarse step at the start, then \|offset\| < 1 µs within 20 Sync; delay stable (± 100 ns) |
| 2 | PPS alignment: after lock, scope 60 s | Board PPS edge vs master PPS edge: \|Δ\| < 1 µs (scope mean / peak-peak); period 1.000000 s |
| 3 | Board step: +10 ms on board time (TSUPDT) | Board PPS jumps; next Sync offset > 1 ms → coarse step back; PPS realigned within 3 Sync |
| 4 | Master step: `phc_ctl /dev/ptp0 adj 0.0001` on the PC (+100 µs) | Board follows within 3 Sync; PPS realigned |
| 5 | Holdover: stop ptp4l (master PPS keeps running) | Board keeps the last addend; log PPS drift vs master over 60 s |

`RESULT: PASS/FAIL` per case (cases 2, 5: from the scope measurement).

## Open points
- A constant PPS offset is expected from PHY TX/RX latency asymmetry (LAN8742A) and the PPS pin path:
  record it after case 2; compensate later (timestamp ingress/egress correction registers, if present).
- No PC NIC with a PPS pin: run case 1 only (offset from the board log and the ptp4l log).
- Pass limits (1 µs, 100 ns) are first targets: tune after the first board run.
- PPSCTRL0 = 0 pulse is 1 PTP clock wide (4.46 ns at 224 MHz) — if too narrow to see on the pin,
  use PPSCTRL0 = 1 (1 Hz, low 537 ms / high 463 ms) and find which edge lines up with the second.
- Company chip with PTP offload (PTO_EN): the MAC can generate Delay_Req itself — see the
  PTP offload note; this flow uses the software path (no offload).
