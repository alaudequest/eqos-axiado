/* SPDX-License-Identifier: GPL-2.0 */
/*
 * eqos.c - trang thai dung chung cua driver (eth) va [4] reset toan bo IP EQoS.
 * Cac khoi khac: dma/eqos_dma.c [5][8], desc/eqos_desc.c [6][10][11],
 * mtl/eqos_mtl.c [7], mac/eqos_mac.c [8]. Phan rieng cua STM32: port/eqos_port.c.
 * Thu tu goi: main().
 */
#include "eqos.h"
#include "eqos_hw.h"
#include "../flow/eqos_flow.h"
// HAL_RCC_GetHCLKFreq (clock CSR cho ETH_MAC1USTCR)
#include "stm32h7xx_hal.h"
#include <stdio.h>

// "So ghi chep" phia phan mem: tx_head/rx_head, thong ke, co started. Nam o .bss nen tu = 0.
// (eth.cfg, eth.speed, eth.duplex_full khong dung: cau hinh nam trong bang CFG cua main.c)
struct eqos_dev eth;

#if FLOW_LOG >= 2
// Bang truong bit ETH_DMAMR (ten giong RM0399), dung de giai ma trong log
static const flow_field_t F_DMAMR[]     = {{"SWR",0,0},{"DA",1,1},{"TXPR",11,11},{"PR",14,12},{"INTM",17,16}};
#endif

/* [4] Doc version, software reset toan bo IP, nap tick 1 us (RM0399 61.9.1 buoc 1-2, tr.3031).
 * Tra ve EQOS_ETIMEDOUT neu SWR khong tu xoa (main() se goi Error_Handler()). */
int eqos_reset(void)
{
  // [FLOW2]
  FLOW_STEP("[4]", "Doc version, software reset DMA/MTL/MAC, tick 1 us");
  //Check MAC version
  // Doc ETH_MACVR (0x40028110) va in dang 8 chu so hex; RM ghi gia tri reset 0x00003142.
  // Ra 0 hoac FFFFFFFF -> clock ETH chua bat hoac chua thoat reset
  printf("MAC version: %08lX\r\n", (unsigned long)AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_VERSION));

  /* MAC/DMA software reset.
   * SWR requests a reset of the EQoS DMA/controller state and self-clears when
   * complete. The loop must reread the volatile register; Do not program rings/MAC settings until this wait succeeds. */
  // Doc ETH_DMAMR vao bien tam
  uint32_t dma_mode = AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_MODE);
  // Dat bit 0 (SWR) trong bien
  dma_mode |= EQOS_DMA_MODE_SWR;
  // [FLOW] In TRUOC khi ghi, de thoi gian in UART khong cong vao thoi gian cho SWR
  FLOW("[4] Ghi ETH_DMAMR.SWR = 1 (software reset DMA/MTL/MAC), cho phan cung tu xoa...\r\n");
  // [FLOW2] DMAMR truoc khi reset (SWR = 0)
  flow_reg2("ETH_DMAMR (truoc SWR)", EQOS_DMA_BASE, EQOS_DMA_MODE, F_DMAMR, FLOW_N(F_DMAMR));
#if FLOW_LOG >= 2
  // [FLOW2] Moc thoi gian (chu ky CPU) ngay truoc khi ghi SWR, de do thoi gian reset
  uint32_t flow_t_swr = DWT->CYCCNT;
#endif
  // Ghi lai -> phan cung bat dau reset mem
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_MODE, dma_mode);
  // Bo dem so lan cho (~1 us moi lan)
  uint32_t elapsed = 0;
  // Moi vong doc lai thanh ghi (macro co volatile) den khi SWR = 0
  while ((AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_MODE) & EQOS_DMA_MODE_SWR) != 0U) {
    // So sanh gia tri cu roi moi tang; qua 100000 lan (~100 ms) -> loi
    if (elapsed++ >= 100000U) {
      // Nguyen nhan hay gap nhat: khong co REF_CLK 50 MHz tu PHY o PA1
      printf("DMA reset timeout: check PA1 50 MHz reference clock\r\n");
      // main() se goi Error_Handler()
      return EQOS_ETIMEDOUT;
    }
    // Cho 1 us giua hai lan doc
    eqos_udelay(1);
  }
  // [FLOW]
  FLOW("[4] SWR tu xoa sau ~%lu us -> reset xong\r\n", (unsigned long)elapsed);
#if FLOW_LOG >= 2
  // [FLOW2] Thoi gian reset do chinh xac bang bo dem chu ky DWT
  printf("[4] SWR do bang DWT: ");
  flow_print_cycles(DWT->CYCCNT - flow_t_swr);
  printf("\r\n");
#endif
  // [FLOW2] DMAMR sau reset: SWR phai = 0
  flow_reg2("ETH_DMAMR (sau SWR)", EQOS_DMA_BASE, EQOS_DMA_MODE, F_DMAMR, FLOW_N(F_DMAMR));

  /* Program clocks-per-microsecond minus one for the MAC's internal tick.
   * csr_clk is the MAC register/CSR clock (= HCLK) used for MDC division and the
   * microsecond tick register; it is NOT the external 50 MHz RMII reference.
   * With this project's HCLK=224 MHz the value is 223. This is unrelated to
   * the IEEE 1588 timestamp increment and does not enable PTP timestamping. */
  // Hoi HAL tan so HCLK hien tai (224 MHz)
  uint32_t csr_clk_hz = HAL_RCC_GetHCLKFreq();
  // Ghi ETH_MAC1USTCR = 224000000 / 1000000 - 1 = 223 (so chu ky HCLK trong 1 us, tru 1)
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, EQOS_MAC_US_TIC_COUNTER,
      csr_clk_hz / 1000000U - 1U);
  // [FLOW] Mong doi 0x000000DF (= 223)
  flow_reg("ETH_MAC1USTCR (223)", EQOS_MAC_BASE, EQOS_MAC_US_TIC_COUNTER);
  return EQOS_OK;
}
