# MAC_Watchdog_Timeout — Receive Watchdog: test code flow

Type: **step-by-step code flow**. Register names follow the Synopsys DWC Ethernet QoS (EQOS) databook; confirm offsets/bits against the target chip's databook version.
Scope: prove the **MAC_Watchdog_Timeout** register works (PWE=1, programmable limit). Other registers are set only where the test cannot run without them.

---

## 1. Feature

**MAC_Watchdog_Timeout** (0x000C)

| Field | Bits | Meaning |
|---|---|---|
| `WTO` | [3:0] | Watchdog limit when `PWE=1` (and `MAC_Configuration.WD=0`). L = (WTO + 2) × 1024 bytes (0 = 2 KB … 14 = 16 KB, 15 reserved). |
| `PWE` | [8] | Programmable Watchdog Enable. |

The watchdog counts **bytes**, not time: a received frame longer than L is cut. The test checks the cut, no time measurement.

---

## 2. Status checked

| Register | Field | Cut frame | Good frame |
|---|---|---|---|
| RDES3 write-back (last descriptor) | `RWT` [22], `PL` [14:0] | RWT=1, PL ≤ L + 4 | RWT=0 |
| **MAC_Rx_Tx_Status** (0x00B8) | `RWT` [8] | 1 | 0 |
| **DMA_CH0_Status** (0x1160) | `RWT` [9] | 1 | 0 |

- MAC_Rx_Tx_Status is clear-on-read: read it once before each frame.
- DMA_CH0_Status is write-1-to-clear: write `RWT | AIS` before each frame.

---

## 3. Setup (only what the test needs)

Save the registers changed below and restore them at the end.

| Setting | Why |
|---|---|
| `MAC_Configuration.JD=1` | Otherwise TX jabber cuts every frame at 2048 B; the test frames are 3008 B and up. |
| `MTL_TxQ0_Operation_Mode`: `TSF=0`, `TTC=64` | Frames are larger than the TX FIFO. |
| `MTL_RxQ0_Operation_Mode`: `RSF=0`, `RTC=64`, `FEP=1` | Frames are larger than the RX FIFO; FEP=1 forwards the cut (error) frame to the descriptor. |

Assumed from the normal init (not changed): internal MAC loopback (`LM=1`), `WD=0` (reset value), TX/RX and DMA started.
DMA: multi-descriptor TX send / RX receive helpers, 16 KB TX buffer (DMA-reachable RAM) and 16 KB RX buffer.
Frame: `DA = SA = MAC_Address0 | 0x88B5 | pattern`. Size = bytes given to TX, FCS excluded (wire = size + 4).

---

## 4. Test cases

Each case: write **MAC_Watchdog_Timeout** first (readback), then send two frames. For each frame:

1. Clear old flags (read MAC_Rx_Tx_Status, write 1s to DMA_CH0_Status).
2. Build and send the frame; receive it. TX/RX timeout → stop, restore, "reset before rerun".
3. Read RDES3.RWT, PL, MAC_Rx_Tx_Status.RWT, DMA_CH0_Status.RWT; print one line.
4. PASS if all three RWT flags = expected, and for the cut frame PL ≤ L + 4.

| Case | MAC_Watchdog_Timeout | L | Frame 1 (expect RWT=0) | Frame 2 (expect RWT=1) |
|---|---|---|---|---|
| 1 | 0x101 (PWE=1, WTO=1) | 3072 | 3008 B | 3136 B |
| 2 | 0x10D (PWE=1, WTO=13) | 15360 | 15296 B | 15424 B |

Frame 1 in Case 1 (3008 B > 2048) also proves the default 2 KB limit is replaced by WTO.
Result: `RESULT: PASS/FAIL` per case, then `N/2 PASS`.

---

## 5. Open point

- Exact cut edge (FCS counted? "> L" or "≥ L") and exact PL of a cut frame: read from the printed PL on the first run.
