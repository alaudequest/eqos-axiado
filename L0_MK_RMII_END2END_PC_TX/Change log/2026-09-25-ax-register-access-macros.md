# Use AX register access macros - 2026-09-25

## Changes

- Added BIT, AX_WRITE_REG32, AX_BASE_WRITE_REG32, AX_READ_REG32 and
  AX_BASE_READ_REG32 in CM7/baremetal/eqos_hw.h, replacing EQOS_REG32.
- BIT uses 1U rather than signed 1 so BIT(31) uses an unsigned shift.
  A definition guard avoids redefining BIT if already provided.
- Register arguments are parenthesized; base and offset are individually cast
  to uintptr_t before addition so offsets always use bytes. Reads/writes retain
  volatile uint32_t accesses and the user-supplied expression-style API.
- Converted all Ethernet register access in CM7/Core/Src/main.c, including
  LAN8742 MDIO callbacks, to AX_BASE_READ_REG32 / AX_BASE_WRITE_REG32.
- Converted seven MODIFY_REG operations and the compound bit-set writes to
  explicit read/modify/write expressions. Clear masks and set values remain
  parenthesized to preserve precedence. Existing __DSB barriers remain in place.
- Updated CM7/baremetal/README.md. GPIO/RCC/core setup, packet descriptors,
  PHY driver and the inline main() flow are unchanged.

## Example

```c
AX_BASE_WRITE_REG32(EQOS_BASE_ADDR, EQOS_DMA_MODE,
    AX_BASE_READ_REG32(EQOS_BASE_ADDR, EQOS_DMA_MODE) | EQOS_DMA_MODE_SWR);
```

## Validation

- CM7 Debug build/link passed without warnings or errors.
- main.c and eqos_port.c passed -Wall -Wextra -Werror syntax checks.
- No EQOS_REG32 or MODIFY_REG calls remain in main.c or eqos_hw.h.
- Register offset/mask assertions still pass.
- ELF size unchanged: text 25,156 bytes; data 144; BSS/NOLOAD 21,748.
- Output: CM7/Debug/eth_4_CM7.elf; log: CM7/Debug/ax-register-build.log.
- Hardware packet testing was not performed.
