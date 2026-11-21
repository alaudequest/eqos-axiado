# MAC_Interrupt_Status TXSTSIS / RXSTSIS — test code flow

Scope: one oversized frame sets MAC_Rx_Tx_Status.TJT and RWT; check TXSTSIS [13] and
RXSTSIS [14] set, then clear when MAC_Rx_Tx_Status is read. Polling only
(MAC_Interrupt_Enable.TXSTSIE/RXSTSIE=1, CPU interrupt off). Internal MAC loopback.

## Status bits (EQOS databook)

| Register.bit | Meaning |
|---|---|
| MAC_Interrupt_Status.TXSTSIS [13] | Transmit status interrupt. Set when any TX error bit in MAC_Rx_Tx_Status is set: EXCOL, LCOL, EXDEF, LCARR, NCARR (half duplex only) or TJT. Cleared by reading MAC_Rx_Tx_Status. |
| MAC_Interrupt_Status.RXSTSIS [14] | Receive status interrupt. Set when MAC_Rx_Tx_Status.RWT is set. Cleared by reading MAC_Rx_Tx_Status. |
| MAC_Rx_Tx_Status.TJT [0] | Transmit Jabber Timeout. The transmit jabber timer expired: the frame was longer than 2048 bytes (10240 with Jumbo, JE=1) and MAC_Configuration.JD=0. |
| MAC_Rx_Tx_Status.RWT [8] | Receive Watchdog Timeout. A frame longer than 2048 bytes (10240 with JE=1, or the MAC_Watchdog_Timeout.WTO limit when PWE=1) was received and MAC_Configuration.WD=0. |

Read-to-clear assumes MAC_CSR_SW_Ctrl.RCWE=0 (default); with RCWE=1 the bits clear by writing 1.

## Setup
MAC_Configuration: LM=1, JD=0, WD=0, JE=0, S2KP=0. MAC_Watchdog_Timeout.PWE=0 (2048 B limit).

## Flow
1. Read MAC_Rx_Tx_Status (clear old status) → TXSTSIS=RXSTSIS=0.
2. Send one 2100 B frame (FCS excluded).
3. Poll MAC_Interrupt_Status (timeout 100 ms) → TXSTSIS=1, RXSTSIS=1. Print both registers.
4. Read MAC_Rx_Tx_Status → TJT=1, RWT=1; then TXSTSIS=RXSTSIS=0.
5. `RESULT: PASS/FAIL` per bit, then `N/2 PASS`.

## Open point
- If loopback cuts the frame so only one bit sets, log MAC_Rx_Tx_Status and split into two frames.
