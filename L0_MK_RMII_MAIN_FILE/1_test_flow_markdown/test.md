# DMA_CHn_Tx_Control TSE_MODE — UDP/IPv4 fragmentation offload (UFO): test code flow

Scope: send one large UDP/IPv4 packet per case through the TSO engine with UFO; the MAC splits it into
IPv4 fragments (UDP header in the first fragment only). Check every received fragment.
Path: MAC TX → external PHY loopback → MAC RX. UDP over IPv4 only (no IPv6, no TCP fragmentation).
STM32H755: no TSE_MODE field (CMSIS) → real chip only.

## Status bits (EQOS databook 5.50a, §11.4 / §18.20)

| Register.bit | Meaning |
|---|---|
| DMA_CHn_Tx_Control.TSE [12] | TSO engine enabled on channel n. |
| DMA_CHn_Tx_Control.TSE_MODE [14:13] | 00 UDP segmented like TCP, 01 UFO with UDP checksum, 10 UFO without UDP checksum. |
| DMA_CHn_Control.MSS / context TDES2.MSS | UFO: MFS = payload after the IPv4 header per fragment, multiple of 8 B. |
| Context TDES3.CTXT [30], TCMSSV [26] | Context descriptor; MFS valid. |
| TDES2.HL [9:0], TMWD [30] (read format, TSE=1) | Header length; TMWD=0: TSO memory write enabled. |
| TDES3.TSE [18], THL [22:19], TPL [17:0] (FD) | Segmentation/fragmentation for this packet, THL=2 (UDP), TPL = UDP header + UDP payload. |
| TDES3.ES [15] (write-back) | TX error summary. |

## Setup
1. PHY BMCR: LOOPBACK=1, ANENABLE=0, SPEED=100M, DUPLEX=full.
2. MAC_Configuration: LM=0, FES=1, DM=1, CST=1, IPC=1, TE=RE=1.
3. MTL_TxQ0_Operation_Mode: TSF=1. MTL_RxQ0_Operation_Mode: DIS_TCP_EF=1.
4. DMA_CH0_Control: MSS=536 (default; the context descriptor sets the MFS per case).
   DMA_CH0_Tx_Control: TSE=1, TSE_MODE = case value.
Save the registers above; restore at the end. Readback mismatch → restore, return -1.

Descriptors per packet (2 TX descriptors):
- Context descriptor: TDES2.MSS = MFS, TDES3: CTXT=1, TCMSSV=1, OWN=1.
- Normal descriptor: TDES0 = header address, TDES1 = payload address,
  TDES2: HL = 42, TMWD=0, B2L = UDP payload length,
  TDES3: OWN=1, FD=1, LD=1, TSE=1, THL=2, TPL = 8 + UDP payload length, CPC=00.

Header (42 B): DA ff:ff:ff:ff:ff:ff | SA MAC Address0 | 0x0800 |
IPv4 (20 B, ID = ID0, DF=0, fragment offset 0) | UDP (5000 → 5001, length = 8 + payload, checksum 0x0000).
Payload: byte pattern i & 0xFF.

## Cases

| # | TSE_MODE | MFS | UDP payload | Expected fragments (IP payload bytes, in RX order) |
|---|---|---|---|---|
| 1 | 10 (no UDP checksum) | 1000 | 2500 | 1000 (UDP header + 992), 1000, 508; in order |
| 2 | 01 (UDP checksum) | 1000 | 2500 | 1000, 1000, 476, then first fragment 32 (UDP header + 24) last |
| 3 | 01, TCP packet (54 B header) | 1000 | 2500 | TCP segmented as in OFFLOAD_TCP_SEGMENT (2 × 1000 + 500) |

Per case:
1. Build header and payload.
2. Send: context descriptor + normal descriptor → wait TDES3.OWN=0.
3. Receive all fragments.
4. Print per fragment: PL, IP total length, MF, fragment offset, RDES1.

PASS per fragment:
- RDES3.ES=0, PL = 14 + 20 + fragment IP payload.
- IPv4: ID = ID0, DF=0, MF=1 (MF=0 on the last-offset fragment), offset = byte offset / 8, header checksum valid.
- Only the offset-0 fragment starts with the UDP header (bytes equal to the sent header, except the checksum).
- Fragment data = sent UDP datagram from byte offset, same length.

PASS per case:
- Number and order of fragments as in the table; no extra frame (RX timeout).
- Case 1: UDP checksum field = 0x0000. Case 2: UDP checksum = value computed in SW over the whole datagram.
- TDES3.ES=0. TX/RX timeout → stop. `RESULT: PASS/FAIL` per case, then `N/3 PASS`.

## Open points
- RX COE on IP fragments: RDES1 for fragments (IPCB / IPCE) not defined in §11.4 → log only;
  the UDP checksum in case 2 is checked in SW.
- Case 2: exact RX order of the middle fragments taken from Figure 11-7 (first fragment last); confirm on HW.
- Context TDES2.MSS width: §11.4.1 says TDES2[17:0]; check against the descriptor chapter.
- MFS not a multiple of 8, and UDP/IPv6 with TSE=1, are not covered (databook: not allowed / software fragments).