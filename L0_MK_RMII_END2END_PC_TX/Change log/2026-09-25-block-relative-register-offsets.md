# Block-relative register offsets and combined configuration - 2026-09-25

## Changes

- CM7/baremetal/eqos_hw.h: all 44 MAC/MTL/DMA register constants now contain
  plain relative byte offsets, not block-base-plus-offset expressions.
  The three block base constants are now absolute MMIO addresses:

  | Block base | Address | Example relative offset |
  | --- | --- | --- |
  | EQOS_MAC_BASE | 0x40028000 | EQOS_MAC_MDIO_ADDRESS = 0x200 |
  | EQOS_MTL_BASE | 0x40028D00 | EQOS_MTL_RXQ0_OPERATION_MODE = 0x30 |
  | EQOS_DMA_BASE | 0x40029000 | EQOS_DMA_CH0_TX_CONTROL = 0x104 |

  EQOS_MTL_BASE continues to identify the supplied driver's queue-0 register
  window at peripheral offset 0xD00; it does not identify the global MTL
  register window at 0xC00. This preserves existing register addresses.
  EQOS_BASE_ADDR remains the overall peripheral base used by configuration/state.
- CM7/Core/Src/main.c: all AX_BASE reads and writes now specify the matching
  MAC, MTL or DMA base, including reset, configuration, descriptor tails,
  diagnostics and MDIO callbacks.
- Step 5 reads each MAC/MTL control once. It combines MAC speed, duplex, CRC
  stripping, loopback and disabled TX/RX state into mac_control; MTL TX combines
  store-and-forward and the optional half-duplex flush into mtl_tx_control.
  Filter and flow settings likewise use local values followed by one write each.
- Step 6 retains one read and one write per DMA control. Step 8 reuses both
  DMA values and mac_control to enable TX/RX after descriptors are ready.
  No intervening writer updates these saved configuration values.
- Reset and MDIO command construction use local variables before writing.
  Hardware status polling (reset, MDIO busy and descriptor ownership) is retained;
  these reads must observe changing hardware state. Full-value registers need
  no read, and per-packet tail writes still occur whenever new work is submitted.
- CM7/baremetal/eqos_port.c: assertions now compare full absolute addresses
  (block base + relative offset) with the vendor map for all 25 accessed registers.
- CM7/baremetal/README.md: describe the new base/offset convention.

## Example

```c
/* eqos_hw.h */
#define EQOS_DMA_BASE           (EQOS_BASE_ADDR + 0x1000u)
#define EQOS_DMA_CH0_TX_CONTROL  0x104u

/* main.c: accesses 0x40029104 */
AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TX_CONTROL, tx_dma_control);
```

Do not pass EQOS_BASE_ADDR with the new DMA/MTL offsets: their definitions
have changed to block-relative values. Historical change logs show the old API.
The Ethernet sequence stays inside main(), and LAN8742.c/.h remain unchanged.

## Validation

- Checked all 44 converted offsets and all 47 AX_BASE accesses in main.c for
  matching block bases. Steps 5 and 6 contain at most one read and one write
  per configured register.
- All 25 absolute register-address assertions and existing bit-mask assertions
  passed against the bundled CMSIS map.
- CM7 Debug build/link and -Wall -Wextra -Werror checks passed without diagnostics.
  Log: CM7/Debug/block-relative-register-build.log.
- ELF: CM7/Debug/eth_4_CM7.elf; text 25,424 bytes, data 144, BSS/NOLOAD 21,748.
- Hardware packet testing was not performed.
