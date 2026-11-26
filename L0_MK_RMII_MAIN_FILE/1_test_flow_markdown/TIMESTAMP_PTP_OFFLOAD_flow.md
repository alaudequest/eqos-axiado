# MAC PTP Offload (PTO) — EQOS databook Table 8-11 message generation: test code flow

Source: EQOS databook 8.7 (PTP Timestamp Offload), Table 8-11, 18.13 (programming guidelines).
Feature: DWC_EQOS_PTO_EN (company chip: PTO_EN; STM32H755: MAC_PTO_Control = ETH_MACPOCR).
Not in the test plan PDF.

Scope: for each Table 8-11 mode, check that the MAC generates the PTP messages itself
(periodic, on trigger, or as a response to a received message). MAC internal loopback:
software-sent PTP frames go TX → RX into the MAC parser; MAC-generated frames come back on RX
and are checked by software. Polling only (MAC_Interrupt_Enable.TSIE=1, CPU interrupt off).

## Table 8-11 modes (MAC_Timestamp_Control)

| Mode | SNAPTYPSEL [17:16] | TSMSTRENA [15] | TSEVNTENA [14] | MAC generates |
|---|---|---|---|---|
| A. Ordinary/Boundary Subordinate | 00 | 0 | 1 | Delay_Req on Sync reception |
| B. Ordinary/Boundary Manager | 00 | 1 | 1 | Sync (periodic / trigger); Delay_Resp on Delay_Req reception |
| C. Transparent Subordinate | 01 | 0 | 1 | Pdelay_Req (periodic / trigger); Pdelay_Resp on Pdelay_Req; Delay_Req on Sync |
| D. Transparent Manager | 01 | 1 | 1 | Pdelay_Req (periodic / trigger); Pdelay_Resp on Pdelay_Req; Sync (periodic / trigger); Delay_Resp on Delay_Req |
| E. Peer-to-Peer Transparent | 11 | x | x | Pdelay_Req (periodic / trigger); Pdelay_Resp on Pdelay_Req |
| F. Invalid (e.g. SNAPTYPSEL=10) | 10 | x | x | Nothing |

## Registers (EQOS reference)

| Register.field | Meaning |
|---|---|
| MAC_PTO_Control (0xBC0): PTOEN [0], ASYNCEN [1], APDREQEN [2], ASYNCTRIG [4], APDREQTRIG [5], DRRDIS [6], PDRDIS [7], DN [15:8] | Offload enable; periodic Sync; periodic Pdelay_Req; one Sync / one Pdelay_Req (self-clear, needs ASYNCEN / APDREQEN); disable Delay_Req/Delay_Resp response; disable Pdelay_Resp response; domainNumber |
| MAC_Source_Port_Identity0/1/2 (0xBC4/0xBC8/0xBCC) | sourcePortIdentity [31:0] / [63:32] / [79:64] of generated messages |
| MAC_Log_Message_Interval (0xBD0): LSI [7:0], DRSYNCR [10:8], LMPDRI [31:24] | Sync interval 2^LSI s; subordinate: 1 Delay_Req per 2^DRSYNCR Sync; Pdelay_Req interval 2^LMPDRI s (signed, 2's complement) |
| MAC_Timestamp_Status.TXTSSIS [15] + MAC_Tx_Timestamp_Status_Nanoseconds/_Seconds | Egress timestamp of a generated Sync / Delay_Req / Pdelay_Req (no descriptor) |
| RDES1 (LD write-back): PMT [11:8], PFT [12], PV [13] | Received PTP message type (1 Sync, 3 Delay_Req, 4 Delay_Resp, 5 Pdelay_Req, 6 Pdelay_Resp), over Ethernet, v2 |

Generated message rules (databook 8.7.2): Sync and Pdelay_Resp are one-step (twoStepFlag=0);
originTimestamp = egress timestamp; Delay_Resp/Pdelay_Resp requestingPortIdentity = sourcePortIdentity
of the received request; DA = 01:1B:19:00:00:00 (Pdelay messages: 01:80:C2:00:00:0E), or the request's
SA when the request was unicast. Received message must match: domainNumber = DN, twoStepFlag = 0,
transportSpecific = 0.

## Setup
1. MAC reset, 100M full duplex, MAC internal loopback (MAC_Configuration.LM=1), MAC start.
2. System time, coarse update (same as MAC_ISR_TSIS_flow.md Setup 1–3), start 100 s.
3. MAC_Timestamp_Control: TSENA=1, TSVER2ENA=1, TSIPENA=1 (PTP over Ethernet), TSENALL=0;
   mode bits per case. MAC_Interrupt_Enable.TSIE=1.
4. MAC_Packet_Filter.PM=1 (receive both PTP multicast addresses).
5. MAC_Source_Port_Identity0/1/2 = 0x44556677 / 0x00112233 / 0x0001 (print; check byte order in the
   received frame on the first case). MAC_PTO_Control: DN=0, PTOEN=1, other bits per case.

Save MAC_Timestamp_Control, MAC_PTO_Control, MAC_Log_Message_Interval, MAC_Packet_Filter,
MAC_Interrupt_Enable; restore at the end (PTOEN=0).

## Software-sent PTP frames (eth_tx_frame_send, EtherType 0x88F7, PTPv2 header)
transportSpecific=0, versionPTP=2, domainNumber=0, twoStepFlag=0, sourcePortIdentity = 0xAABB…
(different from the MAC's), sequenceId = test counter, DA 01:1B:19:00:00:00 (Pdelay_Req:
01:80:C2:00:00:0E), body all 0 (Sync 44 B, Delay_Req 44 B, Pdelay_Req 54 B).

## Per-case check of each MAC-generated frame (RX loopback)
- RDES1.PFT=1, PV=1, PMT = expected type; messageType, domainNumber = DN.
- sourcePortIdentity = MAC_Source_Port_Identity0/1/2; DA as in the rules above.
- Response: sequenceId and requestingPortIdentity = the request's.
- Generated Sync / Delay_Req / Pdelay_Req: TXTSSIS=1; MAC_Tx_Timestamp_Status_* = originTimestamp
  in the frame (read Nanoseconds then Seconds).
- No TX descriptor used (TX ring current/tail pointers unchanged) — print it.
Before each case: drain RX, read MAC_Tx_Timestamp_Status_* and MAC_Timestamp_Status (clear old).
Wait window per case: 3 s unless stated. `RESULT: PASS/FAIL` per case.

## Cases

| # | Mode | Program / send | Expected |
|---|---|---|---|
| A1 | A | Send Sync | 1 Delay_Req generated |
| A2 | A | DRSYNCR=1; send 4 Sync | 2 Delay_Req |
| A3 | A | DRRDIS=1; send Sync | No Delay_Req |
| A4 | A | Send Sync with domainNumber=5 (DN=0) | No Delay_Req |
| A5 | A | Send Sync with twoStepFlag=1 | No Delay_Req |
| B1 | B | ASYNCEN=1, LSI=0 (1 s); wait 3.5 s | 3–4 Sync, one-step; TX timestamp interval ≈ 1 s (±10 %) |
| B2 | B | ASYNCEN=1, LSI=0x0F (2^15 s, no periodic Sync in the window); ASYNCTRIG=1 | 1 Sync; ASYNCTRIG reads 0 |
| B3 | B | Send Delay_Req | 1 Delay_Resp: receiveTimestamp = RX timestamp of the Delay_Req; logMinDelayReqInterval = DRSYNCR + LSI |
| B4 | B | DRRDIS=1; send Delay_Req | No Delay_Resp |
| C1 | C | APDREQEN=1, LMPDRI=0 (1 s); wait 3.5 s | 3–4 Pdelay_Req (each also answered: Pdelay_Resp, see C3) |
| C2 | C | APDREQEN=1, LMPDRI=0x0F; APDREQTRIG=1 | 1 Pdelay_Req; APDREQTRIG reads 0 |
| C3 | C | Send Pdelay_Req | 1 Pdelay_Resp, one-step |
| C4 | C | PDRDIS=1; send Pdelay_Req | No Pdelay_Resp |
| C5 | C | Send Sync | 1 Delay_Req |
| D1 | D | ASYNCEN=1 + APDREQEN=1, LSI=0, LMPDRI=0; wait 3.5 s | 3–4 Sync and 3–4 Pdelay_Req |
| D2 | D | Send Pdelay_Req, then Delay_Req | 1 Pdelay_Resp, 1 Delay_Resp |
| E1 | E | APDREQEN=1, LMPDRI=0; wait 3.5 s | 3–4 Pdelay_Req |
| E2 | E | Send Pdelay_Req, then Sync and Delay_Req | 1 Pdelay_Resp; no Delay_Req, no Delay_Resp |
| F1 | F | SNAPTYPSEL=10; ASYNCEN=1 + APDREQEN=1; send Sync, Delay_Req, Pdelay_Req | Nothing generated |

Total 19 cases: `N_pass/19 PASS`.

## Open points
- Loopback: the MAC also sees its own generated Pdelay_Req and may answer it (Pdelay_Resp) in modes C/D/E,
  and its own Sync is not answered (manager). Count only the expected types per case; log the rest.
- Guideline 18.13.4 step 3 says the Source Port Identity is programmed "to match with ingress Sync":
  if A1 fails with a different sourcePortIdentity, repeat with the Sync's sourcePortIdentity = MAC's.
- Byte order of MAC_Source_Port_Identity0/1/2 in the frame: confirm in A1 and fix the expected value.
- Pdelay_Resp correctionField / requestReceiptTimestamp content: databook Table 8-13 — print, check later.
- STM32H755 (RM0399): ETH_MACPOCR exists; forum reports of offload sending nothing — print MAC_PTO_Control,
  MAC_Timestamp_Control and MAC_Log_Message_Interval readback in every case.
