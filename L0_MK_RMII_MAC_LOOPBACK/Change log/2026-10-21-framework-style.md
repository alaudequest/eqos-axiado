# Framework style: src/ + includes/ - 2026-10-21

## Request and result

Rewrite `CM7/Core/Src` in the style of the Kirkwood validation framework
(`L0_B0_SGMII0_EXTERNAL_LOOPBACK`): a small test file with `do_test()` and test
options, and a flat `includes/` library with the framework names
(`egress_packet`, `ingress_packet`, `eth_test_traffic`, `ETH_PRINTF_L1..3`,
`AX_BASE_READ/WRITE_REG32`, `MAC_*_R_*_VAL`).

## Layout (paths under CM7/Core/Src)

- `main.c`: CubeMX board start (HAL, 224 MHz clock, UART COM1, USER button), then
  `do_test()`; the USER button runs `do_test()` again.
- `src/L0_MK_RMII_MAC_LOOPBACK.c`: `eth_run_test_ops` (0 = traffic test,
  1 = MAC address filter test) and `do_test()`.
- `includes/eth_function_stm32.c/.h`: the only STM32-specific part:
  `eth_rmii_mac_lb()` test flow, `eth_stm32_init()` (D-cache check, DWT, RMII
  pins, SYSCFG, RCC), `udelay/mdelay`, `uart_putc/puts/puthex`,
  `eth_csr_clock_hz`, 66 `_Static_assert` against CMSIS `ETH_TypeDef`.
- `includes/eth_functions.c`: EQoS code, no HAL: `mac_reset_100m`,
  `mac_dma_init`, `mac_desc_init`, `mac_mtl_init`, `mac_basic_init`,
  `mac_init_100m`, `init/enable/disable_mac_internal_lb`, `mac_start`,
  `egress_packet`, `ingress_packet(_status)`, `eth_rx_flush`,
  `generate_ethernet_packet`, `compare_packets`, `eth_test_traffic`,
  `eth_print_mac_counters`, `eth_mac_internal_lb`.
- `includes/eth_filter_functions.c`: the old `test/filter_test.c` (24 cases),
  now selected by `eth_run_test_ops = 1` instead of a constructor.
- `includes/eth_dump_functions.c`: the old FLOW decoders, gated by
  `DEBUG_LEVEL` (2 = register read-back, 3 = bit fields, state, rings, frames)
  and `ETH_DEBUG` (every packet).
- Headers: `eth_registers.h` (AX_*, `EQOS_BASE`, `MAC_100M_*` configuration,
  `MMC_STATS_*`), `mac_defines.h` (`R_*` offsets = RM0399 "Address offset",
  bits, `struct eth_desc`), `eth_traffic_test.h`, `eth_utils.h`,
  `eth_functions_utils.h`, `eth_printf.h`.

The old per-block folders (core, port, dma, desc, mtl, mac, flow, test) and the
old `main.c` are archived unchanged in
`Change log/2026-10-21-before-framework-style/CM7_Core_Src/`.
`CM7/.cproject` is unchanged.

## Behavior

- Register configuration unchanged: 100 Mbit/s full duplex, JE = 1,
  MACWTR = 0x100 (watchdog 2 KB), MTL threshold mode, PR = 1, ring 4 TX / 8 RX,
  buffer 2304. Only difference: `MACCR.LM` is set in its own read-modify-write
  (`0x00316000`, then `0x0031F000`) instead of inside the first MACCR write
  (`0x00317000`). Final MACCR is `0x0031F003` in both.
- Test changed to the framework traffic test: 16 frames of 76 bytes
  (payload 64 aligned to 62), DA 6C:1F:F7:CA:2B:FD (unicast: the H755 MMC
  only counts unicast RX), compare each frame, MMC counters, PASS/FAIL.
- 2300-byte watchdog test: set `ETH_TEST_TRAFFIC_SIZE` to 2286; with WTO = 0
  it FAILs on RDES3.RWT as expected, with WTO = 1 it passes.

## Verification

- CubeIDE 1.19 GCC 13.3, Debug flags: no warning in the new files, also with
  `-Wextra -Wformat=2 -Wshadow -Wundef`. text 39468, data 124, bss 37328.
- PC run against a software model of the EQoS (WSL gcc): traffic 16/16 PASS,
  filter 24/24, 2300 bytes WTO = 0 FAIL (RWT), WTO = 1 PASS. Register write
  sequence old vs new identical except the MACCR/LM writes above.
- Not yet run on the board.

## Notes

- In STM32CubeIDE: Refresh (F5), Project > Clean, Build.
- Documentation: `docs/MAC_LOOPBACK_framework.html` (document 8). Documents
  1-7 describe the old code.

## Update: eth_printf and FLOW log (same day)

- `eth_printf.c`: the user's char-by-char version (with `eth_putdec` /
  `eth_puthex`) is kept, but each argument is now read with its real type.
  The framework version read every `%d`/`%x` as `uint64_t`, which is wrong on
  the 32-bit Cortex-M7 and shifts every following argument.
  - Supports `%d %i %u %x %X %c %s %p %%`, the flags `-` and `0`, a width
    (number or `*`), and `l`, `ll`, `z`.
  - `%x` prints like printf (no automatic `0x`).
  - The prototype has `format(printf, 1, 2)`.
  - Ends with `uart_flush()`.
  - Checked against `snprintf`: 21/21 cases match.
- `eth_print_info()` removed, together with `uart_puts()` / `uart_puthex()`.
  `eth_test_traffic()` prints `Start Egress and Ingress packet(s): %d`.
- FLOW log back instead of the L2/L3 dumps: `eth_flow_utils.h` and
  `eth_flow_functions.c` replace `eth_dump_utils.h` and `eth_dump_functions.c`.
  - `FLOW_LOG` 0/1/2, `FLOW()`, `FLOW2()`, `FLOW_PKT()`, `FLOW_STEP()`,
    `flow_reg/flow_reg2/flow_regw/flow_fields`,
    `flow_dma_state/mtl_state/mac_state/mmc/rings/frame_dump`,
    `flow_print_cycles`.
  - Step tags [1]..[11] as in the old code.
  - `FLOW_PKT_MAX` (default 1) frames of `eth_test_traffic()` print [10]/[11].
- New platform hooks in `eth_function_stm32.c`: `uart_flush`,
  `eth_get_cycles` (DWT), `eth_cpu_clock_hz`, `eth_get_ms` (HAL tick).
  Chip info moved to step [1] in `main.c`.
- Verification:
  - Builds with no warning in the project sources at `FLOW_LOG` 0/1/2 with
    `-Wall -Wextra -Wformat=2 -Wshadow`. text 34812 / 39544 / 44732.
  - PC model results unchanged; register writes identical to before (the
    FLOW code only reads).
- `docs/MAC_LOOPBACK_framework.html` updated: new sections eth_printf and FLOW
  log, sample logs for FLOW_LOG 0/1/2.

## Update 2026-10-26: bit notes on top of the functions

- Each function that writes a register or a descriptor now has a table in its
  header comment: register (RM0399 page), bit [position], value written, and
  what the bit does.
  - `eth_functions.c`: `mac_reset_100m`, `mac_dma_init`, `mac_desc_init`,
    `mac_mtl_init`, `mac_basic_init`, `mac_start`, MACCR.LM functions,
    `egress_packet` (TDES0-3), `ingress_packet_status` (RDES0-3),
    MMC counters.
  - `eth_mac_internal_lb()` has a summary of every bit, in call order.
  - `eth_function_stm32.c`: D-cache, DWT, RMII pins, SYSCFG/RCC, NVIC.
  - `eth_filter_functions.c`: `apply_filter` (MACHT0R/HT1R, MACA1HR/LR,
    MACPFR).
- Only comments changed. The build gives the same RAM addresses and adds no
  warning.

## Update 2026-10-26: DMA burst test (eth_run_test_ops = 2)

- New `includes/eth_burst_functions.c` and `eth_burst_utils.h` add the
  `eth_mac_burst_lb()` test. It is selected in `eth_rmii_mac_lb()`.
- The test runs 4 modes of ETH_DMASBMR (FB/AAL = 1/1, 0/1, 1/0, 0/0).
  - Each mode starts with a reset and an init, then writes DMASBMR before the
    DMA starts.
  - It then sends 14 frames of 60 to 1514 bytes.
- Before each frame the RX buffer is filled with 0xAA. After RX, the test
  counts the bytes the DMA wrote after the frame end (PL).
  - FB=1: the databook says fixed bursts can add placeholder beats there.
  - FB=0: the databook says only the last 32-bit word can change.
- PASS means every frame came back the same. The burst columns only report
  what the hardware did.
- Verification:
  - ARM build: no new warning at FLOW_LOG 2 and 0.
  - PC model (scratchpad only): 56/56 SAME, exit 0. The model fakes the burst
    behaviour, so this only checks the detection code. The real result needs
    the board.
