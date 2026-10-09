# Combine DMA control configuration writes - 2026-09-25

Updated step 6 in CM7/Core/Src/main.c to read TX, RX and channel control once
each, combine their fields in local variables, then write each register once.
TX combines OSP and TXPBL; RX combines RBSZ and RXPBL. Channel control retains
DSL=0 and PBLX8. Unrelated bits are preserved and ST/SR explicitly remain clear.

Step 8 reuses the prepared TX/RX values with ST/SR set after descriptor setup.
This is valid for the current initialization sequence: DMA is stopped and no
other code modifies these control registers between steps 6 and 8.
Do not reuse these snapshots after another writer changes their configuration.

Compared with the previous code, this removes four register reads (two during
configuration and two during start) and two configuration writes. The separate
start writes remain necessary to enable DMA only after the rings are ready.
SYSBUS and interrupt-enable writes remain unchanged.

These were read-modify-write operations, not polling. Polling reads status
repeatedly until a condition changes; reset/MDIO busy and descriptor OWN polling
are still required. EQOS_BASE_ADDR is the common base; different offsets select
different registers. Full constant writes without initial reads would require
intentionally specifying all writable fields instead of preserving other bits.

Validation: CM7 Debug build/link passed without warnings or errors.
Log: CM7/Debug/dma-combined-config-build.log.
Hardware testing was not performed.
