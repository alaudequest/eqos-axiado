# MAC_Configuration SARC — source address insert / replace: test code flow

Scope: for each defined SARC encoding, send a frame without an SA (insert) or with a
dummy SA (replace) and check the received SA is MAC Address0 or MAC Address1.
Internal MAC loopback. TDES3 CPC=00, SAIC=000, so only SARC controls the SA.

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| MAC_Configuration.SARC [30:28] | 010 insert MAC Address0, 011 replace with MAC Address0, 110 insert MAC Address1, 111 replace with MAC Address1. |
| MAC_Address1_High.AE [31] | MAC Address1 enabled (needed as the SA source for 110/111). |
| TDES3.CPC [27:26] | 00: CRC and pad inserted, so the FCS covers the new SA. |
| TDES3.SAIC [25:23] | 000: descriptor does not control the SA. |

## Setup
1. MAC_Packet_Filter: PR=0, RA=0 (DA = MAC Address0 passes the perfect filter).
2. MAC_Address1_High / _Low = 02:11:22:33:44:55, AE=1, SA=0.
3. MAC_Configuration: LM=1, FES=1, DM=1, CST=1, ACS=1, TE=RE=1.

Save the 4 registers; restore at the end. Readback mismatch → restore, return -1.

## Cases

| # | SARC | TX buffer | Expected RX (64 B) |
|---|---|---|---|
| 1 | 010 | DA \| Type \| payload (58 B) | DA \| MAC0 \| Type \| payload |
| 2 | 011 | DA \| dummy SA \| Type \| payload (64 B) | DA \| MAC0 \| Type \| payload |
| 3 | 110 | DA \| Type \| payload (58 B) | DA \| MAC1 \| Type \| payload |
| 4 | 111 | DA \| dummy SA \| Type \| payload (64 B) | DA \| MAC1 \| Type \| payload |

Per case: MAC_Configuration.SARC (readback) → send → receive → print TDES3, RDES3, PL.
PASS: TDES3.ES=0, RDES3.ES=0, PL=64, all bytes equal to the expected frame.
TX/RX timeout → stop. `RESULT: PASS/FAIL` per case, then `N/4 PASS`.
