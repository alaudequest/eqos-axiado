# Functions, RM0399 bring-up order, and one folder per block - 2026-10-15

## Request and result

The inline `main()` (about 900 lines) was restructured in three steps. The
first and last steps keep the behavior unchanged; the second one changes it.

1. **Functions.** Each FLOW step became its own function, and every
   configuration value moved into one `CFG` table in `main.c`, grouped per
   block (board, DMA, MTL, MAC, link, test). Function names follow the
   pre-inline driver (`Change log/2026-09-24-main-before-inline.txt`).
2. **RM0399 fixes.**
   - DMA ST/SR are now set only after the rings, MTL and MAC are configured
     (61.9.1 step 9, p.3032). Before, ST/SR were set before DLAR was written;
     p.3080/3082 call that "unpredictable", and p.3083 says the DMA takes DLAR
     when ST goes 0 -> 1.
   - MAC loopback runs at 100 Mbit/s full duplex. 61.5.8 (p.2998) allows
     loopback only in full duplex. The old code ran at 10 Mbit/s half duplex
     because the removed PHY code left `eth.speed` and `eth.duplex_full` at 0.
   - FTQ is no longer set during MTL init: the FIFO is empty after SWR, and
     61.9.2 does not list FTQ. It came from the pre-inline `eqos_adjust_link()`
     workaround for switching to half duplex at run time.
3. **Folders.** The code was split into one folder per functional block under
   `CM7/Core/Src`, in the order of RM0399 61.9. The table is in
   `CM7/baremetal/README.md`, and the comment "BAN DO FILE" at the top of
   `main.c` maps each log step to its file.

## Layout (paths under CM7/Core/Src)

- `main.c`: `CFG` table, `main()`, board setup (`board_config`,
  `SystemClock_Config`), `print_config`, USER button callback, `Error_Handler`.
- `core/`: `eqos.h` (shared types, per-block config structs, descriptor
  layout, `extern` for `eth`, `tx_desc`, `rx_desc`, `eqos_*` API),
  `eqos_hw.h` (unchanged register map), `eqos.c` (`eth` and `eqos_reset()`).
- `port/`: `eqos_port.h` and `eqos_port.c`, the STM32 glue (`stm32_precheck`,
  `stm32_config_eth` and its helpers, `eqos_udelay`, register-map checks).
  `stm32_precheck()` now returns `EQOS_EINVAL` instead of calling
  `Error_Handler()`, so the driver files do not depend on `main.h`.
- `dma/eqos_dma.c`, `desc/eqos_desc.c`, `mtl/eqos_mtl.c`, `mac/eqos_mac.c`: one
  block each. The FLOW bit-decode table of each register lives next to the
  code that prints it.
- `flow/`: `eqos_flow.h` and `eqos_flow.c`, the FLOW log macros and helpers.
  `FLOW_LOG` is set in `eqos_flow.h` and can be overridden with `-DFLOW_LOG=n`.
- `test/`: `test_loopback.h` and `test_loopback.c`, the test frame, one TX with
  result print, RX poll with compare.

Files moved out of `CM7/baremetal`: `eqos.h`, `eqos_hw.h`, `eqos.c`,
`eqos_port.c`. That folder now keeps only its README, `register_index.md`,
`docs/`, and the excluded `example_main.c`. `CM7/.cproject` is unchanged:
`Core` is already a source folder, and headers are included with relative
paths such as `"../core/eqos.h"`.

## Verification

- Built with the CubeIDE 1.19 toolchain (GCC 13.3) outside `CM7/Debug`. No
  warnings at `FLOW_LOG` 0, 1 and 2, also with `-Wextra`.
- Every function body was compared with the pre-split `main.c`, ignoring
  comments and whitespace. All are identical except the intended
  `stm32_precheck()` / `print_config()` split, so the log keeps the same lines
  in the same order.
- Descriptors and buffers are still in RAM_D1 and aligned to 32 bytes.
  Their addresses depend on the link order. With objects sorted as CubeIDE
  writes `objects.list`, they are: `tx_buf` 0x24000100, `rx_buf` 0x24001A00,
  `tx_desc` 0x24004E80, `rx_desc` 0x24004EC0.
- Not yet run on the board at the time of writing.

## Notes

- In STM32CubeIDE, refresh the project (F5) before building so the managed
  build sees the new folders and drops the removed files. VS Code builds go
  through CubeIDE headless and pick them up.
- The HTML documents in `docs/` still describe the old single `main()`.
