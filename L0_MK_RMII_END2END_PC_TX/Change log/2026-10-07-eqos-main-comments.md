# 2026-10-07 - Detailed EQoS comments in main.c

## Scope

Added explanatory comments to `eth_4/CM7/Core/Src/main.c` in the
`D:/eqos_repo/eqos-axiado` repository. No executable statements, register values,
function signatures, or sequencing were changed.

## Documentation added

- MAC, MTL and DMA responsibilities, TX/RX data flow, absolute block bases and
  relative register offsets, and the distinction between MDIO and RMII.
- RMII pin assignments, alternate-function bit fields, clock enables/readbacks,
  peripheral reset versus DMA software reset, and live status polling.
- Current 224 MHz clock arithmetic, the microsecond tick, and MDC clock selection.
- DMA TX/RX burst lengths, PBLX8, RX buffer size, descriptor skip length, AHB bus
  controls, and the difference between field replacement and OR-only updates.
- Descriptor read/write-back formats, DMA-accessible memory, cache assumptions,
  OWN ownership, memory barriers, ring lengths, tail pointers and wraparound.
- MTL store-and-forward, FIFO flush and checksum-error filtering behavior.
- MAC address packing, promiscuous filtering, speed/duplex, loopback, CRC/padding,
  and the independent MAC and DMA enable bits.
- Raw-frame construction, TX publication/completion/timeouts, RX validation,
  buffer lifetime during UART output, and descriptor recycling.
- LAN8742 bus callbacks, Clause-22 MDIO command ordering, and button request handling.

## Existing experimental settings documented

The source currently enables MAC loopback and comments out LAN8742 initialization
and link negotiation. Consequently, software speed/duplex retain their zero-initialized
values. DMA ST/SR are set before ring initialization and registration. Comments
explain these settings and their debugging implications; this update does not fix
or otherwise alter them. The existing RX tail convention is also preserved.

## Validation

- Compiled the original and annotated translation units with the installed
  Arm GNU 13.3 toolchain and the CM7 Debug target's CPU, ABI, defines, include
  paths and optimization options. Disabled debug information (`-g0`) for the
  object comparison because added comments necessarily change source line numbers.
- Both compilations succeeded. Their object files are byte-for-byte identical:
  SHA-256 `3660E26A5E0C99837E9D88DD94F48D2D5BB1F1052A1EA989E26307AAE96043FF`.
- Preprocessed both versions with `-E -P`; outputs are byte-for-byte identical:
  SHA-256 `B4BAB90773362DC7E62FCA363DD0591D417B424FAC819DAAF72CA999FAB35C09`.
- Both versions report the same existing warnings: `%x` versus `uint32_t` in the
  MAC control print, and unused `LAN8742` / `LAN8742_IOCtx` while PHY init is disabled.
- No firmware flashing or hardware packet test was performed for this comments-only
  update. This check does not validate the existing experimental startup sequence.

## References

- Bundled `CM7/baremetal/eqos_hw.h`, `eqos.h`, LAN8742 component, linker script,
  `stm32h755xx.h` and `stm32h7xx_hal_eth.h` for the actual project definitions.
- [ST RM0399 reference manual](https://www.st.com/resource/en/reference_manual/rm0399-stm32h745755-and-stm32h747757-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)
  for further Ethernet register documentation.
- [Synopsys Ethernet MAC architecture](https://www.synopsys.com/designware-ip/interface-ip/ethernet/ether-mac10-100-universal.html)
  for the MAC/MTL/DMA layer terminology.
