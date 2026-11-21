# MAC_Configuration GPSLCE — giant packet size limit: test code flow

Scope: set GPSL=3000 with GPSLCE=1, send one frame at the limit and one 1 byte over,
check the giant flag in the last RX descriptor. The giant frame is still delivered.
Internal MAC loopback.

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| MAC_Configuration.GPSLCE [23] | Use MAC_Ext_Configuration.GPSL as the giant packet limit instead of 1518 B. |
| MAC_Ext_Configuration.GPSL [13:0] | Giant packet size limit in bytes, FCS included. |
| RDES3.GP [23] (last descriptor) | Giant packet: the received frame is longer than the limit. |
| RDES3.ES [15] (last descriptor) | Error summary; includes GP. |

## Setup
1. MAC_Ext_Configuration.GPSL = 3000.
2. MTL_TxQ0_Operation_Mode: TSF=0, TTC=64 B. MTL_RxQ0_Operation_Mode: RSF=0, RTC=64 B
   (the frame is larger than the MTL FIFOs).
3. MAC_Packet_Filter: PR=0, RA=0.
4. MAC_Configuration: LM=1, FES=1, DM=1, CST=1, ACS=1, GPSLCE=1, TE=RE=1;
   WD=1, JD=1, JE=0, S2KP=0 (otherwise RX watchdog / TX jabber cut the frame at 2048 B).

Save the 5 registers; restore at the end. Readback mismatch → restore, return -1.

## Cases

| # | Frame (FCS excluded) | On the wire | Expected |
|---|---|---|---|
| 1 | 2996 B | 3000 B = GPSL | GP=0, ES=0 |
| 2 | 2997 B | 3001 B = GPSL + 1 | GP=1, ES=1 |

Per case: send (one TX descriptor), receive (2 RX descriptors, status from the last one),
print RDES3 (GP, ES, RWT, OE, CE, PL). PASS: GP and ES as expected, RWT=OE=CE=0,
PL = TX length, all bytes equal. TX/RX timeout → stop.
`RESULT: PASS/FAIL` per case, then `N/2 PASS`.

## Open point
- Case 2 ES=1 not yet confirmed on the board (check added 2026-10-29).
