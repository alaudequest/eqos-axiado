/* SPDX-License-Identifier: GPL-2.0 */
/*
 * eqos_flow.c - than cac ham log FLOW (khai bao o eqos_flow.h).
 * Cac ham duoi day CHI DOC thanh ghi / descriptor de in ra, khong ghi gi vao phan cung.
 * Vi tri bit trong cac bang da doi chieu voi RM0399 Rev 4 (chuong 61).
 */
#include "eqos_flow.h"
#include "../core/eqos.h"
#include "../core/eqos_hw.h"
// HAL_GetTick (flow_step), HAL_RCC_Get*Freq (flow_chip_info)
#include "stm32h7xx_hal.h"

/* In ten thanh ghi, dia chi tuyet doi (base + offset) va gia tri DOC LAI tu phan cung.
 * Doc lai sau khi ghi giup thay phan cung that su giu gia tri gi
 * (vi du bit tu xoa nhu SWR trong ETH_DMAMR se doc ra 0). */
void flow_reg(const char *name, uintptr_t base, uint32_t offset)
{
  // Khi FLOW_LOG = 0, ca lenh duoi bien mat (macro bo qua tham so) nen khong doc thanh ghi
  FLOW("      %-26s @0x%08lX = 0x%08lX\r\n", name,
       (unsigned long)(base + offset),
       (unsigned long)AX_BASE_READ_REG32(base, offset));
  // Tranh canh bao "unused parameter" khi FLOW_LOG = 0
  (void)name; (void)base; (void)offset;
}

#if FLOW_LOG >= 2
// Bang truong bit cua ETH_DMACSR (ten giong RM0399), dung trong flow_dma_state()
static const flow_field_t F_DMACSR[]    = {{"TI",0,0},{"TPS",1,1},{"TBU",2,2},{"RI",6,6},{"RBU",7,7},{"RPS",8,8},
                                           {"RWT",9,9},{"ETI",10,10},{"ERI",11,11},{"FBE",12,12},{"CDE",13,13},
                                           {"AIS",14,14},{"NIS",15,15},{"TEB",18,16},{"REB",21,19}};

// Ten trang thai may DMA (ETH_DMADSR.TPS0 / RPS0, RM0399 tr.3078)
static const char *const flow_tps_name[8] = {
  "Stopped", "Running: lay TX descriptor", "Running: cho status", "Running: doc RAM -> FIFO TX",
  "Ghi timestamp", "(reserved)", "SUSPENDED: het descriptor hoac underflow", "Running: dong TX descriptor"};
static const char *const flow_rps_name[8] = {
  "Stopped", "Running: lay RX descriptor", "(reserved)", "Running: cho frame",
  "SUSPENDED: het RX descriptor", "Running: dong RX descriptor", "Ghi timestamp", "Running: chuyen frame -> RAM"};
// Ten trang thai FIFO MTL (ETH_MTLTXQDR / ETH_MTLRXQDR, RM0399 tr.3104, tr.3109)
static const char *const flow_trcsts_name[4] = {"Idle", "Read (dang dua du lieu cho MAC TX)", "Cho TX status tu MAC", "Flushing"};
static const char *const flow_rxqsts_name[4] = {"rong", "duoi nguong", "tren nguong", "DAY"};
static const char *const flow_rrcsts_name[4] = {"Idle", "doc du lieu", "doc status", "Flushing"};
// Ten trang thai bo phat MAC (ETH_MACDR.TFCSTS, RM0399 tr.3142)
static const char *const flow_tfcsts_name[4] = {"Idle", "cho status/IPG/backoff", "phat Pause", "dang phat frame"};

/* In gia tri tung truong bit cua v, vi du "-> ST=1 OSF=1 TSE=0 TXPBL=8" */
void flow_fields(const flow_field_t *f, uint32_t n, uint32_t v)
{
  printf("        ->");
  for (uint32_t i = 0; i < n; ++i) {
    // Do rong truong = hi - lo + 1; tao mat na cung do rong roi dich ve bit 0
    uint32_t w = (uint32_t)f[i].hi - f[i].lo + 1U;
    uint32_t mask = (w >= 32U) ? 0xFFFFFFFFU : ((1UL << w) - 1U);
    printf(" %s=%lu", f[i].name, (unsigned long)((v >> f[i].lo) & mask));
  }
  printf("\r\n");
}

/* Doc thanh ghi, in hex (flow_reg) roi in them dong giai ma tung bit */
void flow_reg_f(const char *name, uintptr_t base, uint32_t offset,
                       const flow_field_t *f, uint32_t n)
{
  flow_reg(name, base, offset);
  flow_fields(f, n, AX_BASE_READ_REG32(base, offset));
}

/* Tieu de mot buoc + thoi gian tu luc khoi dong */
void flow_step(const char *tag, const char *title)
{
  printf("\r\n================ %s %s   (t = %lu ms) ================\r\n",
         tag, title, (unsigned long)HAL_GetTick());
}

/* Doi so chu ky CPU (DWT) sang "x.yy us" (CPU 224 MHz -> 1 chu ky ~ 4.46 ns) */
void flow_print_cycles(uint32_t cyc)
{
  uint32_t per_us = SystemCoreClock / 1000000U;
  uint32_t us_x100 = (uint32_t)(((uint64_t)cyc * 100U) / per_us);
  printf("%lu chu ky = %lu.%02lu us", (unsigned long)cyc,
         (unsigned long)(us_x100 / 100U), (unsigned long)(us_x100 % 100U));
}

/* In "tx_desc[i]" neu dia chi nam trong ring, nguoc lai in "(ngoai ring)" */
static void flow_desc_name(const char *ring, uint32_t addr, uint32_t base, uint32_t n)
{
  if (addr >= base && addr < base + n * 16U && ((addr - base) % 16U) == 0U)
    printf("%s[%lu]", ring, (unsigned long)((addr - base) / 16U));
  else
    printf("(ngoai ring)");
}

/* Trang thai DMA: may trang thai TX/RX, descriptor DMA dang xu ly, co trang thai DMACSR */
void flow_dma_state(void)
{
  // ETH_DMADSR (offset 0x00C): TPS0 bit 15:12, RPS0 bit 11:8 (bit cao nhat luon = 0)
  uint32_t dsr = AX_BASE_READ_REG32(EQOS_DMA_BASE, 0x00CU);
  uint32_t tps = (dsr >> 12) & 0x7U, rps = (dsr >> 8) & 0x7U;
  printf("      DMA TX: TPS0=%lu %-38s | DMA RX: RPS0=%lu %s\r\n",
         (unsigned long)tps, flow_tps_name[tps], (unsigned long)rps, flow_rps_name[rps]);
  // ETH_DMACCATXDR (0x144) / ETH_DMACCARXDR (0x14C): descriptor ma DMA dang tro toi
  uint32_t ctx = AX_BASE_READ_REG32(EQOS_DMA_BASE, 0x144U);
  uint32_t crx = AX_BASE_READ_REG32(EQOS_DMA_BASE, 0x14CU);
  printf("      DMA dang o: TX 0x%08lX = ", (unsigned long)ctx);
  flow_desc_name("tx_desc", ctx, (uint32_t)(uintptr_t)tx_desc, EQOS_TX_DESC_COUNT);
  printf(" | RX 0x%08lX = ", (unsigned long)crx);
  flow_desc_name("rx_desc", crx, (uint32_t)(uintptr_t)rx_desc, EQOS_RX_DESC_COUNT);
  printf("\r\n");
  flow_reg_f("ETH_DMACSR (co trang thai)", EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS, F_DMACSR, FLOW_N(F_DMACSR));
}

/* Trang thai 2 FIFO cua MTL */
void flow_mtl_state(void)
{
  uint32_t tq = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_TXQ0_DEBUG);
  uint32_t rq = AX_BASE_READ_REG32(EQOS_MTL_BASE, EQOS_MTL_RXQ0_DEBUG);
  printf("      MTL TX FIFO: TXQSTS=%lu (%s), TRCSTS=%lu %s\r\n",
         (unsigned long)((tq >> 4) & 1U), ((tq >> 4) & 1U) ? "con du lieu" : "rong",
         (unsigned long)((tq >> 1) & 3U), flow_trcsts_name[(tq >> 1) & 3U]);
  printf("      MTL RX FIFO: PRXQ=%lu goi dang cho, RXQSTS=%lu %s, RRCSTS=%lu %s\r\n",
         (unsigned long)((rq >> 16) & 0x3FFFU), (unsigned long)((rq >> 4) & 3U), flow_rxqsts_name[(rq >> 4) & 3U],
         (unsigned long)((rq >> 1) & 3U), flow_rrcsts_name[(rq >> 1) & 3U]);
}

/* Trang thai bo phat/thu cua MAC (ETH_MACDR, offset 0x114) */
void flow_mac_state(void)
{
  uint32_t d = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_DEBUG);
  printf("      MAC: TX engine %s, TFCSTS=%lu %s | RX engine %s\r\n",
         ((d >> 16) & 1U) ? "DANG PHAT" : "idle", (unsigned long)((d >> 17) & 3U),
         flow_tfcsts_name[(d >> 17) & 3U], (d & 1U) ? "DANG THU" : "idle");
}

/* Bo dem MMC (chi doc; reset-on-read dang tat nen doc khong lam mat so dem) */
void flow_mmc(void)
{
  printf("      MMC: TX good=%lu  TX 1-collision=%lu  TX nhieu-collision=%lu | RX unicast good=%lu  RX loi CRC=%lu  RX loi align=%lu\r\n",
         (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, 0x768U), (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, 0x74CU),
         (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, 0x750U), (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, 0x7C4U),
         (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, 0x794U), (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, 0x798U));
}

/* In ca 2 ring: 4 word moi descriptor, ai dang giu (OWN), vi tri head/tail/DMA */
void flow_rings(const char *why)
{
  uint32_t ttail = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR);
  uint32_t rtail = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_TAIL_PTR);
  uint32_t tcur = AX_BASE_READ_REG32(EQOS_DMA_BASE, 0x144U);
  uint32_t rcur = AX_BASE_READ_REG32(EQOS_DMA_BASE, 0x14CU);
  printf("      --- Ring descriptor (%s) ---        DES0     DES1     DES2     DES3\r\n", why);
  for (uint32_t i = 0; i < EQOS_TX_DESC_COUNT; ++i) {
    uint32_t a = (uint32_t)(uintptr_t)&tx_desc[i];
    printf("      tx_desc[%lu] @0x%08lX:  %08lX %08lX %08lX %08lX  %s%s%s%s\r\n", (unsigned long)i, (unsigned long)a,
           (unsigned long)tx_desc[i].des0, (unsigned long)tx_desc[i].des1, (unsigned long)tx_desc[i].des2,
           (unsigned long)tx_desc[i].des3, (tx_desc[i].des3 & EQOS_TDES3_OWN) ? "[DMA giu]" : "[CPU giu]",
           (i == eth.tx_head) ? " <- tx_head" : "", (a == ttail) ? " <- TX TAIL" : "", (a == tcur) ? " <- DMA dang o" : "");
  }
  for (uint32_t i = 0; i < EQOS_RX_DESC_COUNT; ++i) {
    uint32_t a = (uint32_t)(uintptr_t)&rx_desc[i];
    printf("      rx_desc[%lu] @0x%08lX:  %08lX %08lX %08lX %08lX  %s%s%s%s\r\n", (unsigned long)i, (unsigned long)a,
           (unsigned long)rx_desc[i].des0, (unsigned long)rx_desc[i].des1, (unsigned long)rx_desc[i].des2,
           (unsigned long)rx_desc[i].des3, (rx_desc[i].des3 & EQOS_RDES3_OWN) ? "[DMA giu]" : "[CPU giu]",
           (i == eth.rx_head) ? " <- rx_head" : "", (a == rtail) ? " <- RX TAIL" : "", (a == rcur) ? " <- DMA dang o" : "");
  }
}

/* Hex dump co chu thich tung truong cua frame Ethernet */
void flow_frame_dump(const char *title, const uint8_t *p, uint32_t len, uint32_t payload_len)
{
  // Ranh gioi cac truong: dich (6) | nguon (6) | EtherType (2) | payload | phan con lai = pad
  const uint32_t cut[6] = {0U, 6U, 12U, 14U, 14U + payload_len, len};
  const char *const name[5] = {"Dest MAC", "Src MAC", "EtherType", "Payload", "Pad (MAC them)"};
  printf("      %s: %lu byte @%p\r\n", title, (unsigned long)len, (const void *)p);
  for (uint32_t s = 0; s < 5U; ++s) {
    uint32_t a = cut[s], b = (cut[s + 1] < len) ? cut[s + 1] : len;
    if (a >= b)
      continue;
    printf("        [%2lu..%2lu] %-14s:", (unsigned long)a, (unsigned long)(b - 1U), name[s]);
    for (uint32_t i = a; i < b; ++i) {
      // Truong dai (pad) xuong dong moi 16 byte cho de doc
      if (i > a && ((i - a) % 16U) == 0U)
        printf("\r\n                                 ");
      printf(" %02X", (unsigned int)p[i]);
    }
    printf("\r\n");
  }
}

/* Thong tin chip va clock (goi sau khi UART da khoi tao) */
void flow_chip_info(void)
{
  uint32_t id = DBGMCU->IDCODE, cpu = SCB->CPUID;
  printf("      Chip: DBGMCU_IDCODE=0x%08lX -> DEV_ID=0x%03lX (0x450 = STM32H74x/75x), REV_ID=0x%04lX\r\n",
         (unsigned long)id, (unsigned long)(id & 0xFFFU), (unsigned long)(id >> 16));
  printf("      Loi : SCB->CPUID=0x%08lX -> Cortex-M7 r%lup%lu\r\n",
         (unsigned long)cpu, (unsigned long)((cpu >> 20) & 0xFU), (unsigned long)(cpu & 0xFU));
  printf("      Clock: SYSCLK=%lu HCLK=%lu PCLK1=%lu PCLK2=%lu Hz\r\n",
         (unsigned long)HAL_RCC_GetSysClockFreq(), (unsigned long)HAL_RCC_GetHCLKFreq(),
         (unsigned long)HAL_RCC_GetPCLK1Freq(), (unsigned long)HAL_RCC_GetPCLK2Freq());
  printf("      Cache: I-cache=%lu D-cache=%lu (SCB->CCR bit 17/16)\r\n",
         (unsigned long)((SCB->CCR >> 17) & 1U), (unsigned long)((SCB->CCR >> 16) & 1U));
}
#endif /* FLOW_LOG >= 2 */
