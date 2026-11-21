# MAC_Interrupt_Status LPIIS / PMTIS / PHYIS — test code flow

Scope: for each bit, cause the event, check the bit sets, read the source register,
check the bit clears. Polling only (MAC_Interrupt_Enable bit set, CPU interrupt off).
Other bits: MAC_ISR_RXTX_STS_flow.md, MAC_ISR_TSIS_flow.md, MAC_ISR_MMC_flow.md.

## Flow per bit
1. Read the source register (clear old status) → bit = 0.
2. Trigger the event.
3. Poll MAC_Interrupt_Status (timeout 100 ms) → bit = 1. Print it + source register.
4. Read the source register → bit = 0.
5. `RESULT: PASS/FAIL` per bit, then `N/M PASS`.

## Cases

| # | Bit | Trigger | Source register (read clears) |
|---|---|---|---|
| 1 | LPIIS [5] | MAC_LPI_Control_Status.LPIEN=1, TX idle → TX LPI entry | MAC_LPI_Control_Status.TLPIEN — log only (RMII/PHY EEE unconfirmed) |
| 2 | PMTIS [4] | MAC_PMT_Control_Status.MGKPKTEN=1, PWRDWN=1; PC sends a magic packet — manual | MAC_PMT_Control_Status.MGKPRCVD |
| — | PHYIS [3] | Read only, expect 0 (PHY interrupt input not wired) | — |

## Open point
- Bit positions: confirm against the databook / chip on the first run.
