# MAC_Ext_Configuration USP — unicast Slow Protocol detection: test code flow

Scope: with Slow Protocol detection on (SPEN=1), send a unicast Slow Protocol frame
(DA = MAC Address0, EtherType 0x8809) and check the RX descriptor reports it as OAM
only when USP=1. PHY loopback (LAN8742), MAC loopback off.

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| MAC_Ext_Configuration.SPEN [17] | Slow Protocol detection enable (EtherType 0x8809). |
| MAC_Ext_Configuration.USP [18] | Also detect Slow Protocol frames with a unicast DA (MAC Address0). USP=0: only DA 01:80:C2:00:00:02. |
| RDES3.LT [18:16] (last descriptor) | 001 = type packet, 111 = OAM / Slow Protocol detected. |
| RDES1.OPC [31:16] (last descriptor) | OAM subtype / code (log only). |

## Setup
1. LAN8742 PHY loopback (done in main.c for this test).
2. MAC_Configuration: LM=0, FES=1, DM=1, CST=1, ACS=1, TE=RE=1.
3. MAC_Packet_Filter: PR=0 (normal DA filtering).

Save MAC_Ext_Configuration, MAC_Configuration, MAC_Packet_Filter; restore at the end.
Readback mismatch → restore, return -1.
Frame (60 B, FCS excluded): DA = SA = MAC0 | 0x8809 | subtype 0x03 (OAM) | zeros.

## Cases

| # | MAC_Ext_Configuration | Expected |
|---|---|---|
| 1 | SPEN=1, USP=0 | LT=001 (not detected) |
| 2 | SPEN=1, USP=1 | LT=111 (detected) |

Per case: SPEN/USP (readback) → send → receive → print RDES1, RDES3, LT, OPC, PL.
PASS: LT as expected, TDES3.ES=0, RDES3.ES=0, PL=60, all bytes equal.
TX/RX timeout → stop. `RESULT: PASS/FAIL` per case, then `N/2 PASS`.
