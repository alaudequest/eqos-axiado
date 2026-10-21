# Execute the Ethernet sequence directly in main() - 2026-09-24

## Request and resulting flow

The previous change only moved function definitions into main.c. This change
moves the active Ethernet statements into main() itself, so stepping through
initialization, TX and RX no longer enters separate EQoS driver functions.

Numbered blocks in main():

1. Existing HAL/system clock, UART and button initialization.
2. DWT delay clock, Ethernet AF11 pin configuration, RMII selection, RCC reset.
3. MAC/DMA software reset with timeout and MDIO clock-divider selection.
4. LAN8742_RegisterBusIO, LAN8742_Init, LAN8742_StartAutoNego and bounded link wait.
5. Negotiated MAC speed/duplex, MTL configuration, filter and MAC address.
6. H755 AHB DMA configuration with interrupts disabled.
7. Direct initialization of TX/RX descriptors, ring bases, lengths and tails.
8. Enable DMA/MAC, construct the original test frame and request startup TX.
9. Inside the loop: copy TX data, publish ownership, update tail, wait and report.
10. Inside the loop: poll RX ownership, validate/print data, rearm and update tail.

## Changed files

- CM7/Core/Src/main.c: replace EQoS driver functions and application wrappers
  with the sequence above. Direct CMSIS ETH-> register accesses make registers
  visible while stepping. Descriptor storage is now typed tx_desc/rx_desc arrays
  of four volatile words; contiguous 16-byte stride and 32-byte ring alignment
  are checked at compile time. Buffers are tx_buf/rx_buf; eth still exposes the
  head indexes, link mode and packet statistics for the debugger.
- LAN8742 IO callbacks remain callable functions as required by the original
  lan8742.c. Their MDIO read/write code now accesses registers directly, with
  a shared busy-wait helper using a 1-second HAL tick timeout and argument checks.
- Removed unused generic PHY, stop, link-polling, MAC-address setter and register
  dump functions. The application never called stop/link-polling. TX errors now
  print DMACSR, descriptor status and TX tail directly instead of a full dump.
- CM7/baremetal/eqos_port.c: retain only DWT microsecond delay and CMSIS register
  mapping assertions; GPIO/RCC setup is now inside main().
- CM7/baremetal/eqos.h: retain configuration/state types, constants and delay;
  remove obsolete EQoS API declarations and the unused PHY init callback field.
- CM7/baremetal/eqos.c: still an empty build-compatible file; update its comment.
- Both CM7 linker scripts: update the descriptor initialization comment only.
- CM7/baremetal/README.md: document the current flow and historical example.

The system clock function, button callback, error handler, LAN8742 callbacks,
and microsecond delay remain outside main. The vendor lan8742.c/.h are unchanged.
The prior main.c is saved as 2026-09-24-main-before-inline.txt in this directory
for comparison/restoration; its .txt extension prevents compilation.

## Preserved behavior and deliberate limits

Same Ethernet pins, MAC addresses, test payload/EtherType, promiscuous mode,
blocking TX, polled RX, startup send and USER-button send. D-cache must remain
disabled. Startup send and button sends now share the inline TX block.
Descriptor OWN barriers, busy-TX protection, RX context/error/fragment/length
checks, and exactly-once recycling are retained. RX tail convention is unchanged.
MDIO clock values outside the supported 20-300 MHz table now fail explicitly.
This is a dedicated H755 flow, not a reusable multi-platform EQoS driver API.
The excluded example_main.c targets the removed API and is historical only.

## Validation

- CM7 Debug build/link passed without warnings or errors.
  Log: CM7/Debug/inline-main-build.log. ELF: CM7/Debug/eth_4_CM7.elf.
- Additional main.c/eqos_port.c compilation checks with -Wall -Wextra -Werror
  passed. Ring size, alignment configuration and descriptor stride checks passed.
- ELF inspection confirmed no HAL_ETH_* or former EQoS driver entry points;
  eqos_udelay is the only remaining eqos_* function. Original LAN8742 functions
  remain linked. All DMA rings and buffers remain aligned in RAM_D1.
- ELF size: text 25,260 bytes; data 144 bytes; BSS/NOLOAD 21,748 bytes.
- No board execution was performed. Check startup/link, repeated button TX
  beyond four frames, and low-rate RX beyond eight frames to exercise ring wrap.
  UART dumps still limit RX throughput. Automatic cable recovery is not added.
