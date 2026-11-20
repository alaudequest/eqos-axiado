# IXIA_TX_PAUSE — STM32 TX under received PAUSE frames, measured by the Ixia: test code flow

Scope: the STM32 sends 1518 B Test 2 frames continuously to the Ixia. The Ixia sends single
PAUSE frames and measures, from its capture timestamps, how long the STM32 TX stops.
All pass/fail decisions are made on the Ixia; the STM32 only sends and obeys PAUSE.
Code: `0_test_cases/IXIA_TX_PAUSE.c` (test ID 40, `TEST_IXIA_TX_PAUSE`). Ixia script: IxTest8g_Pause.
Loopback version (pause measured on the STM32): MAC_Q0_TX_FLOW_CTRL_PAUSE_flow.md.

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| MAC_Rx_Flow_Ctrl.RFE [0] | Decode received PAUSE frames, hold TX for the received pause time. |
| MAC_Configuration.DM [13] | Full duplex; PAUSE is only used in full duplex. |
| PAUSE frame pause time | In quanta: 1 quantum = 512 bit times = 5.12 µs at 100 Mbps. 0 = resume TX (XON). |

## Setup (STM32)
1. Link by PHY auto-negotiation, MAC loopback off (main.c, Ixia branch). DM=0 → FAIL, stop.
2. MAC_Rx_Flow_Ctrl: RFE=1 (readback; mismatch → FAIL, stop). Saved and restored at the end.
3. CH0 rings bound, TX pipelined (TX timeout 0), 8 frame buffers (2 × TX ring).
4. Frames: 1518 B incl. CRC (1514 B built), dst 00:00:AA:BB:CC:02, IPv4 10.7.10.32 → 10.7.10.30,
   UDP 5000 → 6000, "IXTS" at offset 42, sequence number at offset 46, DE AD BE EF fill.
5. Press USER, then start IxTest8g_Pause on the Ixia.

| Define | Value | Why |
|---|---|---|
| PAUSE_FRAME_SIZE | 1518 | One frame every T ≈ 123 µs on the wire (1538 B incl. preamble + IFG). |
| PAUSE_RUN_MS | 300000 | Must cover the whole Ixia script, including capture read-out between rounds. |
| PAUSE_STALL_LIMIT_MS | 10000 | Longest pause is 335 ms; no TX progress for 10 s → FAIL. |
| PAUSE_FLUSH_US | 1000000 | Longer than one 335 ms pause. |

## Flow (STM32)
1. Send frames as fast as possible for PAUSE_RUN_MS (no printf).
2. While paused the TX ring stays with DMA: eth_egress_packet() returns ETH_PKT_ETIMEOUT after
   5 ms, the same frame is retried and counted as a stall. Retrying does not add to the gap on
   the wire: the ring already holds frames, and the MAC sends as soon as the pause ends.
3. Flush the TX ring, print frames sent, stalls, MMC TX good, flush result.
4. Restore MAC_Rx_Flow_Ctrl. `RESULT` covers the STM32 side only (no abort, flush OK).

## Flow (Ixia, per round)
1. Start capture, wait 100 ms, send one PAUSE frame with pause time Q.
2. Wait for the pause to end + 200 ms, stop capture.
3. Keep only frames with "IXTS" at offset 42 (skips ARP); read their ns timestamps.
4. gap = largest time between two frames (the pause); T = median time between frames.

XON round: PAUSE 65535, then ≈ 50 ms of filler frames to a dummy MAC (line rate, exact time),
then PAUSE 0. The gap must be ≈ 50 ms, not 335 ms.

## Cases (Ixia pass rule)

Expected P = Q × 5.12 µs. tol = 1 % of P + 20 µs. T = 123.04 µs (1518 B frames).
PASS if P − tol ≤ gap ≤ P + 2T + tol. The extra T covers the frame the STM32 is already sending
when the PAUSE arrives.

| # | Q (quanta) | P | tol | PASS window (gap) |
|---|---|---|---|---|
| 1 | 255 | 1 305.6 µs | 33.1 µs | 1 272.5 – 1 584.7 µs |
| 2 | 4096 | 20 971.5 µs | 229.7 µs | 20 741.8 – 21 447.3 µs |
| 3 | 65535 | 335 539.2 µs | 3 375.4 µs | 332 163.8 – 339 160.7 µs |
| 4 | XON (65535 then 0) | ≈ 50 ms | set by the script | ≈ 50 ms, clearly below 335 ms |

## Notes
- Q = 255 (1.3 ms) is shorter than the 5 ms TX slot wait, so it may show no stalls on the STM32;
  stalls appear for Q = 4096 and 65535. The stall count is information only.
- PAUSE frames are handled in the MAC; the filler frames are dropped by the address filter.
  RX is not read in this test, so ARP may fill the RX ring; this does not affect TX or PAUSE.
- The Ixia selects frames by "IXTS", not by MAC, so the STM32 source MAC does not matter here.

## Open points
- The header comment in IXIA_TX_PAUSE.c still describes the older Ixia flow (PAUSE every 50 ms
  for 5 s, fps before/during/after) and the old PAUSE_RUN_MS comment ("before + 5 s PAUSE + after").
- Not run on HW yet with the new Ixia script.
