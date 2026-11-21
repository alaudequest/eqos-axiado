# EQoS baremetal

## Framework style: src/ + includes/ in Core/Src (2026-10-21, latest)

The code follows the Kirkwood validation framework layout:
`../Core/Src/src/L0_MK_RMII_MAC_LOOPBACK.c` holds `eth_run_test_ops` and
`do_test()`, and `../Core/Src/includes/` holds the library (`eth_functions.c`,
`eth_function_stm32.c`, `eth_filter_functions.c`, `eth_dump_functions.c`,
`eth_printf.c`, `eth_registers.h`, `mac_defines.h`, `eth_traffic_test.h`,
`eth_utils.h`). Only `eth_function_stm32.c` (and `main.c`) depend on STM32;
the EQoS code uses `AX_BASE_READ/WRITE_REG32` only. See
`../../Change log/2026-10-21-framework-style.md` and
`../../docs/MAC_LOOPBACK_framework.html`. The per-block layout below is
archived in `../../Change log/2026-10-21-before-framework-style/`.

Test modes (`eth_run_test_ops` in `L0_MK_RMII_MAC_LOOPBACK.c`, one
`includes/eth_*_functions.c` each; per-mode pages in `../../docs/`):

| Mode | Test | File | Doc page |
|---|---|---|---|
| 0 | traffic, N frames TX = RX | `eth_functions.c` | `MAC_LOOPBACK_test_plan.html` |
| 1 | MAC address filter, 24-case table (board 2026-10-16: 24/24) | `eth_filter_functions.c` | `MAC_LOOPBACK_filter.html` |
| 2 | DMA burst FB / AAL | `eth_burst_functions.c` | `MAC_LOOPBACK_burst.html` |
| 3 | PTP timestamp, one-step | `eth_ptp_functions.c` | `MAC_LOOPBACK_ptp.html` |
| 4, 5 | VLAN insertion, QinQ | `eth_vlan_functions.c` | `MAC_LOOPBACK_vlan_tag.html` |
| 6 | TSO | `eth_tso_functions.c` | `MAC_LOOPBACK_tso.html` |
| 7 | checksum offload | `eth_coe_functions.c` | `MAC_LOOPBACK_coe.html` |
| 8, 9 | VLAN filter, VLAN strip | `eth_vlan_rx_functions.c` | `MAC_LOOPBACK_vlan_rx.html` |
| 10 | SA insertion / replacement, CRC-pad | `eth_sa_functions.c` | `MAC_LOOPBACK_sa_cpc.html` |
| 11 | MAC address filter with PR = 0, 18 cases, framework style | `eth_addr_functions.c` | `MAC_LOOPBACK_addr_filter.html` |
| 12 | interrupt status with nothing enabled: DMACSR / DMAISR / MTLQICSR / MACISR, IOC, RBU, overflow, RWT, FBE | `eth_istat_functions.c` | `MAC_LOOPBACK_istat.html` |

## One folder per block in Core/Src (2026-10-15, replaced 2026-10-21)

The active EQoS code no longer lives in this folder. It is split into one
folder per functional block under `../Core/Src`, in the order of RM0399 61.9.
`main()` in `../Core/Src/main.c` reads top to bottom as the bring-up sequence,
and the `CFG` table above it holds every configuration value, grouped per
block. The `[n]` numbers are the step tags printed by the FLOW log. `Core` is
already a source folder of the CubeIDE project, so its subfolders build
without project changes. Headers are included with relative paths, for
example `"../core/eqos.h"`.

| Folder under `../Core/Src` | Files | Contents | Step |
|---|---|---|---|
| (root) | `main.c` | `CFG` table, `main()`, board setup, USER button, `Error_Handler` | [1] [2] |
| `core/` | `eqos.h`, `eqos_hw.h`, `eqos.c` | shared types and API, register offsets and bits, state `eth`, software reset (SWR) and MAC1USTCR | [4] |
| `port/` | `eqos_port.h`, `eqos_port.c` | STM32 glue: D-cache check, DWT, RMII pins, SYSCFG, RCC, NVIC, `eqos_udelay` | [2] [3] |
| `dma/` | `eqos_dma.c` | DMA channel 0 configuration, DMA start (ST/SR) | [5] [8] |
| `desc/` | `eqos_desc.c` | descriptor rings and buffers, `eqos_send`, `eqos_recv`, `eqos_recv_done` | [6] [10] [11] |
| `mtl/` | `eqos_mtl.c` | MTL TX/RX queue 0 | [7] |
| `mac/` | `eqos_mac.c` | packet filter, MAC address, MACCR, MAC start (TE/RE) | [8] |
| `flow/` | `eqos_flow.h`, `eqos_flow.c` | FLOW log: `FLOW_LOG` level, register and descriptor decoders | all |
| `test/` | `test_loopback.h`, `test_loopback.c` | test frame, one TX, RX poll and compare | [9] [10] [11] |

This folder keeps only this README, `register_index.md`, `docs/`, and the
excluded historical `example_main.c`.

Behavior changes made the same day, before the split: DMA ST/SR are set only
after the rings, MTL and MAC are configured (RM0399 61.9.1 step 9), MAC loopback
runs at 100 Mbit/s full duplex (RM0399 61.5.8), and FTQ is no longer set during
MTL initialization. See `../../Change log/2026-10-15-split-eqos-by-block.md`.

## Inline main sequence (2026-09-24)

Ethernet accesses use `AX_BASE_READ_REG32` / `AX_BASE_WRITE_REG32` with the
matching absolute `EQOS_MAC_BASE`, `EQOS_MTL_BASE`, or `EQOS_DMA_BASE` and a
block-relative register offset from `eqos_hw.h`, including in LAN8742 MDIO
callbacks. Configuration steps read each control once, combine fields locally,
then write once. Status polling remains live. CMSIS remains for
GPIO/RCC/core peripherals and compile-time validation of the EQoS register map.

The active Ethernet flow now runs directly in `../Core/Src/main.c`, inside
`main()` as numbered steps 1-10: board setup, reset, LAN8742 initialization,
MAC/MTL/DMA configuration, descriptor initialization, TX and RX recycling.
There are no `eqos_init/send/recv/setup_rings` functions in the active firmware.
`eqos_port.c` supplies only the microsecond delay and register-map assertions.
`eqos.h` retains shared types/constants and the delay declaration; the old
driver API declarations have been removed. `example_main.c` is an excluded
historical example for the old API, not a buildable alternative entry point.
See `../../Change log/2026-09-24-inline-ethernet-sequence-in-main.md`.
The sections below describe earlier layouts and generic porting history.

## Single-file debugging (2026-09-24)

The full EQoS implementation, including init, TX/RX, MDIO, descriptor storage
and ring setup, now lives in `../Core/Src/main.c` under
`USER CODE BEGIN EQOS_Implementation`. `eqos.c` is an empty compatibility file.
`eqos.h`, `eqos_hw.h`, and `eqos_port.c` retain their API, register definitions,
and board-specific functions. See the change log
`../../Change log/2026-09-24-eqos-implementation-in-main.md`.

## STM32H755 integration (2026-09-23)

This project now uses `Core/Src/main.c` with this driver. `eqos_port.c` is the
STM32H755 RMII port, not the original multi-board template. D-cache must remain
disabled; the port rejects initialization if it is enabled. DMA storage is in
RAM_D1. `example_main.c` is documentation only and excluded from both IDE builds.
See `../../Change log/2026-09-23-cm7-baremetal-ethernet.md` for the pin map,
STM32-specific driver changes, build results, and hardware debugging procedure.
The generic porting notes below describe the original driver.

Driver Synopsys DesignWare Ethernet QoS viết lại từ U-Boot `dwc_eth_qos.c`,
bỏ toàn bộ device model / clk framework / malloc.

## File

| File | Vai trò | Có phải sửa? |
|---|---|---|
| `eqos_hw.h` | Offset + bit của thanh ghi | Không |
| `eqos.h` | API công khai + tuỳ chọn biên dịch | Chỉnh macro cấu hình |
| `eqos.c` | Toàn bộ logic driver | Không |
| `eqos_port.c` | Clock, reset, pinmux, glue của SoC | **Có — chỉ file này** |
| `example_main.c` | Ví dụ gửi ARP | Tham khảo |

## Đối chiếu với driver U-Boot

| U-Boot | Baremetal |
|---|---|
| `eqos_probe()` + `eqos_start()` | `eqos_init()` |
| `eqos_stop()` | `eqos_stop()` |
| `eqos_send()` | `eqos_send()` |
| `eqos_recv()` | `eqos_recv()` |
| `eqos_free_pkt()` | `eqos_recv_done()` |
| `eqos_adjust_link()` | `eqos_adjust_link()` (static) |
| `struct eqos_ops` (15 con trỏ hàm) | 5 hàm `eqos_board_*` / `eqos_dcache_*` |
| `struct eqos_config` | macro biên dịch + `struct eqos_cfg` |
| `phy_connect()/phy_startup()` của U-Boot | PHY clause-22 tự viết trong `eqos.c` |
| `memalign()` | mảng `static` căn theo cache line |
| `dev_read_*()` (device tree) | trường trong `struct eqos_cfg` |

## Bộ nhớ DMA — chọn 1 trong 2

### A. Descriptor ở vùng không cache (mặc định, khuyên dùng)

`EQOS_DESC_CACHED = 0`. Không cần flush/invalidate descriptor, `DSL = 0`,
descriptor xếp sát nhau 16 byte. Đây là lý do bỏ được toàn bộ logic
`desc_per_cacheline` phức tạp nhất của driver gốc.

Yêu cầu: section `.eqos_nocache` phải được map là **Normal Non-Cacheable**
(đừng dùng Device-nGnRnE — truy cập không căn lề sẽ fault).

Thêm vào linker script:

```ld
. = ALIGN(4096);
.eqos_nocache (NOLOAD) : {
    __eqos_nocache_start = .;
    *(.eqos_nocache)
    . = ALIGN(4096);
    __eqos_nocache_end = .;
} > RAM
```

Rồi trong code khởi tạo MMU, map `[__eqos_nocache_start, __eqos_nocache_end)`
với thuộc tính Normal-NC.

### B. Descriptor ở vùng có cache

`EQOS_DESC_CACHED = 1`. Driver giãn mỗi descriptor ra đúng một cache line và
gọi `eqos_dcache_clean/invalidate`. **Chỉ dùng được khi**
`(EQOS_CACHELINE - 16) / EQOS_AXI_WIDTH <= 7` (trường DSL của HW chỉ rộng 3 bit).

| cacheline | AXI width | DSL cần | Dùng được? |
|---|---|---|---|
| 64 | 8 (64-bit) | 6 | ✅ |
| 64 | 16 (128-bit) | 3 | ✅ |
| 64 | 4 (32-bit) | 12 | ❌ → dùng cách A |

Buffer dữ liệu (1600 byte) luôn nằm ở vùng cache thường và được xử lý qua
`EQOS_BUF_CACHED`.

## Cần biết trước khi chạy

Ba con số bạn phải tra từ TRM của SoC, sai là không chạy:

1. **`cfg.base`** — địa chỉ base khối EQoS.
2. **`cfg.csr_clk_hz`** — tần số clock CSR/AHB thật sự cấp cho MAC.
   Dùng để chọn CR của MDIO và nạp `MAC_US_TIC_COUNTER`. Sai → MDIO
   đọc ra 0xffff hoặc treo.
3. **`EQOS_AXI_WIDTH`** — độ rộng bus AXI (4/8/16 byte). Sai → DSL sai →
   DMA đọc nhầm descriptor.

## Trình tự bring-up

Làm theo thứ tự này, dừng lại ở bước nào lỗi thì sửa bước đó:

1. **Đọc `MAC_VERSION`** (offset 0x110). Ra `0x00000000` hoặc `0xffffffff`
   → clock chưa bật hoặc chưa thoát reset. Sửa `eqos_board_init()`.
2. **`DMA_MODE.SWR` tự clear.** Không clear → clock chưa chạy đủ, hoặc SoC
   cần quirk (`eqos_board_fix_soc_reset`).
3. **MDIO đọc được PHY ID.** `eqos_mdio_read(dev, addr, 2)` ra khác
   0x0000/0xffff. Không được → sai `csr_clk_hz`, sai địa chỉ PHY, hoặc PHY
   còn đang bị giữ reset.
4. **PHY lên link.** Không lên → sai pinmux, sai chế độ interface trong
   glue register, hoặc chưa cấp 25MHz/50MHz clock cho PHY.
5. **TX chạy.** Gửi ARP, bắt bằng Wireshark ở đầu kia. Không thấy gì →
   kiểm tra `MTL_TXQ0_DEBUG`, `DMA_CH0_STATUS`, và TX delay của RGMII.
6. **RX chạy.** Không nhận được → kiểm tra `MTL_RXQ0_DEBUG.PRXQ`:
   - PRXQ > 0 nhưng `eqos_recv()` trả EAGAIN → vấn đề descriptor/cache.
   - PRXQ = 0 → frame không vào tới MAC, vấn đề ở PHY/RGMII RX delay.

`eqos_dump_regs()` in sẵn tất cả thanh ghi cần cho các bước trên.

## Khác biệt có chủ ý so với driver U-Boot

| Thay đổi | Lý do |
|---|---|
| `eqos_recv()` kiểm tra bit `ES`, `FD`, `LD` | Driver gốc bỏ qua → frame lỗi vẫn đi lên |
| Mỗi TX descriptor có buffer riêng | Gốc dùng chung 1 buffer cho cả ring |
| RX ring mặc định 8 thay vì 4 | Bớt drop khi burst |
| Promiscuous mặc định **tắt** | Gốc luôn bật; ở đây thành tuỳ chọn |
| Vòng chờ trong `eqos_stop()` có `udelay` | Gốc busy-loop 1e6 lần không delay → thời gian không xác định |
| Có `struct eqos_stats` | Đếm lỗi để debug |
| Descriptor ring không cache (mặc định) | Bỏ được logic gom nhóm cache line |

## Chưa hỗ trợ

Có chủ ý, để giữ code đơn giản — thêm sau nếu cần:

- Interrupt (driver hoàn toàn polling)
- Multi-queue / QoS thật (CBS, TAS, 802.1Qav/Qbv) — chỉ dùng queue 0
- PTP / timestamp
- Checksum offload
- MDIO clause 45 (bản gốc có; ở đây bỏ vì PHY gigabit thường dùng C22)
- Jumbo frame
