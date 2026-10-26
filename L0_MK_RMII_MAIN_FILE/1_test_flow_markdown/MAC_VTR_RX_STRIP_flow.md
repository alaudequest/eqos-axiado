# MAC_VLAN_Tag — RX VLAN tag stripping: test code flow

Type: **step-by-step code flow**. Register names follow the Synopsys DWC Ethernet QoS (EQOS) databook (MAC_VLAN_Tag = ETH_MACVTR on STM32H7); confirm offsets/bits against the target chip's databook version.
Scope: prove **MAC_VLAN_Tag.EVLS** (all 4 encodings) and **EVLRXS** work. Other registers are set only where the test cannot run without them.

---

## 1. Feature

**MAC_VLAN_Tag** (0x0050)

| Field | Bits | Meaning |
|---|---|---|
| `VL` | [15:0] | VLAN tag to compare. With `ETV=1` only the 12-bit VID [11:0] is compared. |
| `ETV` | [16] | 12-bit VID compare. |
| `EVLS` | [22:21] | RX outer VLAN tag stripping: 00 = never, 01 = strip if VLAN filter passes, 10 = strip if VLAN filter fails, 11 = always. |
| `EVLRXS` | [24] | Report the outer VLAN tag in the RX descriptor (RDES0). |

Stripping removes the 4 tag bytes (TPID 0x8100 + TCI) after the SA, so the received frame is 4 bytes shorter.

---

## 2. Status checked

| Register | Field | Expected |
|---|---|---|
| RDES3 write-back (last descriptor) | `PL` [14:0] | 64 (not stripped) or 60 (stripped) |
| RDES3 write-back | `RS0V` [25] | 1 (RDES0 valid) |
| RDES0 write-back | `OVT` [15:0] | TCI of the sent frame (tag reported even when stripped) |
| RX data | bytes 12..15 | not stripped: `81 00 TCI`; stripped: EtherType `88 B5` + payload at byte 12 |

Log only: RDES2 `VF` [15] (VLAN filter pass), RDES3 `LT` [18:16].

---

## 3. Setup (only what the test needs)

Save MAC_VLAN_Tag and MAC_Packet_Filter; restore at the end.

| Setting | Why |
|---|---|
| `MAC_VLAN_Tag`: `VL=100`, `ETV=1`, `EVLRXS=1` | Filter compare result (pass for VID 100, fail for VID 101) selects stripping for EVLS=01/10; EVLRXS gives the tag in RDES0. |
| `MAC_Packet_Filter.VTFE=0` | VID 101 (filter fail) must still be received to observe EVLS=10/11. |

Assumed from the normal init: internal MAC loopback, TX/RX and DMA started, store-and-forward.
Frame (64 B, FCS excluded): `DA = SA = MAC_Address0 | 0x8100 | TCI | 0x88B5 | pattern`, PCP=5, DEI=1 → TCI = 0xB064 (VID 100) / 0xB065 (VID 101). Built with `eth_construct_vlan_frame()`.

---

## 4. Test cases

Each case: write `MAC_VLAN_Tag.EVLS` first (readback), then send the frames. For each frame:

1. Build and send the frame; receive it. TX/RX timeout → stop, restore, "reset before rerun".
2. Read PL, RS0V, RDES0.OVT; print one line (log VF).
3. Compare data: not stripped → whole frame equal; stripped → RX = TX with bytes 12..15 removed.
4. PASS if PL, RS0V, OVT and data are as expected.

| Case | EVLS | Frame VID (filter) | Expected |
|---|---|---|---|
| 1 | 00 never | 100 (pass) | not stripped, PL=64, OVT=0xB064 |
| 2 | 01 if pass | 100 (pass) | stripped, PL=60, OVT=0xB064 |
| | | 101 (fail) | not stripped, PL=64, OVT=0xB065 |
| 3 | 10 if fail | 100 (pass) | not stripped, PL=64 |
| | | 101 (fail) | stripped, PL=60, OVT=0xB065 |
| 4 | 11 always | 101 (fail) | stripped, PL=60, OVT=0xB065 |

Result: `RESULT: PASS/FAIL` per case, then `N/4 PASS`.

---

## 5. Open point

- With `VTFE=0`, the VLAN compare must still run for EVLS=01/10 to work: confirm with RDES2.VF (1 for VID 100, 0 for VID 101). If not, run Case 2/3 with `VTFE=1` and VID 100 only.
