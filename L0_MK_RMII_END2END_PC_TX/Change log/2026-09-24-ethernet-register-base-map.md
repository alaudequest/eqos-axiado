# Map EQoS to STM32H755 Ethernet base - 2026-09-24

## Changes

- `CM7/baremetal/eqos_hw.h` includes the CMSIS device header for STM32H755
  and defines `EQOS_BASE_ADDR = ETH_BASE`.
- `EQOS_MAC_BASE = ETH_MAC_BASE - EQOS_BASE_ADDR` explicitly derives the
  MAC block offset from the vendor map. Other targets retain offset zero.
- `CM7/Core/Src/main.c` sets `cfg.base = EQOS_BASE_ADDR`.
- `CM7/baremetal/eqos_port.c` checks the MAC base, configuration, filter,
  MDIO, and MAC address offsets against CMSIS with compile-time assertions.
- The vendor CMSIS header is unchanged.

## Address mapping

The supplied stm32h755xx.h defines ETH_MAC_BASE as ETH_BASE, at 0x40028000.
EQoS accesses registers as `dev->base + offset`; its MAC block offset must
therefore remain zero. Making EQOS_MAC_BASE an absolute address would add
the peripheral base twice.

| Symbol | Value | Meaning |
| --- | --- | --- |
| EQOS_BASE_ADDR / ETH_BASE / ETH_MAC_BASE | 0x40028000 | Absolute base |
| EQOS_MAC_BASE | 0x0000 | MAC block offset |
| EQOS_MAC_CONFIGURATION | 0x0000 | MACCR at 0x40028000 |
| EQOS_MAC_MDIO_ADDRESS | 0x0200 | MACMDIOAR at 0x40028200 |
| EQOS_MAC_MDIO_DATA | 0x0204 | MACMDIODR at 0x40028204 |
| EQOS_MAC_ADDRESS0_HIGH | 0x0300 | MACA0HR at 0x40028300 |
| EQOS_MAC_ADDRESS0_LOW | 0x0304 | MACA0LR at 0x40028304 |

## Validation

CM7 Debug build/link passed without warnings or errors; all register-map
assertions passed. Log: `CM7/Debug/register-map-build.log`.
ELF size is unchanged: text 29,192, data 144, BSS/NOLOAD 21,740 bytes.
This makes the existing address mapping explicit; hardware behavior is unchanged.
