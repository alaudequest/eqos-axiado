# MAC_VLAN_Incl — TX VLAN tag insert / replace / delete: test code flow

Type: **step-by-step code flow**. Register names follow the Synopsys DWC Ethernet QoS (EQOS) databook (MAC_VLAN_Incl = ETH_MACVIR on STM32H7); confirm offsets/bits against the target chip's databook version.
Scope: prove **MAC_VLAN_Incl** register mode works (VLC, VLT, CSVL, with VLP=1). Other registers are set only where the test cannot run without them.

---

## 1. Feature

**MAC_VLAN_Incl** (0x0060)

| Field | Bits | Meaning |
|---|---|---|
| `VLT` | [15:0] | VLAN tag (TCI: PCP[15:13], DEI[12], VID[11:0]) used for insert/replace. |
| `VLC` | [17:16] | TX VLAN control: 00 = none, 01 = delete, 10 = insert, 11 = replace. |
| `VLP` | [18] | 1: use `VLC` from this register (0: VLC ignored, external control input used). |
| `CSVL` | [19] | Inserted/replaced tag type: 0 = C-VLAN 0x8100, 1 = S-VLAN 0x88A8. |
| `VLTI` | [20] | 1: tag control comes from the TX descriptor. Kept 0 in this test (register mode). |

The MAC changes the frame on TX and recalculates the FCS (TDES3 `CPC=00`).

---

## 2. Status checked

The change is seen on the looped-back RX frame:

| Check | Expected |
|---|---|
| RDES3 `PL` [14:0] | 64 (tagged) or 60 (untagged) |
| RX bytes 12..15 | tag inserted/replaced: TPID (`81 00` or `88 A8`) + VLT; deleted: no tag, EtherType `88 B5` at byte 12 |
| Rest of RX data | equal to the expected frame |
| RDES3 `ES` [15] | 0 |

---

## 3. Setup (only what the test needs)

Save MAC_VLAN_Incl; restore at the end (VLC=00).

| Setting | Why |
|---|---|
| `MAC_VLAN_Incl.VLP=1`, `VLTI=0`, `VLT=0xB064` | Register mode; VLT = PCP 5, DEI 1, VID 100. |

Assumed from the normal init: internal MAC loopback, TX/RX and DMA started, RX VLAN stripping off (`MAC_VLAN_Tag.EVLS=00`, reset value) and VLAN filter off (`MAC_Packet_Filter.VTFE=0`), so the RX frame shows exactly what TX sent.
TX descriptor: `CPC=00` (CRC + pad insertion), as sent by `eth_tx_frame_send()` with fd_ctrl=0.

Frames (FCS excluded), DA = SA = MAC_Address0, pattern payload:

| Name | Layout | Size |
|---|---|---|
| Untagged | `DA SA 88B5 payload` | 60 B (`eth_construct_frame`) |
| Tagged | `DA SA 8100 0xB065 88B5 payload` (VID 101) | 64 B (`eth_construct_vlan_frame`) |

---

## 4. Test cases

Each case: write MAC_VLAN_Incl first (readback), then send one frame:

1. Build the expected RX frame from the TX frame and the case (insert/replace/delete the 4 tag bytes at byte 12).
2. Send; receive. TX/RX timeout → stop, restore, "reset before rerun".
3. Print PL and RX bytes 12..15.
4. PASS if ES=0, PL and the whole frame equal the expected frame.

| Case | MAC_VLAN_Incl | TX frame | Expected RX |
|---|---|---|---|
| 1 Insert C-VLAN | VLC=10, CSVL=0 | untagged 60 B | 64 B, bytes 12..15 = `81 00 B0 64` |
| 2 Replace | VLC=11, CSVL=0 | tagged 64 B (0xB065) | 64 B, bytes 12..15 = `81 00 B0 64` |
| 3 Delete | VLC=01 | tagged 64 B (0xB065) | 60 B, = untagged frame |
| 4 Insert S-VLAN | VLC=10, CSVL=1 | untagged 60 B | 64 B, bytes 12..15 = `88 A8 B0 64` |

Result: `RESULT: PASS/FAIL` per case, then `N/4 PASS`.

---

## 5. Open points

- `VLP=0`: the external control input may not be connected on the target chip; not tested here (VLP=1 in all cases).
- Descriptor mode (`VLTI=1`, TDES2 `VTIR`, tag from a TX context descriptor) and the inner tag register MAC_Inner_VLAN_Incl are a separate test; the current TX helper does not set TDES2 VTIR or context descriptors.
