# MAC_Ext_Configuration PDC / MAC_RxQ_Ctrl4 UDC — RX packet duplication: test code flow

Scope: one received packet is delivered to several RX DMA channels (DCS bit map in MAC_Address(#i)_High).
Check the number of copies per channel, the copy content, and that duplicated packets consume one
descriptor per channel (not one per packet). Path: MAC TX (CH0) → external PHY loopback → MAC RX.
Real chip only (STM32H755: 1 RX DMA channel, no PDC).

## Status bits (EQOS databook 5.50a, §9.3.3.3 / §9.3.3.4 / §18.15)

| Register.bit | Meaning |
|---|---|
| MAC_Ext_Configuration.PDC | 1: DCS holds a channel bit map; duplication enabled. |
| MAC_RxQ_Ctrl4.UDC [31] | 1: unicast duplication; 0: unicast → lowest DCS bit only. |
| MAC_Address(#i)_High.DCS, AE (i = 1..31) | DMA channel bit map for packets matching this DA; address enable. |
| MAC_RxQ_Ctrl1.UPQ, MCBCQEN, MCBCQ | Untagged unicast queue; MC/BC routed to MCBCQ (= highest RxQ H). |
| MTL_RxQ_DMA_Map.QnDDMACH | 1: per-packet DMA channel (from DCS) for queue n. |
| RDES3.OWN, FD, LD, ES, PL (per channel) | Descriptor done, single-descriptor packet, error, length. |

H = highest RX queue in the configuration. NCH = 8 RX DMA channels.\
EQOS databook, 9.3.3.3: \
“The Packet Duplication feature is supported only on the highest MTL Queue configured. Therefore, packet duplication for Broadcast/Multicast packets requires that the MCBCQ field of MAC_RxQ_Ctrl1 register is programmed to the highest RxQ present in the configuration.”

## Setup
1. PHY BMCR: LOOPBACK=1, ANENABLE=0, SPEED=100M, DUPLEX=full.
2. MAC_Configuration: LM=0, FES=1, DM=1, CST=1, ACS=1, TE=RE=1.
3. MAC_Packet_Filter: PR=0, PM=0, HMC=0, HUC=0 (perfect DA filter only).
4. RX queues: MAC_RxQ_Ctrl0 enables Q0 and QH; MTL_RxQH_Operation_Mode: RSF=1.
5. Routing: MAC_RxQ_Ctrl1: UPQ=H, MCBCQEN=1, MCBCQ=H. MTL_RxQ_DMA_Map: Q0DDMACH=1, QHDDMACH=1.
6. MAC_Ext_Configuration: PDC=1. MAC_RxQ_Ctrl4: UDC = case value.
7. MAC_Address1 = 02:11:22:33:44:55 (unicast), MAC_Address2 = 01:00:5E:00:00:01 (multicast); AE=1, DCS = case bit map.
8. RX DMA channels 0..7: own ring (8 descriptors, 1600 B buffers), DMA_CHn_Rx_Control.SR=1.
Save the registers above; restore at the end. Readback mismatch → restore, return -1.
Frame (128 B, FCS excluded): DA (case) | SA MAC Address0 | 0x88B5 | payload: case, seq k, byte pattern.

## Cases

| # | DA | UDC | Queue | DCS bit map | Frames sent | Expected RX |
|---|---|---|---|---|---|---|
| 1 | Unicast | 1 | H | CH0, CH1 | 1 | 1 copy on CH0 and CH1 |
| 2 | Unicast | 1 | H | CH0..CH7 | 1 | 1 copy on every channel |
| 3 | Unicast | 0 | H | CH2, CH5 | 1 | CH2 only (lowest bit) |
| 4 | Multicast | 0 | H (MCBCQ) | CH1, CH3, CH6 | 1 | 1 copy on CH1, CH3, CH6 |
| 5 | Unicast | 1 | Q0 (UPQ=0) | CH1, CH4 | 1 | CH1 only (not the highest queue) |
| 6 | Unicast | 1 | H | CH0..CH3 | 4 (seq 0..3) | 4 descriptors used on each of CH0..CH3 (16 total for 4 packets) |

Per case:
1. Set UDC, UPQ, DCS (readback); re-arm all RX rings (OWN=1).
2. Send the frame(s) on TX CH0 → wait TDES3.OWN=0.
3. Poll every RX channel 0..7 until timeout; count completed descriptors per channel.
4. Print per channel: copies received, RDES3, PL, seq numbers.

PASS:
- Each expected channel: number of copies = frames sent; RDES3: ES=0, FD=LD=1, PL=128.
- Copy bytes = sent frame; seq numbers in order 0..k on every channel (case 6).
- Channels not in the expected set: no descriptor completed (OWN still 1).
- Case 6: descriptors used per channel = 4, total = 16 (a 1-packet-1-descriptor count would give 4).
`RESULT: PASS/FAIL` per case, then `N/6 PASS`. TX timeout → stop.

## Open points
- DCS encoding with PDC=1 ("state parity value"): assumed bit map, bit n = channel n → confirm in the Reference Manual.
- H and the DCS field width depend on the chip configuration; read them from the chip spec.
- MMC RX packet counters: count once per packet or once per copy? Log in case 6 (not a PASS condition).
- One full channel (RBU) during duplication: packet stays in the MTL queue for all channels → not covered.
- Duplication not done when VLAN / L3-L4 filters pick the channel, and not for preemption packets → not covered.
- Broadcast duplication not tested separately (same MCBCQ path as multicast, case 4).
