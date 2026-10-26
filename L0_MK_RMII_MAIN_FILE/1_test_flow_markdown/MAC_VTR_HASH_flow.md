# MAC_VLAN_Tag — VLAN hash filter: test code flow

Type: **step-by-step code flow**. Register names follow the Synopsys DWC Ethernet QoS (EQOS) databook (MAC_VLAN_Tag = ETH_MACVTR, MAC_VLAN_Hash_Table = ETH_MACVHTR on STM32H7); confirm offsets/bits against the target chip's databook version.
Scope: prove **MAC_VLAN_Tag.VTHM** with **MAC_VLAN_Hash_Table** works. Other registers are set only where the test cannot run without them.

---

## 1. Feature

| Register | Field | Bits | Meaning |
|---|---|---|---|
| MAC_VLAN_Tag (0x0050) | `VTHM` | [25] | VLAN hash match: the top 4 bits of the CRC of the VLAN tag index MAC_VLAN_Hash_Table. |
| MAC_VLAN_Tag | `ETV` | [16] | 1: CRC over the 12-bit VID. |
| MAC_VLAN_Hash_Table (0x0058) | `VLHT` | [15:0] | Bit `idx` = 1: VLAN frames whose hash = idx pass. |

Hash index (same as Linux stmmac `dwmac4_update_vlan_hash`):

1. CRC-32 (reflected polynomial 0xEDB88320, init 0xFFFFFFFF) over the 12 VID bits, LSB first.
2. Invert, bit-reverse, take the top 4 bits → idx 0..15.

Host-computed: **VID 100 → idx 0**, VID 1 → idx 8. Print the computed idx in the test.

---

## 2. Status checked

| Check | Hit | Miss |
|---|---|---|
| Frame received (RX within 10 ms) | yes | no (dropped) |
| RDES3 `ES` [15], `PL` [14:0] | ES=0, PL=64 | — |
| RX data | equal to TX | — |

Log only: RDES2 `VF` [15] (VLAN filter pass).

---

## 3. Setup (only what the test needs)

Save MAC_VLAN_Tag, MAC_VLAN_Hash_Table, MAC_Packet_Filter; restore at the end.

| Setting | Why |
|---|---|
| `MAC_VLAN_Tag`: `VTHM=1`, `ETV=1`, `VL=1` | Hash match on the 12-bit VID. VL=1 (≠ 100, different hash) so a perfect VID match cannot let the frame in. Do not use VL=0: with VL=0 all VLAN frames match. |
| `MAC_Packet_Filter.VTFE=1` | Drop VLAN frames that fail the VLAN filter. |

Assumed from the normal init: internal MAC loopback, TX/RX and DMA started.
Frame (64 B, FCS excluded): `DA = SA = MAC_Address0 | 0x8100 | TCI (VID 100) | 0x88B5 | pattern`. Built with `eth_construct_vlan_frame()`.

---

## 4. Test cases

Each case: write MAC_VLAN_Hash_Table first (readback), then send one frame (VID 100, idx 0).

1. Build and send the frame; receive with a 10 ms timeout. TX timeout or RX stalled mid-frame → stop, restore.
2. Print received / not received, VF, PL.
3. PASS as in section 2.

| Case | MAC_VLAN_Hash_Table | Expected |
|---|---|---|
| 1 Hit | only bit idx (0x0001) | received, ES=0, PL=64, data equal |
| 2 Miss | only bit (idx + 1) & 15 (0x0002) | dropped (RX timeout) |

Result: `RESULT: PASS/FAIL` per case, then `N/2 PASS`.

---

## 5. Open points

- Is the CRC computed over 12 VID bits exactly as Linux does? If Case 1 fails: set VLHT=0xFFFF (frame must pass), then one bit at a time to find the hardware index.
- Does a VL perfect match also pass when VTHM=1? Not needed for this test (VL=1 never matches VID 100).
