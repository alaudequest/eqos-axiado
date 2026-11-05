# TDES3 CIC — transmit checksum offload on TX queues 0–7: test code flow

Scope: for each TX queue Q0..Q7, send IPv4 UDP/TCP/ICMP frames built in code with the checksum
fields zeroed (or set to a marker), and pick the COE mode per frame with TDES3.CIC.
Path: MAC TX → external PHY loopback → MAC RX. RX checksum offload (IPC=1) checks the result in RDES1.
STM32H755: 1 TX queue / 1 TX DMA channel → only Q0 runs; Q1–Q7 on the real chip.

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| PHY BMCR.LOOPBACK [14] | External PHY loopback (IEEE 802.3 Clause 22). |
| MTL_TxQn_Operation_Mode.TXQEN [3:2], TSF [1] | 10: queue enabled. TSF=1 store-and-forward (COE needs the whole packet in the TX FIFO). |
| MTL_RxQ0_Operation_Mode.DIS_TCP_EF [6] | 1: frames with checksum errors are not dropped. |
| TDES3.CIC [17:16] (FD, read format) | 00 off, 01 IP header only, 10 IP header + payload (pseudo-header sum preset by SW), 11 IP header + payload + pseudo-header in HW. |
| TDES3.CPC [27:26] | 00: CRC and pad inserted by the MAC. |
| TDES3.ES [15], PCE [12], IHE [0] (write-back) | Error summary, payload checksum error, IP header error. |
| RDES3.ES [15], RS1V [26], PL [14:0] (last descriptor) | RX error summary, RDES1 valid, packet length. |
| RDES1.IPCE [7], IPV4 [4], IPHE [3], PT [2:0] | Payload checksum error, IPv4, IP header error, payload type (1 UDP, 2 TCP, 3 ICMP). |

NQ = TXCOE_NUM_TXQ (local #define: 1 on STM32H755, 8 on the real chip).

## Setup
1. PHY BMCR: LOOPBACK=1, ANENABLE=0, SPEED=100M, DUPLEX=full.
2. MAC_Configuration: LM=0, FES=1, DM=1, CST=1, IPC=1, TE=RE=1.
3. MTL_RxQ0_Operation_Mode: DIS_TCP_EF=1. RX on RX queue 0 / DMA channel 0.
4. Per queue n < NQ (TX queue n is fed by TX DMA channel n):
   - DMA_CHn: TX descriptor ring, DMA_CHn_Tx_Control.ST=1 (start TX DMA).
   - MTL_TxQn_Operation_Mode: TXQEN=10 (queue enabled), TSF=1 (store-and-forward).
Save PHY BMCR, MAC_Configuration, MTL_RxQ0/TxQn_Operation_Mode, DMA_CHn_Tx_Control; restore at the end.
Readback mismatch → restore, return -1.

Frame (built in code per queue n and case c):
- Ethernet: DA ff:ff:ff:ff:ff:ff | SA MAC Address0 | EtherType 0x0800.
- IPv4 (20 B): ID = n<<8 | c, 192.168.10.2 → 192.168.10.255, TTL 64, checksum field 0x0000.
- L4 header: UDP / TCP / ICMP, checksum field as in the case table.
- Payload: "Qn Cc" + byte pattern.

## Cases (per queue)

| # | Frame | CIC | L4 checksum field in buffer | Expected RDES1 |
|---|---|---|---|---|
| 1 | UDP, 17 B payload (< 60 B, MAC pads) | 11 | 0x0000 | IPV4=1, PT=1, IPHE=0, IPCE=0 |
| 2 | TCP (20 B header, PSH/ACK), 1000 B payload | 11 | 0x0000 | IPV4=1, PT=2, IPHE=0, IPCE=0 |
| 3 | ICMP echo request, 56 B data | 11 | 0x0000 | IPV4=1, PT=3, IPHE=0, IPCE=0 |
| 4 | UDP, 64 B payload | 10 | pseudo-header sum (SW) | IPV4=1, PT=1, IPHE=0, IPCE=0 |
| 5 | UDP, 64 B payload | 01 | 0xABCD | IPHE=0, IPCE=1; UDP field still 0xABCD |
| 6 | UDP, 64 B payload | 00 | 0xABCD | IPHE=1; IP field still 0x0000, UDP 0xABCD |

Per case:
1. Build the frame (checksum fields as in the table).
2. Send on Qn: TDES3.CPC=00, TDES3.CIC = case value → wait TDES3.OWN=0.
3. Receive the frame on RX queue 0.
4. Print Qn, case, TDES3, RDES3, RDES1, PL.

PASS:
- TX: TDES3.ES=0, PCE=0, IHE=0.
- RX: RS1V=1, PL = TX length (60 for case 1, padded).
- RDES1 as in the table.
- RX bytes = TX bytes, except the IP and L4 checksum fields.
- Cases 1–4: both checksum fields are non-zero (inserted by HW).

TX/RX timeout → stop that queue.
`RESULT: PASS/FAIL` per case, then `N/(6×NQ) PASS`.

## Open points
- Set TXCOE_NUM_TXQ per chip (1 on STM32H755). Q1–Q7 need a new TX helper for the DMA_CHn ring (eth_tx_frame_send uses CH0 only).
- TX and RX COE are checked against each other only; no independent software checksum.
- Case 6: RDES3.ES may be set and IPCE/IPCB with an IP header error is unconfirmed → log only.
- CIC=11 with a non-zero L4 field: check in the databook whether HW ignores it; optional case 7 (0xABCD → IPCE=0).
- Confirm the bit positions above against the EQOS databook version used (not in eqos_hw.h).
