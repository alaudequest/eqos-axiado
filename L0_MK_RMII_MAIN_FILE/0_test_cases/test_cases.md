# MTL and DMA test guide

The new MTL and DMA test files are in `../1_test_cases/`.
The older MAC, VLAN and PHY tests remain in `0_test_cases/`.

## How to run a test

1. Set `ACTIVE_TEST` in `CM7/Core/Inc/eth_config_test_case.h` to a name from the tables below.
2. Build CM7 and load it onto the board.
3. Reset the board and open the UART terminal at 115200 baud.
4. Press USER once. The selected test runs once and prints its results.
5. Reset before running again. Rebuild and load the firmware when changing tests.

The default selection is still the original CRC-error test.
Do not add the new `.c` files as separate build sources. `main.c` includes
`1_test_cases/eth_queue_test_select.h`, which includes the selected test and
`eth_queue_test_common.h` for shared frame and descriptor functions.

The tests use simple Ethernet frames with EtherType `0x88B5`. Source and
destination addresses are the board's MAC address. Each frame has a number
and a repeating data pattern, so the test can detect wrong or repeated data.
Frame lengths exclude the four FCS bytes unless stated otherwise.
All tests except flow control use internal MAC loopback: TX data returns to RX
inside the MAC. DMA starts after the buffers, rings and settings are ready.

## Terms used in the tests

| Term | Meaning |
|---|---|
| TX / RX | Transmit / receive. |
| FIFO | A hardware buffer that holds data in the order it arrives. |
| Descriptor / ring | A descriptor tells DMA where a buffer is and how to use it. A ring is a list of descriptors that repeats from the first entry after the last entry. |
| OWN | `1`: DMA can use the descriptor and buffer. `0`: the CPU can check or prepare them. |
| FCS / CRC | FCS is the four-byte error check at the end of an Ethernet frame. CRC is the calculation used to make or check it. |
| Store-and-forward | Wait for the whole frame before sending it to the next stage. |
| Underflow / overflow | Underflow: TX runs out of data while sending. Overflow: RX runs out of space for incoming data. |
| Starvation | DMA has no ready descriptor to use. |
| Scatter-gather | Store parts of one frame in separate buffers and send them as one frame. |
| Bus contention | Ethernet DMA must share memory access with another device, such as MDMA. This can slow its transfers. |
| PAUSE / hysteresis | PAUSE asks the sender to wait. Different levels for starting and stopping PAUSE prevent repeated on/off switching near one level; this difference is called hysteresis. |

## MTL tests

Each file below is in `1_test_cases/`.

| ACTIVE_TEST | File | What the test checks |
|---|---|---|
| `TEST_RMII_MTL_TX_STORE_FORWARD` (7) | [L1_RMII_MTL_TX_STORE_FORWARD.c](../1_test_cases/L1_RMII_MTL_TX_STORE_FORWARD.c) | Check that TSF is enabled. Send 60, 128, 512 and 1514-byte frames using two descriptors each, then compare all received bytes. **Manual:** delay DMA reads partway through a frame and measure when TX starts. TX must wait until the complete frame is in the FIFO. The basic loopback run does not check this timing. |
| `TEST_RMII_MTL_TX_THRESHOLD` (8) | [L1_RMII_MTL_TX_THRESHOLD.c](../1_test_cases/L1_RMII_MTL_TX_THRESHOLD.c) | Try all eight TX threshold settings (TTC). Use the smallest DMA burst, send a 1514-byte frame, and check the underflow flag (UF), error flag (ES) and RX data. Then send a 60-byte frame to check that TX still works. **Manual:** run another device's bus transfers at the same time to slow Ethernet DMA. If no underflow occurs, underflow recovery is still untested. |
| `TEST_RMII_MTL_RX_POLICY` (9) | [L1_RMII_MTL_RX_POLICY.c](../1_test_cases/L1_RMII_MTL_RX_POLICY.c) | Keep RX store-and-forward (RSF) enabled. Try all four combinations of forward-error (FEP) and forward-short-frame (FUP) settings. Send good frames, 64-byte frames with a bad FCS, and 44-byte short frames with a good FCS. These last two lengths include FCS. Check delivery or drop, CRC count, RX length, data and error flags. Send a good frame after each special frame to check that RX still works. |
| `TEST_RMII_MTL_RX_OVERFLOW` (10) | [L1_RMII_MTL_RX_OVERFLOW.c](../1_test_cases/L1_RMII_MTL_RX_OVERFLOW.c) | Stop returning RX descriptors to DMA, then send 64 more 512-byte frames. Check that the FIFO overflow count increases and that DMA does not change buffers held by the CPU. Return the descriptors, check frames left in the FIFO, and receive a new frame without resetting. Automatic flow control (EHFC) is disabled so it cannot prevent the overflow. |
| `TEST_RMII_MTL_RX_FLOW_CONTROL` (11) | [L1_RMII_MTL_RX_FLOW_CONTROL.c](../1_test_cases/L1_RMII_MTL_RX_FLOW_CONTROL.c) | Skip if the RX FIFO is too small for these settings. Otherwise, use an external sender at 100 Mbit/s, full duplex, sending to the board MAC. RFA=0 requests PAUSE at 1 KiB of free space; RFD=1 stops requesting PAUSE at 1.5 KiB free. Keep RX descriptors for 5 seconds, then return completed descriptors for 5 seconds. **Manual:** capture PAUSE frames and compare their timing with FIFO fill levels. The code does not automatically decide whether PAUSE timing passes. |

For the RX policy test, software adds the FCS to the special frames. Hardware
padding and FCS generation are disabled for those transmissions so the MAC
sends the intended short frame or bad FCS.

## DMA tests

| ACTIVE_TEST | File in `1_test_cases/` | What the test checks |
|---|---|---|
| `TEST_RMII_DMA_RING_WRAP` (12) | [L1_RMII_DMA_RING_WRAP.c](../1_test_cases/L1_RMII_DMA_RING_WRAP.c) | Send groups of frames and delay checking RX. Compare every frame number and data pattern, check OWN before reusing TX buffers, and check for extra RX frames at the end. Default `QUEUE_TEST_WRAPS=2000` sends 16000 frames with the 8-entry RX ring. Check values placed before and after the rings detect writes outside the rings. **Manual:** use a bus trace or suitable debugger checks to detect reads outside the rings; the check values only detect writes. |
| `TEST_RMII_DMA_TX_SCATTER` (13) | [L1_RMII_DMA_TX_SCATTER.c](../1_test_cases/L1_RMII_DMA_TX_SCATTER.c) | Split a frame after byte 31. Make the second descriptor ready before giving the first to DMA. Check that both descriptors finish and RX receives one complete frame. Also split a frame across the last ring entry and entry zero, then send a normal frame. |
| `TEST_RMII_DMA_BURST` (14) | [L1_RMII_DMA_BURST.c](../1_test_cases/L1_RMII_DMA_BURST.c) | Set `QUEUE_TEST_PBL` to 1, 2, 4, 8, 16 or 32. PBLX8 is disabled, so the selected burst length is not multiplied by eight. Check the register setting and 128 received 1514-byte frames. Print the total CPU cycles and the longest time to send and receive one frame. Rebuild and reset for each PBL value. **Manual:** repeat while another DMA/MDMA transfer uses the bus, then compare speed and delay with your system requirements. The timings include software work, not only Ethernet transfer time. |
| `TEST_RMII_DMA_TX_STARVATION` (15) | [L1_RMII_DMA_TX_STARVATION.c](../1_test_cases/L1_RMII_DMA_TX_STARVATION.c) | Move the tail pointer so DMA reaches a descriptor with OWN=0. Check for a new TX-buffer-unavailable flag (TBU) and print DMA state. Prepare descriptors, set OWN, and update the tail. Check two full TX rings of traffic without resetting DMA. |
| `TEST_RMII_DMA_RX_STARVATION` (16) | [L1_RMII_DMA_RX_STARVATION.c](../1_test_cases/L1_RMII_DMA_RX_STARVATION.c) | Let DMA run out of RX descriptors. Check for a new RX-buffer-unavailable flag (RBU) and a DMA missed-frame or MTL overflow count. Check that descriptor status and buffers held by the CPU are unchanged. Return descriptors and check that RX works again without resetting DMA. |

## Reading the results

`Automatic checks: 0 failure(s)` means only that the checks which ran found no
failures. It does not mean every requirement has been tested.

- **FAIL:** a check found an error or timed out.
- **MANUAL:** use external traffic, bus-load tools or measurements to finish this check.
- **INCOMPLETE:** the needed event did not happen, such as TX underflow.
- **SKIP:** the hardware does not support the settings needed for the check.

These files still need to be run on the board to confirm hardware behavior.

## Debug registers

If the RX overflow test stops with an OWN timeout, check the printed test step
and TX/RX slot number. An RX timeout while filling the ring is a setup problem;
it is not a successful overflow check. Do not clear OWN manually to continue.

The initial RX tail points to the last slot. DMA can stop at that address even
when the slot has OWN=1. Before sending the last fill frame, `qt_starve_rx()`
moves the tail to slot 0. Earlier slots remain CPU-owned (OWN=0), so they cannot
be overwritten. This lets the last slot fill before the overflow traffic starts.

The expected steps are: fill all RX slots, send 64 additional frames, return
RX descriptors and receive queued frames, then check a new 128-byte frame.
RBU is cleared once before filling the ring and checked after the extra traffic.
It can become set as soon as the last RX slot is filled. Do not clear it between
these steps: DMA may remain suspended without setting RBU again. The test prints
CSR after filling the ring so this event is visible. A zero DMA missed-frame
count is allowed when the MTL FIFO overflow counter records the dropped frames.
If a wait still times out, the report includes the descriptor address/status,
DMA current descriptor addresses, tail addresses and MAC/DMA control registers.
Reset before rerunning after any timeout.

| Ethernet offset | Register | When it helps |
|---|---|---|
| `0x0D08` | MTL TX queue debug | Inspect TX queue state during store-and-forward, underflow or a timeout. |
| `0x0D38` | MTL RX queue debug | Inspect RX FIFO fill level during overflow, flow control or a timeout. |
| `0x100C` | DMA debug status | Inspect DMA state when it runs out of descriptors or a transfer times out. |
| `0x116C` | DMA channel missed-frame count | Check dropped-frame counts during RX starvation/overflow. |
| `0x0D34` | MTL overflow/missed-frame count | Check whether the RX FIFO overflowed. |

Debug values show the hardware state at the time of the read. They do not by
themselves prove that a frame finished correctly. The tests also check
descriptor status and received data.

These offsets start at the Ethernet peripheral base. `EQOS_MTL_BASE` already
includes `0x0D00`, and `EQOS_DMA_BASE` already includes `0x1000`; do not add these
base offsets twice.

Reading `0x116C` or `0x0D34` clears its counter. The RX stress helper reads each
once before sending test traffic to clear old counts, then once afterward to
get the test count. Do not subtract these two reads. Do not keep these counters
in an automatically refreshed debugger watch, because that would clear them
during the test. The printed values include counter-overflow flags. DMA missed
frames and MTL overflow counts describe different drop points; they are not
an exact total of all lost frames.
