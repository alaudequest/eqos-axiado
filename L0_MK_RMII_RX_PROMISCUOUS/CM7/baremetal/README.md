# STM32H755 DWC EQoS polling module

This folder contains the active Ethernet driver. Core/Src/main.c handles
HAL/clock/UART/button setup and compile-time test dispatch.

## Files

- eqos.c: LAN8742/MDIO, MAC/MTL/DMA configuration, descriptor rings, TX/RX,
  promiscuous control and diagnostics.
- eqos_port.c: STM32 GPIO/RMII/clocks/reset, DWT delay and register-map checks.
- eqos.h: configuration, software statistics and public API.
- eqos_hw.h: register offsets and bit definitions.
- ../0_test_case/L0_MK_RMII_RX_PROMISCUOUS.c: Promiscuous_disable test.
- ../Core/Inc/eth_test_case_config.h: compile-time test selection.

The other register notes in this folder are reference material; the API below
describes the current STM32 implementation.

## Select and build

TEST_RMII_RX_PROMISCUOUS_DISABLE defaults to 1. Set it to 0 in
eth_test_case_config.h, or define it as 0 in the compiler settings, to build
the board-only idle application. Add future test branches alongside the
existing conditional branches in main.c, and guard each test translation unit
with its selector. Include test headers, never .c files.

Both Debug and Release source entries in .cproject include 0_test_case.
Refresh the CM7 project and rebuild in STM32CubeIDE so its generated makefiles
include the new test folder. baremetal/example_main.c remains excluded.
The existing project/output name still uses L0_MK_RMII_END2END_PC_TX_CM7.

## Run Promiscuous_disable

1. Attach the passive RJ45 TX-pair-to-RX-pair loopback before resetting.
   PHY auto-negotiation must complete; the external simultaneous TX/RX test
   requires full duplex. Internal MAC loopback remains disabled.
2. Open the board UART at 115200 baud. The test runs automatically once.
3. The test sends a 60-byte Ethernet-II frame, EtherType 0x88B5, to the board
   address 00:80:E1:00:00:00 and requires its return.
4. It sends to foreign unicast 6C:1F:F7:CA:2B:FD and requires no matching
   receive during ETH_TEST_RX_TIMEOUT_MS (default 500 ms).
5. It sends another own-address frame and requires its return before PASS.
   Each frame carries a run number and step, so old/unrelated packets cannot
   satisfy these checks. Matching packets are compared byte-for-byte.
6. Press B1 to repeat. Several repeats exercise the TX/RX ring wraparound.
   A request during a run schedules one subsequent run.

The passive loopback does not swap MAC addresses. Both accepted frames have
the board MAC as DA and SA; the rejection frame changes DA only (plus its
test step). Hardware inserts FCS, and the MAC strips received FCS.
PASS reports an observation within the configured receive window, not proof
against all possible physical faults. TX completion alone is never PASS.
On a TX timeout, reset the board before retrying.

Without the loopback (e.g. a connected PC), the automatic loopback check
normally fails its first own-address receive timeout. Afterwards the idle
test continues dumping accepted PC traffic; target the board MAC.
No Python packet generator is required by the loopback test.

## Driver API

    int eqos_init(struct eqos_dev *dev, const struct eqos_cfg *cfg);
    int eqos_send(struct eqos_dev *dev, const void *frame, uint32_t length);
    int eqos_receive(struct eqos_dev *dev, void *buffer,
                     uint32_t capacity, uint32_t *length);
    int eqos_set_promiscuous(struct eqos_dev *dev, bool enabled);
    void eqos_dump_regs(const struct eqos_dev *dev);

- One active STM32 Ethernet device; keep dev alive. Call initialization once.
  All driver calls run in main context with SysTick active.
- Only RMII/LAN8742 automatic discovery and the STM32 fixed base are supported.
  Legacy generic configuration fields are retained, but unsupported values
  are rejected (see eqos.h). A zero reset/link timeout selects the default.
- D-cache must be disabled. Descriptors reside in .eqos_nocache, buffers
  in .bss, both in DMA-accessible RAM_D1 under the supplied linker scripts.
- Initialization configures DMA while stopped, prepares and publishes the
  rings, then starts MAC/DMA. Driver failures return an error to the caller.
- eqos_send copies data into driver storage and polls completion for up to
  approximately one second. It excludes generated padding/FCS from length.
  A timeout leaves DMA ownership intact and blocks subsequent sends.
- eqos_receive is nonblocking: EQOS_OK means a packet was copied;
  EQOS_EAGAIN means no packet. Errors and oversized packets are recycled.
  The caller owns the copy and does not call a release function.
- Initialization with promiscuous=false selects own unicast + broadcast.
  Source-address, hash, inverse and receive-all filtering are disabled.
  eqos_set_promiscuous explicitly sets or clears PR while preserving other
  filter bits. It is intended for use between test runs.
- Diagnostics include MAC filter/address registers, DMA status, descriptor
  status, MMC counters and software statistics, even when no RX was delivered.
