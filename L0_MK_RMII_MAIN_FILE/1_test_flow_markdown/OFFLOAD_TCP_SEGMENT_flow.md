# DMA_CHn_Tx_Control TSE — TCP segmentation offload: test code flow

Scope: send one large TCP packet (header + payload) per case with TSO; the MAC splits it into
MSS-sized segments, copies and updates the headers. Check every received segment.
Path: MAC TX → external PHY loopback → MAC RX; RX checksum offload (IPC=1) checks each segment.
STM32H755: TX DMA channel 0 only → cases 1–3; case 4 (channel isolation) on the real chip.

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| DMA_CHn_Tx_Control.TSE [12] | TCP segmentation enabled on channel n. |
| DMA_CHn_Control.MSS [13:0] | Default maximum segment size. |
| Context TDES2.MSS [13:0], TDES3.CTXT [30], TCMSSV [26] | Context descriptor carries the MSS; TCMSSV=1: MSS valid. |
| TDES2.B1L [13:0], B2L [29:16] (read format) | Buffer 1 = header only (length H), buffer 2 = TCP payload. |
| TDES3.TSE [18], THL [22:19], TPL [17:0] (FD) | TSO for this packet, TCP header length in 32-bit words, TCP payload length. |
| TDES3.ES [15] (write-back) | TX error summary. |
| RDES1.IPCE [7], IPV6 [5], IPV4 [4], IPHE [3], PT [2:0] | Payload checksum error, IP version, IP header error, PT=2 TCP. |

## Setup
1. PHY BMCR: LOOPBACK=1, ANENABLE=0, SPEED=100M, DUPLEX=full.
2. MAC_Configuration: LM=0, FES=1, DM=1, CST=1, IPC=1, TE=RE=1.
3. MTL_TxQ0_Operation_Mode: TSF=1. MTL_RxQ0_Operation_Mode: DIS_TCP_EF=1.
4. DMA_CH0_Control: MSS=1000 (default MSS, used when the context descriptor does not set one).
   DMA_CH0_Tx_Control: TSE=1 (TSO enabled on channel 0).
Save the registers above; restore at the end. Readback mismatch → restore, return -1.

Descriptors per packet (2 TX descriptors):
- Context descriptor: TDES2.MSS = case MSS, TDES3: CTXT=1, TCMSSV=1, OWN=1.
- Normal descriptor: TDES0 = header address, TDES1 = payload address,
  TDES2: B1L = H, B2L = payload length,
  TDES3: OWN=1, FD=1, LD=1, TSE=1, THL = TCP header length / 4, TPL = payload length, CPC=00.

Header: DA ff:ff:ff:ff:ff:ff | SA MAC Address0 | [VLAN] | IPv4/IPv6 | TCP (seq S0, flags ACK|PSH|FIN).
Payload: byte pattern i & 0xFF.

## Cases

| # | Header (H) | MSS | Payload | Expected segments |
|---|---|---|---|---|
| 1 | IPv4 20 + TCP 20 (54 B) | 536 (context; DMA_CH0_Control.MSS = 1000) | 1300 | 2 × 536 + 228 |
| 2 | VLAN + IPv4 60 (options) + TCP 60 (options) (138 B) | 1000 | 2500 | 2 × 1000 + 500 |
| 3 | VLAN + IPv6 40 + TCP 60 (options) (118 B) | 1000 | 2500 | 2 × 1000 + 500 |
| 4 | CH0..CH7 at once, header differs per channel (TCP src port 5000+n) | 1000 | 2000 | 2 per channel, header = own channel's |

Per case:
1. Build header (H B) and payload.
2. Send: context descriptor + normal descriptor → wait TDES3.OWN=0.
3. Receive N segments (N = ceil(payload / MSS)).
4. Print per segment: k, PL, IP ID, TCP seq, TCP flags, RDES1.

PASS per segment k (len_k = MSS, last segment = rest):
- RDES3.ES=0, PL = H + len_k.
- Header bytes = sent header, except these fields updated by HW:
  - IPv4 total length / IPv6 payload length,
  - IPv4 ID = ID0 + k, IPv4 header checksum,
  - TCP seq = S0 + k × MSS, TCP checksum,
  - TCP flags: PSH/FIN only on the last segment.
- Payload bytes = sent payload from offset k × MSS, length len_k.
- RDES1: IPHE=0, IPCE=0, PT=2 (TCP).

PASS per case: all N segments PASS, TDES3.ES=0, no extra frame after segment N (RX timeout).
TX/RX timeout → stop. `RESULT: PASS/FAIL` per case, then `N/M PASS`.

## Open points
- TX path needs a new helper (context desc + header-only buffer 1 + TSE); eth_tx_frame_send cannot do it.
- 256 B TSO memory per channel: max header used is 138 B; header > 256 B is not tested.
- Flags rule (PSH/FIN last only, CWR first only) and IPv6 extension-header support: confirm in the databook.
- Case 4 needs 8 TX DMA channels (real chip); channel isolation = no header mixing between channels.
- Confirm descriptor bit positions against the EQOS databook version (only TSE/MSS confirmed via CMSIS).
