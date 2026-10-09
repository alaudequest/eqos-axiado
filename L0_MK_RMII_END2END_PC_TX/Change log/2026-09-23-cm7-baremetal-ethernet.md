# CM7 bare-metal Ethernet integration - 2026-09-23

## Objective and scope

Replace the CM7 application HAL_ETH path with the existing `CM7/baremetal`
polling driver. Keep the system clock configuration, HAL tick, COM1 UART,
button BSP, MAC addresses, and test payload. CM4 is unchanged. The previously
commented-out CM7 dual-core boot handshake remains inactive.

## Changed files and reasons

- `CM7/Core/Src/main.c`: use `eqos_init`, blocking `eqos_send`, polling
  `eqos_recv`, and `eqos_recv_done`. Remove the obsolete HAL ETH handle,
  descriptors, RX allocation callbacks, PHY HAL wrappers, and unused dumps.
  Small `MX_ETH_Init`, `ETH_SendPacket`, and `ETH_ReceivePacket` functions provide
  simple breakpoint locations. UART is initialized before Ethernet so init
  failures are visible. Button interrupt only requests a send in the main loop.
- `CM7/baremetal/eqos_port.c`: replace placeholder board code with direct RCC,
  SYSCFG RMII selection, GPIO AF11, and Ethernet reset setup. Use DWT CYCCNT for
  microsecond delays at SystemCoreClock. Keep the Ethernet NVIC IRQ disabled.
  Add compile-time register-offset checks against the vendor CMSIS ETH_TypeDef.
- `CM7/baremetal/eqos.h`: STM32H755 selects a 32-bit DMA bus, 32-byte alignment,
  and uncached descriptor/buffer handling. D-cache must stay disabled; the board
  port returns EQOS_EINVAL if it is enabled. No MPU configuration is added.
- `CM7/baremetal/eqos.c`: preserve the existing descriptor rings and send/receive
  algorithms. Add STM32 guards to avoid unsupported multi-queue priority/control
  registers, queue quantum, upper DMA addresses, and AXI system-bus settings.
  Keep fixed FIFO configuration and use the H755 AHB DMA settings. Do not set the
  generic MAC port-select bit. Skip gigabit PHY registers for MII/RMII so the
  LAN8742 cannot be misidentified as a gigabit link. Add memory barriers before
  DMA tail writes and after observing RX ownership returned to the CPU.
- `CM7/Core/Src/stm32h7xx_hal_msp.c`: remove HAL Ethernet MSP initialization and
  deinitialization; the board port now owns Ethernet pins and clocks.
- `CM7/Core/Src/stm32h7xx_it.c`: remove the HAL ETH handle and IRQ dispatch.
- Both CM7 linker scripts: explicitly place `.eqos_nocache` in RAM_D1 as NOLOAD,
  aligned to 32 bytes. The driver initializes all descriptor words before use;
  packet buffers remain in RAM_D1 `.bss`. The section name alone does not change
  memory cacheability: this bring-up depends on D-cache remaining off.
- `CM7/.cproject`: add `baremetal` to Debug and Release source entries, excluding
  `example_main.c` to avoid a second main(). Vendor HAL sources remain available;
  unused HAL ETH code is removed from the final ELF by linker garbage collection.
- `CM7/Debug` generated make metadata: add the baremetal objects and correct stale
  eth_3 source/linker paths to eth_4 for the command-line build. CubeIDE regenerates
  these files; the lasting source selection lives in `.cproject`.
- `CM7/baremetal/README.md`: identify the active STM32 port and this change log.

## Ethernet GPIO mapping

Same signal mapping as the original MSP, all AF11, push-pull, no internal pulls,
very-high GPIO speed:

| Pin | RMII signal |
| --- | --- |
| PA1 | REF_CLK, external 50 MHz from PHY |
| PA2 | MDIO |
| PA7 | CRS_DV |
| PC1 | MDC |
| PC4 | RXD0 |
| PC5 | RXD1 |
| PB13 | TXD1 |
| PG11 | TX_EN |
| PG13 | TXD0 |

No dedicated PHY reset GPIO was present in this project. PHY reset uses MDIO.
RMII REF_CLK remains 50 MHz for both 10 and 100 Mbps. The driver scans PHY
addresses, resets the PHY, and waits up to approximately 5 seconds for link
negotiation. Connect the cable before resetting the board.

## Application behavior

1. Initialize the existing clock, UART (115200, 8N1), and button.
2. Initialize Ethernet in RMII, promiscuous mode, with MAC loopback disabled.
3. Send one test frame, destination `6C:1F:F7:CA:2B:FD`, source
   `00:80:E1:00:00:00`. Payload remains `Hello world hehe lmao`, including the
   terminating zero byte as before. EtherType changes from 0x0800 to experimental
   0x88B5 because the text payload is not an IPv4 packet. MAC adds padding/FCS.
4. Poll RX, print each frame in hex, then release its descriptor to DMA.
5. Press USER to send again. Button bounce may request another frame.

`eqos_send` waits for DMA completion with a bounded timeout. The printed TX
success indicates local DMA completion, not confirmation from the receiving PC.
RX errors are dropped/rearmed by the driver. Successful RX must be released
exactly once with `eqos_recv_done`. No Ethernet IRQ, RTOS, LwIP, IP, or ping is used.

## Validation performed

- Modified C sources compiled with GCC 13.3.rel1, Cortex-M7 hard-float, -Wall and
  -Wextra: no warnings or errors.
- Forced full Debug build using STM32CubeIDE bundled GNU make (`make -C CM7/Debug
  -B -j4 all`): passed, no warnings or errors. Build log:
  `CM7/Debug/baremetal-build.log`.
- Output: `CM7/Debug/eth_3_CM7.elf` (existing project/artifact name retained).
  Size: text 27,592 bytes; data 124 bytes; BSS/NOLOAD 21,708 bytes.
- ELF symbol inspection: eqos_init/send/recv/recv_done present; no HAL_ETH_* symbols.
- ELF DMA locations, all 32-byte aligned in RAM_D1:
  TX buffers 0x24000240; RX buffers 0x24001B40;
  TX descriptors 0x24004EA0; RX descriptors 0x24004EE0.
  `.eqos_nocache` is 192 bytes (4 TX + 8 RX descriptors, 16 bytes each).
- `.cproject` parsed successfully as XML; both configurations include the driver
  and exclude the example entry point. Release and RAM-link builds were not run.
- Register compatibility checked against the bundled STM32H755 CMSIS header and
  HAL source. Reference: ST RM0399, Ethernet chapter. Local reference manual:
  `D:/STM32/rm0399-stm32h745755-and-stm32h747757.pdf`.

## Board debugging / remaining validation

Hardware has not been flashed or exercised by this change. On-board validation:

1. Refresh/rebuild the CM7 project in CubeIDE. Ensure it resolves to this eth_4
   folder; the project is still named eth_3_CM7. Flash the rebuilt CM7 ELF.
2. Connect Ethernet to a live PC/switch before reset. Open COM1 at 115200 8N1.
   Expect PHY discovery, negotiated link, ready message, and `TX: complete (0)`.
3. Capture on the destination PC with Wireshark filter `eth.type == 0x88b5`.
   Change `dest_mac` in main.c if the PC NIC has a different address. Press USER
   several times to exercise TX ring wraparound.
4. Send raw frames from the peer to the board MAC, or send broadcast traffic.
   Expect UART RX dumps. Receive more than eight frames at a low rate to verify
   ring recycling. A transmitted frame does not automatically return to RX;
   use peer traffic for this test. This firmware does not answer ping or ARP.
5. Break at eqos_board_init, eqos_init, eqos_send, eqos_recv, eqos_recv_done.
   Watch `eth.stats`, `eth.tx_head`, `eth.rx_head`, the descriptor arrays, and
   ETH DMACSR/DMACTDTPR/DMACRDTPR. eqos_dump_regs is available for DMA diagnostics.
6. If DMA software reset times out, check PA1's 50 MHz clock, RMII selection,
   and Ethernet RCC enables. If PHY discovery fails, inspect MDIO/MDC. If link
   negotiation times out, check cable/peer and reset again with the cable attached.

Limitations kept deliberately simple: initialization errors print then enter
Error_Handler; automatic cable unplug/replug recovery is not implemented. UART
hex dumps hold the RX buffer and are slow, so sustained traffic can overflow the
ring. Keep D-cache disabled and do not move DMA buffers into DTCM. CubeMX code
regeneration can restore the old HAL Ethernet initialization; preserve/reapply
this integration if regenerating from the unchanged .ioc configuration.
