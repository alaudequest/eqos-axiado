
#Yes. On your Nucleo setup the two run from completely independent oscillators. The table below says which one drives which block in the EQOS, and why that's acceptable.

## Which clock drives what (STM32H755 Nucleo)

| Function | EQOS clock input | STM32H755 source | Oscillator |
|---|---|---|---|
| MAC TX/RX bit timing, **pause timer** (counts quanta in bit times) | `clk_tx_i` / `clk_rx_i` (RMII: 50 MHz REF_CLK, divided for 100 Mbit/s) | RMII REF_CLK from the LAN8742A | PHY's 25 MHz crystal (about ±50 ppm) |
| **System time / timestamps** (what PTP synchronizes) | `clk_ptp_ref_i` | HCLK (via the PLL) | HSI, the internal RC (percent-level accuracy in your project) |
| CSR / DMA / bus | `clk_csr_i`, AHB | HCLK | HSI |

So the pause timer counts time on the wire, and the system time counts time on the CPU clock. They share no oscillator, which is why case 1 can measure one against the other.

## Why the pause timer uses the line clock

IEEE 802.3 defines pause time in **bit times** (1 quantum = 512 bit times), not in seconds. So the MAC naturally counts it in the TX/RX clock domain, the same clock that moves the bits. Each link partner pauses according to its own line clock. That's the right behaviour, because flow control only cares about "how many frames' worth of time", not absolute time.

## Why it's fine for PTP that the timestamps use a different, poor clock

PTP doesn't need an accurate oscillator, because correcting the oscillator is exactly what it does:

1. The PTP servo compares the board's time with the master's and adjusts `MAC_Timestamp_Addend` (fine correction).
2. A +0.34 % HSI error is just a constant frequency offset. The servo cancels it with an addend about 0.34 % lower, and the system time then runs at the master's rate.
3. The real requirement is **stability**, not accuracy. An HSI that drifts with temperature or has jitter makes the servo work harder and adds more offset noise. A crystal-based PTP clock gives better sync, and with it the error can be reduced to tens of ppm instead of percent.

| Clock property | Effect on PTP sync |
|---|---|
| Constant frequency error (HSI +0.34 %) | Removed by the servo through the addend (needs headroom: addend ≈ 2³¹ gives about ±50 %) |
| Drift (temperature, voltage) | Residual offset between Sync messages, made worse by a longer Sync interval |
| Jitter | Noise on every timestamp |
| Holdover (master lost) | The clock drifts at the raw oscillator error (+0.34 % = 3.4 ms per second on HSI) |

## How a timestamp crosses between the two clocks

Even with separate clocks, a timestamp is **captured** in the line-clock domain: the SFD is detected on `clk_tx_i`/`clk_rx_i`. The **value** it latches comes from the system time on `clk_ptp_ref_i`, and the MAC synchronizes between the two domains internally. That synchronization adds a small fixed latency plus up to about one PTP-clock period of jitter (about 4.5 ns at 224 MHz). This is part of the constant PHY/MAC latency in the PPS flow's open points.

## For the company chip

`clk_ptp_ref_i` is a separate input pin on the EQOS. Whether it shares an oscillator with the SGMII/PHY reference clock depends on how the chip was integrated. Ask the SoC team:
- What drives `clk_ptp_ref_i`, and at what frequency? This sets `SSINC` and the addend.
- Is it crystal-based or PLL-based, and with what accuracy and jitter? This sets your PTP pass limits.
- Is it the same source as the Ethernet reference clock? If it is, a test like PAUSE_TIMESTAMP case 1 would measure close to 0 ppm, and it would only check the pause timer logic, not a second clock.

On the Nucleo, if you clock HCLK from the HSE instead of the HSI, both errors come down to tens of ppm. Case 1 could then be tightened from ±2 % to about ±200 ppm, as mentioned before.# How the fine correction method work in Timestamp
## The two parts

In fine mode the system time counter is driven by two things:

```
           every HCLK cycle                    only when accumulator overflows
clk ──► [ accumulator += Addend ] ──carry──► [ System_Time_ns += SSINC ]
          (32-bit, wraps at 2^32)
```

- **Addend** decides **how often** the time advances.
- **SSINC** decides **how much** time is added each time it advances.

On each HCLK cycle the hardware adds `Addend` to a hidden 32-bit accumulator. When the sum goes past 2³², the accumulator wraps around and produces a **carry**. Only on a carry does the system time increase, by `SSINC` nanoseconds. On cycles with no carry, the time stays the same.

## Example 1: HCLK = 200 MHz, Addend = 2³¹, SSINC = 10

2³¹ is exactly half of 2³², so the accumulator overflows on every **second** cycle:

| HCLK cycle | Accumulator (after add) | Carry? | System time (ns) |
|---|---|---|---|
| 0 | 0 | | 0 |
| 1 | 0.5 × 2³² | no | 0 |
| 2 | 1.0 × 2³² → wraps to 0 | **yes** | **10** |
| 3 | 0.5 × 2³² | no | 10 |
| 4 | wraps to 0 | **yes** | **20** |

- One HCLK cycle is 5 ns, so a carry comes every 10 ns. That's a 100 MHz tick.
- Each tick adds 10 ns.
- So the time advances 10 ns per 10 ns of real time. It runs at exactly the right speed.

The general rule is:

```
tick rate      = HCLK × Addend / 2^32
time speed     = tick rate × SSINC        (must equal 1e9 ns per second)
→ Addend       = 2^32 × (1e9 / SSINC) / HCLK
```

## Example 2: when the ratio isn't a whole number (HCLK = 240 MHz)

You still want a 100 MHz tick with `SSINC = 10`:

```
Addend = 2^32 × 100/240 = 0x6AAA_AAAB   (≈ 0.4167 × 2^32)
```

Now the carries are **not evenly spaced**. To make it easy to see, here is the same idea at a small scale: the accumulator wraps at 12, and the addend is 5 (5/12 = 100/240).

| Cycle | Acc. before | +5 | Carry? | Time (ns) |
|---|---|---|---|---|
| 1 | 0 | 5 | | 0 |
| 2 | 5 | 10 | | 0 |
| 3 | 10 | 15 → 3 | **yes** | 10 |
| 4 | 3 | 8 | | 10 |
| 5 | 8 | 13 → 1 | **yes** | 20 |
| 6 | 1 | 6 | | 20 |
| 7 | 6 | 11 | | 20 |
| 8 | 11 | 16 → 4 | **yes** | 30 |
| 9 | 4 | 9 | | 30 |
| 10 | 9 | 14 → 2 | **yes** | 40 |
| 11 | 2 | 7 | | 40 |
| 12 | 7 | 12 → 0 | **yes** | 50 |

- Over 12 cycles (12 × 4.1667 ns = 50 ns of real time) there are exactly 5 carries, so the time advances 50 ns.
- The carries come after 3, 2, 3, 2, 2 cycles: unevenly, but **exactly right on average**.
- The leftover in the accumulator remembers the fraction, so no error builds up.

Compare coarse mode at 240 MHz: it adds `SSINC` = 4 every cycle, which is 48 ns per 50 ns, so time runs 4 % slow forever. That's the problem the TSIS flow accepts and the PAUSE flow avoids.

## Example 3: why it's called "fine correction"

Suppose PTP finds your crystal is 100 ppm slow, so your clock is losing time. You raise the addend by 100 ppm:

```
Addend_new = 0x8000_0000 × (1 + 0.0001) = 0x8003_4B5E   (HCLK = 200 MHz)
```

- The accumulator now overflows slightly more often: 100.01 MHz instead of 100 MHz.
- Each tick still adds 10 ns.
- So the time runs 0.01 % faster and catches up smoothly. It **never jumps**.

| | What you change | Effect |
|---|---|---|
| Coarse correction (`TSUPDT`) | The time **value** (+/− offset) | Time jumps instantly |
| Fine correction (addend) | The time **rate** | Time speeds up or slows down smoothly |

## The rule that follows

Carries can't happen more than once per HCLK cycle, so the tick rate must be **below** HCLK. That's why the addend must be below 2³². In practice you choose SSINC so the tick is about half of HCLK (Addend ≈ 2³¹), which leaves room to adjust both faster and slower.
