# MAC control basic loopback cases

Select one case using `ACTIVE_TEST` in `CM7/Core/Inc/eth_config_test_case.h`
(or a compiler `-DACTIVE_TEST=...` override), rebuild CM7, flash, and reset.
Press USER once to execute. Reset before rerunning, especially after a timeout.
The existing default test selection is preserved.

| Case file / selection | ETH_MACCR (offset 0x0000) | Basic check |
| --- | --- | --- |
| `MAC_CTRL_ARPEN.c` / `TEST_MAC_CTRL_ARPEN` (17) | ARPEN bit 31 | Enable ARP offload; ordinary frame roundtrip |
| `MAC_CTRL_SARC.c` / `TEST_MAC_CTRL_SARC` (18) | SARC bits 30:28 = 011 | Replace deliberately different TX source address with MAC0 |
| `MAC_CTRL_IPC.c` / `TEST_MAC_CTRL_IPC` (19) | IPC bit 27 | Enable receive checksum offload; ordinary frame roundtrip |
| `MAC_CTRL_GPSLCE.c` / `TEST_MAC_CTRL_GPSLCE` (20) | GPSLCE bit 23 | Set MACECR.GPSL to 1518; accept a shorter frame |

Each case sets its feature before starting MAC/DMA, enables internal MAC
loopback, verifies register readback, and sends a 128-byte Ethernet frame
(EtherType 0x88B5). Other requested feature fields are cleared for isolation.
The STM32 header calls bit 31 `ETH_MACCR_ARP`.

UART prints the complete TX, expected RX, and actual RX frame in hex, plus
descriptor status and a named PASS/FAIL result. Checks cover TX errors, RX
FD/LD/error status, exact length, and every received byte. MAC adds FCS on TX;
CST strips it on RX, so the compared frames exclude FCS. SARC intentionally
changes bytes 6..11; its expected RX uses the configured MAC0 address.
An RX timeout reports FAIL without dumping a DMA-owned buffer.

These are basic feature-enabled data-path tests. ARP request/reply offload,
IP checksum validation/error injection, giant-frame boundary rejection, and
the other SARC encodings require additional functional cases. A smoke-test
PASS does not establish those behaviors. The case files are included from
main through the existing queue selector; do not compile them separately.
