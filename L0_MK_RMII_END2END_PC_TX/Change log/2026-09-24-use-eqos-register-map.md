# Use eqos_hw.h for inline Ethernet register access - 2026-09-24

## Changes

- CM7/Core/Src/main.c: replaced every ETH-> register access with
  EQOS_REG32(EQOS_BASE_ADDR, EQOS_...offset), including MAC/DMA reset,
  configuration, ring setup, TX/RX tails, diagnostics and LAN8742 MDIO callbacks.
  The sequence remains directly inside main(); no EQoS driver functions added.
- CM7/baremetal/eqos_hw.h: added the volatile 32-bit MMIO lvalue macro
  EQOS_REG32(base, offset). It expands to a register dereference at base+offset.
  Added DMA interrupt-enable offset 0x1134, RX DISTCPEF bit 6, and H755 AHB
  fixed-burst/alignment bits 0/12. AHB names are explicitly separate from the
  generic AXI system-bus definitions so their fields are not interchanged.
- CM7/baremetal/eqos_port.c: compile-time assertions check all 25 accessed
  Ethernet offsets and the three added masks against the vendor CMSIS map.
  These checks generate no runtime register accesses.
- CM7/baremetal/README.md: describe the current register access convention.

Example:

```c
EQOS_REG32(EQOS_BASE_ADDR, EQOS_DMA_MODE) |= EQOS_DMA_MODE_SWR;
```

EQOS_BASE_ADDR remains mapped to the STM32 ETH_BASE (0x40028000), as requested
in the earlier base-mapping change. Register constants remain relative offsets,
so the base is added once. MODIFY_REG is only a generic read/modify/write macro;
its Ethernet operand now uses the EQoS register map.
GPIO/RCC/SYSCFG/DWT/NVIC and system clock setup still use CMSIS definitions
because eqos_hw.h describes the Ethernet peripheral, not those peripherals.
The vendor headers and LAN8742 implementation are unchanged.

## Validation

- CM7 Debug build/link passed without warnings or errors.
- main.c and eqos_port.c passed -Wall -Wextra -Werror syntax checks.
- All 25 register-offset and added mask assertions passed.
- Source check confirms no ETH-> accesses or ETH_MAC/MTL/DMA mask names
  remain in main.c.
- ELF: CM7/Debug/eth_4_CM7.elf. Build log:
  CM7/Debug/eqos-register-access-build.log.
- Hardware packet testing was not performed; this changes register notation,
  preserving the addresses, field values and sequence.
