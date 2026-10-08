# Move EQoS implementation into main.c - 2026-09-24

## Changes

- Moved the full implementation from CM7/baremetal/eqos.c into
  CM7/Core/Src/main.c, below the application functions in the clearly marked
  USER CODE BEGIN EQOS_Implementation section. Preserved the original driver
  license notice and attribution alongside the moved code.
- Includes register helpers, MDIO and generic PHY helpers, MAC/MTL/DMA
  configuration, descriptor arrays and packet buffers, ring setup, init/stop,
  send, receive/release, link polling, MAC address setup, and register dump.
- The driver body was verified to be moved verbatim. Function names, public
  API, packet handling, LAN8742 integration, and descriptor logic are unchanged.
- eqos.c is now an empty compatibility translation unit, so existing CubeIDE
  source lists still build without duplicate function definitions.
- eqos.h and eqos_hw.h remain the API and register headers. eqos_port.c retains
  board GPIO/clocks/reset, timing, cache hooks, and logging.
- Updated CM7/baremetal/README.md to identify the new implementation location.
- Removed an existing invalid SET_BIT(heth.Instance->MACCPR, ETH_MACCPR_PR)
  statement found during compilation: heth no longer exists and MACCPR is not
  the packet-filter register. The existing cfg.promiscuous=true already enables
  promiscuous mode through eqos_config_mac, so its intent is preserved.

## Debugging

Set breakpoints directly in main.c at eqos_init, eqos_config_mac,
eqos_config_dma, eqos_setup_rings, eqos_rx_desc_arm, eqos_send, eqos_recv,
and eqos_recv_done. Descriptor arrays and eth are now in the same translation
unit. Do not include eqos.c or restore its implementation while main.c contains
these definitions. Refresh the project and load the rebuilt ELF before debugging.

The custom USER CODE section is a navigation marker; do not assume CubeMX will
preserve it during regeneration. Keep a backup before regenerating main.c.

## Validation

- CM7 Debug build and link passed without warnings or errors.
- Output: CM7/Debug/eth_4_CM7.elf.
- Log: CM7/Debug/eqos-main-build.log.
- nm and addr2line verified init, send, receive, ring setup and RX descriptor
  arming resolve to main.c in the ELF debug information.
- ELF size: text 29,180 bytes; data 144 bytes; BSS/NOLOAD 21,748 bytes.
  Moving translation units can change layout; use current symbols rather than
  descriptor addresses recorded in older change logs.
- Hardware packet testing was not performed.
