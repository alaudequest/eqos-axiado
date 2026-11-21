# MAC_Packet_Filter HUC — unicast hash filter: test code flow

Scope: with HUC=1 and HPF=0, unicast frames are filtered only by the 64-bit hash table;
the perfect filter (MAC Address0) is not used. Send a frame whose DA is not MAC Address0,
so the hash table is the only way in. Check a hash hit is received and a miss is dropped.
Internal MAC loopback.

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| MAC_Packet_Filter.HUC [1] | Unicast DA filtered by the hash table. |
| MAC_Packet_Filter.HPF [10] | 0: hash table only. 1: hash table or perfect filter. |
| MAC_Hash_Table_Reg0 / Reg1 (0x0010 / 0x0014) | 64-bit hash table: index 0..31 → Reg0 bit idx, 32..63 → Reg1 bit idx − 32. |
| RDES3.RS2V [27] (last descriptor) | RDES2 status valid. |
| RDES2.HF [18] | Frame passed the hash filter. |
| RDES2.MADRM [26:19] | With HF=1: the hash index that matched. |

Hash index: CRC-32 of the 6 DA bytes (Ethernet CRC), bit-reversed; the top 6 bits are the
index (same as Linux stmmac). Software uses the reflected CRC (poly 0xEDB88320, init
0xFFFFFFFF, final invert), so the index is reflected bits 0..5 read in reverse order.
Example: DA 02:00:00:00:00:01 → CRC 0x8B0D303E → bits 5..0 = 111110 → reversed 011111 = 31.

## Setup
1. MAC_Configuration: LM=1, FES=1, DM=1, CST=1, ACS=1, TE=RE=1.
2. MAC_Packet_Filter: HUC=1; PR=RA=PM=HPF=HMC=DAIF=0.

Frame (64 B, FCS excluded): DA = 02:00:00:00:00:01 (not MAC0) | SA = MAC0 | 0x88B5 | pattern.
idx = hash index of DA (printed with its register and bit).

## Cases

| # | Hash table | Expected |
|---|---|---|
| 1 Hit | only bit idx set (31: Reg0 bit 31) | Received: RS2V=1, HF=1, MADRM = idx, ES=0, PL=64, all bytes equal |
| 2 Miss | only bit (idx + 1) & 63 set (32: Reg1 bit 0) | Dropped: no frame within 10 ms |

Per case: write MAC_Hash_Table_Reg0/Reg1 (whole registers, readback) → send → receive.
Print TDES3, RDES2, RDES3 (RS2V, HF, MADRM, ES, PL). TX timeout or RX stalled → stop.
`RESULT: PASS/FAIL` per case, then `N/2 PASS`.

## End
MAC_Packet_Filter.HUC=0, MAC_Hash_Table_Reg0/Reg1 = 0.

## Open point
- Restore clears HUC and the hash table only; MAC_Configuration and the other
  MAC_Packet_Filter bits are not saved/restored (same gap as noted in the earlier review).
