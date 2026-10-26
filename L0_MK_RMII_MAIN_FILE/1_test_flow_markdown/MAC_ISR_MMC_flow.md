# MAC_Interrupt_Status MMCTXIS / MMCRXIS / MMCIS — MMC interrupt: test code flow

Scope: preset the MMC counters near half-scale, send frames until one TX and one RX
counter cross 0x8000_0000, check the MMC bits set, read the counters, check they clear.
Polling only (CPU interrupt off). Internal MAC loopback.

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| MAC_Interrupt_Status.MMCIS [8] | Set when MMCRXIS, MMCTXIS or MMCRXIPIS is set. Clears when all of them are 0. |
| MAC_Interrupt_Status.MMCRXIS [9] | Set when any unmasked bit in MMC_Rx_Interrupt is set. Clears when MMC_Rx_Interrupt is all 0. |
| MAC_Interrupt_Status.MMCTXIS [10] | Set when any unmasked bit in MMC_Tx_Interrupt is set. Clears when MMC_Tx_Interrupt is all 0. |
| MMC_Tx_Interrupt.TXGPKTIS [21] | Tx_Packet_Count_Good reached half (0x8000_0000) or full (0xFFFF_FFFF). Cleared by reading that counter. |
| MMC_Rx_Interrupt.RXUCGPIS [17] | Rx_Unicast_Packets_Good reached half or full. Cleared by reading that counter. |
| MMC_Control.CNTPRST [4] / CNTPRSTLVL [5] | Preset all counters: CNTPRSTLVL=0 → 0x7FFF_FFF0 (almost half), 1 → 0xFFFF_FFF0 (almost full). CNTPRST self-clears. |

## Setup
1. MMC_Control: RSTONRD=0, CNTFREEZ=0, CNTSTOPRO=0.
2. MMC_Tx_Interrupt_Mask / MMC_Rx_Interrupt_Mask: mask all, then unmask only TXGPKTIM / RXUCGPIM.
   Write only the implemented mask bits (others are reserved, read 0):
   In STM32H7, only these bits available, dont touches the reserved bits.
   RX all = 0x0C020060 (RXLPITRCIM, RXLPIUSCIM, RXUCGPIM, RXALGNERPIM, RXCRCERPIM),
   TX all = 0x0C20C000 (TXLPITRCIM, TXLPIUSCIM, TXGPKTIM, TXMCOLGPIM, TXSCOLGPIM).
3. Read Tx_Packet_Count_Good and Rx_Unicast_Packets_Good (clear old status) → MMCIS=MMCTXIS=MMCRXIS=0.
4. MMC_Control.CNTPRST=1, CNTPRSTLVL=0; wait until CNTPRST reads 0.

## Flow
1. Send 20 unicast frames (DA=MAC0), receive each one → both counters pass 0x8000_0000 (16 frames needed).
2. Poll MAC_Interrupt_Status (timeout 100 ms) → MMCTXIS=1, MMCRXIS=1, MMCIS=1.
   Print MAC_Interrupt_Status, MMC_Tx_Interrupt (TXGPKTIS=1), MMC_Rx_Interrupt (RXUCGPIS=1).
3. Read Tx_Packet_Count_Good (expect ≥ 0x8000_0000) → MMCTXIS=0, MMCIS still 1 (RX pending).
4. Read Rx_Unicast_Packets_Good (expect ≥ 0x8000_0000) → MMCRXIS=0, MMCIS=0.
5. `RESULT: PASS/FAIL` per step 2–4, then `N/3 PASS`.

## Open points
- Bit positions and counter names: confirm against the databook / chip on the first run.
- Reading a counter clears its interrupt bit; read MMC_Tx/Rx_Interrupt before the counters.
