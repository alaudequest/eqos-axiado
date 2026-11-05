# MAC_Packet_Filter HMC — multicast hash filter: test code flow

Scope: with HMC=1, PM=0 and HPF=0, multicast frames are filtered only by the 64-bit hash
table. Send a multicast frame, check a hash hit is received and a miss is dropped, then
check PM=1 (pass all multicast) overrides the hash table. Internal MAC loopback.
Proposed code: `MAC_PFR_HASH_MC.c` (ACTIVE_TEST to be assigned).

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| MAC_Packet_Filter.HMC [2] | Multicast DA filtered by the hash table. |
| MAC_Packet_Filter.PM [4] | Pass all multicast frames (overrides HMC). |
| MAC_Packet_Filter.HPF [10] | 0: hash table only. 1: hash table or perfect filter. |
| MAC_Hash_Table_Reg0 / Reg1 (0x0010 / 0x0014) | 64-bit hash table: index 0..31 → Reg0 bit idx, 32..63 → Reg1 bit idx − 32. |
| RDES3.RS2V [27] (last descriptor) | RDES2 status valid. |
| RDES2.HF [18] | Frame passed the hash filter. |
| RDES2.MADRM [26:19] | With HF=1: the hash index that matched. |

Hash index: same as MAC_PFR_HASH_UC_flow.md (reflected CRC-32 of the DA, bits 0..5 reversed).
DA 01:00:5E:00:00:01 → index 32 (Reg1 bit 0).

## Setup
1. MAC_Configuration: LM=1, FES=1, DM=1, CST=1, ACS=1, TE=RE=1.
2. MAC_Packet_Filter: HMC=1; PR=RA=PM=HPF=HUC=DAIF=0.

Save MAC_Configuration, MAC_Packet_Filter, MAC_Hash_Table_Reg0/Reg1; restore at the end.
Frame (64 B, FCS excluded): DA = 01:00:5E:00:00:01 (multicast: DA bit 0 = 1) | SA = MAC0 | 0x88B5 | pattern.
idx = hash index of DA (printed with its register and bit).

## Cases

| # | Setting | Hash table | Expected |
|---|---|---|---|
| 1 Hit | HMC=1, PM=0 | only bit idx (32: Reg1 bit 0) | Received: RS2V=1, HF=1, MADRM = idx, ES=0, PL=64, all bytes equal |
| 2 Miss | HMC=1, PM=0 | only bit (idx + 1) & 63 (33: Reg1 bit 1) | Dropped: no frame within 10 ms |
| 3 Pass all | HMC=1, PM=1 | same as case 2 (miss) | Received, PL=64, all bytes equal; HF logged |

Per case: write MAC_Packet_Filter.PM and MAC_Hash_Table_Reg0/Reg1 (readback) → send → receive.
Print TDES3, RDES2, RDES3 (RS2V, HF, MADRM, ES, PL). TX timeout or RX stalled → stop.
`RESULT: PASS/FAIL` per case, then `N/3 PASS`.

## Open points
- Case 3: HF value when PM passes the frame is not defined in the test; log only.
- Optional case: broadcast DA FF:FF:FF:FF:FF:FF with the miss table → still received
  (broadcast is not hash-filtered unless MAC_Packet_Filter.DBF=1).
