# MAC_VLAN_Tag — Double VLAN (S-VLAN / inner tag): test code flow

Type: **step-by-step code flow**. Register names follow the Synopsys DWC Ethernet QoS (EQOS) databook (MAC_VLAN_Tag = ETH_MACVTR on STM32H7); confirm offsets/bits against the target chip's databook version.
Scope: prove the double VLAN bits of **MAC_VLAN_Tag** work. Other registers are set only where the test cannot run without them.

---

## 1. Feature

**MAC_VLAN_Tag** (0x0050)

| Field | Bits | Meaning |
|---|---|---|
| `VL`, `ETV` | [15:0], [16] | VLAN tag to compare (12-bit VID when ETV=1). |
| `ESVL` | [18] | S-VLAN (TPID 0x88A8) frames are treated as VLAN frames. |
| `ERSVLM` | [19] | Filter on S-VLAN (1) or C-VLAN (0) type tag. |
| `EDVLP` | [26] | Enable double VLAN processing. |
| `ERIVLT` | [27] | Filter on the inner tag (1) instead of the outer tag (0). |
| `EIVLS` | [29:28] | RX inner tag stripping (11 = always). |
| `EIVLRXS` | [31] | Report the inner tag in RDES0. |

---

## 2. Status checked

| Register | Field | Meaning |
|---|---|---|
| RX result | received / dropped | VLAN filter result (VTFE=1) |
| RDES3 write-back | `PL` [14:0], `RS0V` [25] | Length, RDES0 valid |
| RDES0 write-back | `IVT` [31:16], `OVT` [15:0] | Inner / outer tag (Case 3) |

Log only: RDES3 `LT` [18:16] (expect 101 = double VLAN).

---

## 3. Setup (only what the test needs)

Save MAC_VLAN_Tag and MAC_Packet_Filter; restore at the end.

| Setting | Why |
|---|---|
| `MAC_VLAN_Tag`: `EDVLP=1`, `ESVL=1`, `ETV=1` (all cases) | Double VLAN processing with an S-VLAN outer tag, 12-bit VID compare. |
| `MAC_VLAN_Tag`: `VTIM=0`, `VTHM=0`, `EVLS=00` (all cases, written explicitly) | No inverse / hash match, no outer strip, so leftover state from other tests cannot change the result. |
| `MAC_Packet_Filter.VTFE=1` | Frames failing the VLAN filter are dropped. |

Assumed from the normal init: internal MAC loopback, TX/RX and DMA started.
Setup also sets `MAC_Configuration` LM/FES/DM/CST/ACS/TE/RE: CST=1, so expected PL = 68 (64 in Case 3).
Frame (68 B, FCS excluded), built by hand (no helper for two tags):
`DA = SA = MAC_Address0 | 0x88A8 | S-TCI | 0x8100 | C-TCI | 0x88B5 | pattern`
Outer S-TCI = VID 10 (0x000A) or 11; inner C-TCI = PCP 5, DEI 1, VID 100 (0xB064) or 101.

---

## 4. Test cases

Each case: write MAC_VLAN_Tag first (readback), then send the frames. For each frame:

1. Build and send; receive with a 10 ms timeout. TX timeout or RX stalled → stop, restore.
2. Print received / dropped, PL, RS0V, RDES0, LT.
3. PASS as in the table.

| Case | MAC_VLAN_Tag (on top of setup) | Frame outer / inner VID | Expected |
|---|---|---|---|
| 1 Outer S-VLAN filter | `ERIVLT=0`, `ERSVLM=1`, `VL=10` | 10 / 100 | received, PL=68 |
| | | 11 / 100 | dropped |
| 1b Outer tag type mismatch | `ERIVLT=0`, `ERSVLM=0`, `VL=10` | 10 / 100 | dropped (outer is 0x88A8, filter expects C-VLAN) |
| 2 Inner C-VLAN filter | `ERIVLT=1`, `ERSVLM=0`, `VL=100` | 10 / 100 | received, PL=68 |
| | | 10 / 101 | dropped |
| 3 Inner tag status + strip | Case 2 + `EIVLRXS=1`, `EVLRXS=1`, `EIVLS=11` | 10 / 100 | received, PL=64, inner tag (bytes 16..19) removed, RS0V=1, IVT=0xB064, OVT=0x000A |

Result: `RESULT: PASS/FAIL` per frame, then `N/N PASS` per file.

**Files** (one `ACTIVE_TEST` each, in `0_test_cases/`):

| File | ACTIVE_TEST | Cases above |
|---|---|---|
| `MAC_VTR_DVLAN_OUTER.c` | `TEST_MAC_VTR_DVLAN_OUTER` (26) | 1, 1b (3 frames) |
| `MAC_VTR_DVLAN_INNER.c` | `TEST_MAC_VTR_DVLAN_INNER` (27) | 2 (2 frames) |
| `MAC_VTR_DVLAN_STRIP.c` | `TEST_MAC_VTR_DVLAN_STRIP` (28) | 3 (1 frame); IVT/OVT log only until `DVLAN_STRIP_CHECK_TAGS=1` |

---

## 5. Open point

- Case 3 OVT/IVT layout and LT value: confirm against the databook RDES0 description on the first run (log first, then make pass/fail).
