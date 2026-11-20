The Tcl SDK manuals are here, so I can check the option names against your actual documentation.Appa.html looks like the command reference. I'll extract its text and grep the options I need.Both `ixCheckTransmitDone` and `ixCheckLinkState` need an `after 1000` before them. My BadCrc script is missing that too. Checking `ixTakeOwnership`, `filter` and `stat`:Filter and stat names are confirmed. One last check on the `ip`, `udp` and `filterPallette` field names:Now the pause test. It's a different shape: the STM32 transmits and the Ixia measures its rate before, during and after pause frames.Now the BadCrc fixes. I'll make the same four changes the manual requires, so all seven scripts match.Dry-run all seven scripts against stub Ixia commands. For the pause test, the stub simulates the STM32 rate dropping to 0 while pause frames are on.All seven pass the dry run. Converting to CRLF and saving them to your folder:All seven scripts are saved in `D:\Ixia\Ixia_400`. They passed a dry run in a plain Tcl shell with fake Ixia commands, but none has run on the real Ixia yet. I checked every command and option name against your manual (`D:\Ixia\MANUALS\Tcl SDK`), so they should be right for your IxOS version.

## The test cases

Each STM32 check uses its MMC counters: `MMCRUPGR` (RX unicast good), `MMCRCRCEPR` (RX CRC errors) and `MMCRAEPR` (RX alignment errors). The unicast counter is a clean reference because ARP is broadcast, so ARP doesn't add to it.

| Script | STM32 mode | Ixia sends (after 1000 normal frames) | Expected on STM32 |
|---|---|---|---|
| `IxTest8_BadCrc` | Receive | 1000 frames with a bad CRC | CRC errors = 1000, unicast good = 1000 |
| `IxTest8b_AlignErr` | Receive | 1000 frames with extra bits **and** a bad CRC | Alignment errors (or CRC errors) = 1000, unicast good = 1000 |
| `IxTest8c_Dribble` | Receive | 1000 frames with extra bits but a good CRC | **Accepted**: unicast good = 2000. IEEE 802.3 says to cut off the extra bits and keep the frame. |
| `IxTest8d_Runt` | Receive | 1000 × 60 B frames, good CRC | Dropped: unicast good = 1000 |
| `IxTest8e_Oversize` | Receive | 1000 × 1600 B frames, good CRC | Not counted as good: unicast good = 1000 |
| `IxTest8f_Vlan` | Receive | 1000 frames tagged with VLAN ID 100 | Accepted when there's no VLAN filter: unicast good = 2000 |
| `IxTest8g_Pause` | **Transmit (Test 2 mode)** | Pause frames (pause time 65535 quanta = 335 ms) every 50 ms, for 5 s | The Ixia measures STM32 fps before, during and after. **Pass:** during < 1 % of before, and after > 90 % of before. |

The pause test is the only one where the Ixia measures the result itself. For it to work, the STM32 firmware needs RX flow control turned on (`RFE` bit in `ETH->MACRXFCR`) and a full-duplex link.

## What the manual corrected

- **`rateMode usePercentRate`** must be the first command after `stream setDefault`.
- **`ixCheckLinkState` and `ixCheckTransmitDone`** each need `after 1000` before them.
- **`-ipProtocol`** uses the name `udp`.
- **Auto-negotiation** defaults to *off* on 10/100 ports, so the scripts now turn it on.

**This may affect Test 2 and Test 4:** the manual says a `patternMask1` bit set **high** means "compare this bit". If those scripts use mask `00 00 00 00`, `userDefinedStat1` may count every frame, not just IXTS frames. Check this on the first real run. The new scripts don't use the filter.

## Files changed

All in `D:\Ixia\Ixia_400`, with CRLF line endings:

- **`IxTest8_BadCrc.tcl`** (updated):
  - `rateMode` moved to first after `stream setDefault`.
  - Added `after 1000` before the link check and before the transmit-done check.
  - `-ipProtocol` changed from `17` to `udp`.
  - Added `-autonegotiate true`.
  - The report now names the MMC registers to read.
- **New:**
  - `IxTest8b_AlignErr.tcl`
  - `IxTest8c_Dribble.tcl`
  - `IxTest8d_Runt.tcl`
  - `IxTest8e_Oversize.tcl`
  - `IxTest8f_Vlan.tcl`
  - `IxTest8g_Pause.tcl`

I didn't make a git commit in `Ixia_400`.