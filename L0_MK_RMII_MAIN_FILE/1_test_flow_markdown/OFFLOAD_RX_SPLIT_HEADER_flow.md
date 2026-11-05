# MAC_Ext_Cfg1 SPLM / DMA_CHn_Control SPH — RX header-payload split: test code flow

Scope: receive frames of different types with split header enabled and check that the header lands in
buffer 1 (RDES0) and the payload in buffer 2 (RDES2) at the split point the parser picks; frames the
parser cannot classify must not be split (everything in the payload buffer).
Path: MAC TX → external PHY loopback → MAC RX. STM32H755: no SPH / SPLM registers → real chip only.

## Status bits (EQOS databook 5.50a, §11.2.4 / §17.3 / §18.21)

| Register.bit | Meaning |
|---|---|
| DMA_CHn_Control.SPH | 1: split header enabled on RX channel n. |
| MAC_Ext_Cfg1.SPLM | 00 L3/L4 split, 01 L2 split (SPLOFST), 10 L3/L4 for IP + L2 for non-IP / tagged IP. |
| MAC_Ext_Cfg1.SPLOFST | L2 split offset, counted from the start of the Length/Type field. |
| MAC_Ext_Configuration.HDSMS [22:20] | Max header size for the split = buffer 1 size (≥ ARBS). |
| DMA_CHn_Rx_Control.RBSZ, ARBS | Buffer 2 size; ARBS=0 (buffer 1 size from HDSMS). |
| RDES0 / RDES2 (read format) | Header buffer address (first descriptor only) / payload buffer address. |
| RDES2.HL [9:0] (write-back, FD=1) | Header length written to buffer 1; 0 = not split. Non-IP: HL[9:2]. |
| RDES3.FD, LD, RS1V, RS2V, ES, PL | First/last descriptor, RDES1/RDES2 valid, error, total packet length. |
| RDES1.IPV4 [4], IPV6 [5], PT [2:0] | IP version and payload type found by the parser. |

## Setup
1. PHY BMCR: LOOPBACK=1, ANENABLE=0, SPEED=100M, DUPLEX=full.
2. MAC_Configuration: LM=0, FES=1, DM=1, CST=1, IPC=1, TE=RE=1. VLAN stripping off.
3. MAC_Ext_Configuration: HDSMS = 256 B. MAC_Ext_Cfg1: SPLOFST=2, SPLM = case value.
4. DMA_CH0_Rx_Control: RBSZ = 1600, ARBS=0. DMA_CH0_Control: SPH=1.
5. RX ring: per descriptor, RDES0 = header buffer (256 B), RDES2 = payload buffer (1600 B),
   both pre-filled with 0xA5 before each case.
Save the registers above; restore at the end. Readback mismatch → restore, return -1.
TX: normal frames from eth_tx_frame_send(), CIC=11 for IP frames (valid checksums). Payload: byte pattern i & 0xFF.

## Cases

| # | SPLM | Frame (total 200 B) | Expected HL | Header buffer / payload buffer |
|---|---|---|---|---|
| 1 | 00 | IPv4 + TCP with 12 B options | 66 | Eth+IP+TCP / rest |
| 2 | 00 | IPv6 + Hop-by-Hop (8 B) + UDP | 70 | Eth+IPv6+ext+UDP / rest |
| 3 | 00 | IPv4 with 4 B options + ICMP | 38 | Eth+IP (with options) / ICMP header + data |
| 4 | 00 | Non-IP, EtherType 0x88B5 | 0 | untouched (0xA5) / whole frame |
| 5 | 00 | VLAN-tagged IPv4 + UDP, tag not stripped | 0 | untouched (0xA5) / whole frame |
| 6 | 01 | IPv4 + UDP (L2 split for IP too) | 14 | Eth header / IP+UDP+payload |
| 7 | 10 | IPv4 + UDP | 42 | Eth+IP+UDP / rest |
| 8 | 10 | VLAN-tagged IPv4 + UDP, tag not stripped | 18 | Eth+VLAN / IP+UDP+payload |

Per case:
1. Set SPLM (readback), pre-fill all RX buffers with 0xA5.
2. Send the frame → wait TDES3.OWN=0.
3. Receive: wait RDES3.OWN=0 on the first descriptor (one descriptor expected, FD=LD=1).
4. Print RDES3, RDES2.HL, RDES1, PL, first bytes of both buffers.

PASS:
- RDES3.ES=0, FD=1, LD=1, RS2V=1, PL = 200 (frame length, FCS stripped).
- RDES2.HL as in the table.
- HL > 0: header buffer [0, HL) = sent frame [0, HL); payload buffer [0, PL−HL) = sent frame [HL, PL).
- HL = 0: payload buffer [0, PL) = sent frame; header buffer still 0xA5.
- No byte written past the expected length in either buffer (still 0xA5).
`RESULT: PASS/FAIL` per case, then `N/8 PASS`. RX timeout → stop.

## Open points
- HL = 0: databook says "not split" but not which buffer gets the frame; spec says payload buffer → confirm on HW.
- Non-IP HL format (HL[9:2]): confirm whether case 6/8 HL reads 14/18 or HL>>2.
- Header longer than HDSMS, and corrupt IPv4/IPv6 length (split point may be wrong): not covered, log-only candidates.
- RX path needs a new helper (two buffers per descriptor, HL read); eth_rx_frame_receive uses one buffer.
- SPH, SPLM, SPLOFST bit positions are in the Reference Manual, not the databook → confirm.
