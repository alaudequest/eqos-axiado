# Ethernet loopback RX fix — 2026-09-21

Changed source: `CM7/Core/Src/main.c`.

This is a human-readable change record with selected before/after excerpts, not a complete patch exported from the editor. The project was not a Git repository when these changes were made.

## Problem

`ETH_DumpRxPacket()` dereferenced `p->buffer`, but the receive callbacks never initialized that pointer. This is a likely cause of the reported HardFault; no hardware fault-register capture was available to confirm the exact faulting instruction.

The allocation was also only 100 bytes, with packet storage starting partway into it, while `heth.Init.RxBuffLen` permitted DMA to write 1536 bytes. Received lengths were hardcoded to 100, and RX reading depended on the TX-complete interrupt.

## `Changes`

### 1. Give each RX node real packet storage

Before:

```c
typedef struct {
    ETH_BufferTypeDef *AppBuff;
    uint8_t buffer[100] __ALIGNED(32);
} ETH_AppBuff;
```

After:

```c
#define ETH_APP_RX_BUFFER_SIZE 1536U
#define ETH_APP_RX_POOL_SIZE (2U * ETH_RX_DESC_CNT)

typedef struct {
    ETH_BufferTypeDef AppBuff;
    uint8_t in_use;
    uint8_t buffer[ETH_APP_RX_BUFFER_SIZE] __ALIGNED(32);
} ETH_AppBuff;

static ETH_AppBuff rx_pool[ETH_APP_RX_POOL_SIZE];
```

The metadata is embedded in each slot. The pool replaces `malloc(100)`. The current linker places it in RAM_D1, and the current project leaves D-cache disabled. Alignment alone is not cache coherency support.

### 2. Initialize the buffer pointer and preserve the actual received length

The allocation callback finds a free slot, marks it in use, initializes its node, and returns its byte-array address. If no slot is available, it returns NULL.

The link callback recovers the containing slot and initializes the node:

```c
ETH_AppBuff *app =
    (ETH_AppBuff *)(buff - offsetof(ETH_AppBuff, buffer));
ETH_BufferTypeDef *p = &app->AppBuff;
p->buffer = buff;
p->len = Length;
p->next = NULL;
```

Previously, `p->buffer` was unset and `p->len` was always 100. The callback appends the initialized node to the packet list.

### 3. Move processing out of the interrupt

Previously, the TX-complete callback called `HAL_ETH_ReadData()` and printed; the RX-complete callback dumped a shared packet pointer.

Now the callbacks only set flags:

```c
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth)
{
    UNUSED(heth);
    tx_complete = 1U;
}

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth)
{
    UNUSED(heth);
    rx_pending = 1U;
}
```

The main loop clears the RX flag before draining completed packets with `HAL_ETH_ReadData()`. For each packet, it dumps, compares, and releases the buffers. TX completion triggers `HAL_ETH_ReleaseTxPacket()` in the main loop.

Receive flow:

```text
Allocate storage -> DMA receives -> RX interrupt sets flag
-> main calls HAL_ETH_ReadData -> HAL links received buffers
-> dump -> compare -> release pool slots
```

### 4. Correct the TX test configuration

Added after constructing the frame:

```c
TxConfig.Length = TxBuffer.len;
TxConfig.pData = &frame;
TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CRCPAD;
```

This supplies the missing TX length, gives HAL a packet identity for release bookkeeping, and disables checksum insertion for the raw test payload. MAC CRC/padding insertion remains enabled. The transmit return status is checked and the submitted bytes are printed.

### 5. Add comparison and RX validation

- `ETH_DumpRxPacket()` rejects a NULL data pointer or a length above buffer capacity before reading bytes.
- `ETH_CompareRxPacket()` compares submitted TX bytes against the beginning of RX, across its linked buffers.
- Output reports MATCH or FAIL, lengths, mismatch count, and the first differing byte when present.
- Extra RX bytes are reported separately; trailing padding/FCS are not validated.
- `ETH_FreeRxPacket()` returns each processed slot to the pool.

## Validation

CM7 Debug compiled and linked successfully using the installed STM32CubeIDE GNU toolchain. The linker map placed `rx_pool` at `0x240002A0` in RAM_D1. No board flashing or hardware loopback validation was performed.

## How to save the next change yourself

### A written changelog

1. Open the project's `Change log` folder.
2. Create a file named `YYYY-MM-DD-short-description.md`.
3. Record the problem, changed files/functions, reason for the change, and verification performed.
4. Paste important before/after code snippets if useful.

Template:

```markdown
# Short description — YYYY-MM-DD

Changed files:

## Problem

## Changes and reasons

## Validation

## Remaining work
```

### An exact code diff without Git

Before editing, save a copy of the source. In PowerShell, from the project root:

```powershell
New-Item -ItemType Directory -Force -Path '.\Change log'
Copy-Item -LiteralPath '.\CM7\Core\Src\main.c' -Destination '.\Change log\main.before.c'
```

Use a unique backup name for each change so you do not overwrite an older baseline. After editing, if Git is installed, it can compare two files even outside a repository:

```powershell
git diff --no-index --output="Change log/main-change.patch" -- "Change log/main.before.c" "CM7/Core/Src/main.c"
```

Exit code 1 means differences were found; this is expected. A patch records exact line additions and removals. A Markdown changelog explains why they were made. A copy taken after editing cannot recover the earlier version.

Keep backup `.c` files outside IDE source/build folders, or exclude them from compilation if the IDE automatically discovers them.
