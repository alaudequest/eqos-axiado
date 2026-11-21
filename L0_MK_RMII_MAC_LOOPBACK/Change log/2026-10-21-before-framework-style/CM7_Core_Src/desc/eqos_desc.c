/* SPDX-License-Identifier: GPL-2.0 */
/*
 * eqos_desc.c - descriptor va buffer (RM0399 61.10): [6] tao 2 ring va bao cho DMA,
 * [10] eqos_send() gui 1 frame, [11] eqos_recv() / eqos_recv_done() nhan 1 frame.
 * struct eqos_desc (4 word, 16 byte) duoc dinh nghia o core/eqos.h.
 */
#include "../core/eqos.h"
#include "../core/eqos_hw.h"
#include "../flow/eqos_flow.h"
#include <stdio.h>
#include <string.h>

/* Rings: 4 TX and 8 RX entries by current eqos.h defaults. The linker places
 * .eqos_nocache in DMA-accessible RAM_D1, with NOLOAD: initialize every word
 * explicitly in eqos_setup_rings(). D-cache is disabled globally (checked in stm32_precheck()).
 * Buffers are also in RAM_D1 (.bss).
 * 32-byte alignment is the M7 cache-line alignment;
 * Descriptors remain packed at a 16-byte stride inside each ring. DSL (Descriptor skip length) must therefore remain zero.
 * DSl: extra space the DMA skips after a descriptor before reading the next one. Zero means descriptors are continuos.
 */
// Mang 4 descriptor TX (64 byte): can 32 byte, dat vao section .eqos_nocache (linker -> RAM_D1 0x2400xxxx)
struct eqos_desc tx_desc[EQOS_TX_DESC_COUNT]
  __attribute__((aligned(32), section(".eqos_nocache")));
// Mang 8 descriptor RX (128 byte), cung thuoc tinh
struct eqos_desc rx_desc[EQOS_RX_DESC_COUNT]
  __attribute__((aligned(32), section(".eqos_nocache")));
// 4 buffer TX x 1600 byte: moi descriptor TX co buffer rieng (.bss, RAM_D1)
static uint8_t tx_buf[EQOS_TX_DESC_COUNT][EQOS_BUF_SIZE] __attribute__((aligned(32)));
// 8 buffer RX x 1600 byte: DMA ghi frame nhan duoc vao day
static uint8_t rx_buf[EQOS_RX_DESC_COUNT][EQOS_BUF_SIZE] __attribute__((aligned(32)));

/* These checks protect assumptions in the DMA configuration and wrap logic:
 * index = (index + 1) & (count - 1) requires a nonzero power-of-two ring count.
 * RBSZ describes each RX buffer's size.
 * A 1600-byte buffer covers an ordinary 1518-byte frame / 1522-byte VLAN frame.
 * Jumbo/multi-descriptor packet reassembly is not implemented here. */
// _Static_assert kiem tra luc BIEN DICH (sai -> loi build): descriptor phai dung 16 byte
_Static_assert(sizeof(struct eqos_desc) == 16, "DMA descriptor stride must be 16 bytes");
// So descriptor TX > 0 va la luy thua 2 (x & (x-1) == 0 chi dung khi x la luy thua 2)
_Static_assert(EQOS_TX_DESC_COUNT > 0 &&
  (EQOS_TX_DESC_COUNT & (EQOS_TX_DESC_COUNT - 1)) == 0, "TX ring must be power of two");
// Tuong tu cho RX
_Static_assert(EQOS_RX_DESC_COUNT > 0 &&
  (EQOS_RX_DESC_COUNT & (EQOS_RX_DESC_COUNT - 1)) == 0, "RX ring must be power of two");
// Buffer chia het 32 va <= 0x3FFF (truong RBSZ rong 14 bit)
_Static_assert(EQOS_BUF_SIZE % 32 == 0 && EQOS_BUF_SIZE <= 0x3FFF,
  "RX buffer must be aligned and fit RBSZ");
// Khong phai H755, hoac cau hinh descriptor/buffer nam vung co cache -> dung bien dich
#if !EQOS_STM32H7 || EQOS_DESC_CACHED || EQOS_BUF_CACHED
#error "This inline sequence requires STM32H755 with uncached DMA storage"
#endif

#if FLOW_LOG >= 2
// Bang truong bit TDES3/RDES3 (ten giong RM0399), dung de giai ma trong log
static const flow_field_t F_TDES3_RD[]  = {{"FL",14,0},{"CIC",17,16},{"CPC",27,26},{"LD",28,28},{"FD",29,29},{"CTXT",30,30},{"OWN",31,31}};
static const flow_field_t F_TDES3_WB[]  = {{"IHE",0,0},{"DB",1,1},{"UF",2,2},{"ED",3,3},{"CC",7,4},{"EC",8,8},{"LC",9,9},
                                           {"NC",10,10},{"LoC",11,11},{"PCE",12,12},{"FF",13,13},{"JT",14,14},{"ES",15,15},
                                           {"TTSS",17,17},{"LD",28,28},{"FD",29,29},{"CTXT",30,30},{"OWN",31,31}};
static const flow_field_t F_RDES3_RD[]  = {{"BUF1V",24,24},{"BUF2V",25,25},{"IOC",30,30},{"OWN",31,31}};
static const flow_field_t F_RDES3_WB[]  = {{"PL",14,0},{"ES",15,15},{"LT",18,16},{"DE",19,19},{"RE",20,20},{"OE",21,21},
                                           {"RWT",22,22},{"GP",23,23},{"CE",24,24},{"RS0V",25,25},{"RS1V",26,26},
                                           {"RS2V",27,27},{"LD",28,28},{"FD",29,29},{"CTXT",30,30},{"OWN",31,31}};
#endif

/* [6] Initialize TX/RX descriptors and register both rings with DMA
 * (RM0399 61.9.1 buoc 4-6, tr.3031). */
void eqos_setup_rings(void)
{
  // [FLOW2]
  FLOW_STEP("[6]", "Khoi tao descriptor, dang ky ring voi DMA");
  /* TX initially has no work: OWN=0 in every descriptor. RX starts pre-posted:
   * each descriptor point to address of their empty buffer with BUF1V and OWN.
   * CPU writes address/unused words first, executes DSB, then set OWN bit.
   * The CPU must not modify an RX buffer after giving OWN away.
   * DMA is still stopped here (ST/SR = 0): RM0399 allows writing DLAR only while
   * the DMA is stopped (tr.3083/3084); eqos_start_dma() runs after [8].  */
  // Lap qua 4 descriptor TX: xoa ca 4 word -> OWN = 0 (CPU giu).
  // Bat buoc vi section NOLOAD khong duoc startup xoa tu dong
  for (uint32_t i = 0; i < EQOS_TX_DESC_COUNT; ++i) {
    tx_desc[i].des0 = 0;
    tx_desc[i].des1 = 0;
    tx_desc[i].des2 = 0;
    tx_desc[i].des3 = 0;
  }
  // Lap qua 8 descriptor RX: "nap san" buffer trong cho DMA
  for (uint32_t i = 0; i < EQOS_RX_DESC_COUNT; ++i) {
    // DES0 = dia chi buffer (ep con tro -> uintptr_t -> so 32-bit)
    rx_desc[i].des0 = (uint32_t)(uintptr_t)rx_buf[i];
    // DES1, DES2 = 0: khong dung buffer thu hai
    rx_desc[i].des1 = 0;
    rx_desc[i].des2 = 0;
    // Cho 3 lenh ghi tren vao RAM truoc khi trao quyen
    __DSB();
    // Trao cho DMA: OWN (bit 31) | BUF1V (bit 24) = 0x81000000
    rx_desc[i].des3 = EQOS_RDES3_OWN | EQOS_RDES3_BUF1V;
  }
  // Chac chan moi descriptor da nam trong RAM truoc khi bao dia chi cho DMA
  __DSB();
  // [FLOW] Dia chi phai nam trong AXI SRAM 0x2400xxxx (DMA ETH truy cap duoc)
  FLOW("[6] tx_desc @%p  rx_desc @%p  (16 byte/descriptor)\r\n", (void *)tx_desc, (void *)rx_desc);
  FLOW("[6] tx_buf  @%p  rx_buf  @%p  (1600 byte/buffer)\r\n", (void *)tx_buf, (void *)rx_buf);
  FLOW("[6] rx_desc[0]: des0=0x%08lX (= rx_buf[0])  des3=0x%08lX (OWN|BUF1V)\r\n",
       (unsigned long)rx_desc[0].des0, (unsigned long)rx_desc[0].des3);
  // [FLOW2] Giai ma RDES3 dang read format
  flow_fields(F_RDES3_RD, FLOW_N(F_RDES3_RD), rx_desc[0].des3);

  /* Tell DMA where each descriptor array starts in RAM: &tx_desc[0] and
   * &rx_desc[0]. Write the full RAM address; do not subtract the EQoS base.
   * Write descriptor count minus 1 into each ring-length register:
   * 4 TX descriptors -> write 3; 8 RX descriptors -> write 7.
   * DMA moves to the next descriptor by adding 16 bytes plus any DSL gap.
   * Here DSL=0, so descriptors are 16 bytes apart. DMA wraps to the start
   * after the last descriptor. tx_head/rx_head are only for our C code;
   * DMA tracks its own position.
   * The tail tells DMA where the available work ends. Updating it can
   * restart DMA after it has stopped there. It refers to descriptors,
   * not packet data. Set OWN=1 to give a descriptor to DMA, and keep the
   * tail address and ring size consistent with the descriptors in use. */
  // ETH_DMACTXDLAR = dia chi tx_desc[0] (dia chi RAM day du)
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_LIST_ADDR,
      (uint32_t)(uintptr_t)&tx_desc[0]); //Tx List Address
  // ETH_DMACTXRLR = 4 - 1 = 3
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_RING_LEN,
      EQOS_TX_DESC_COUNT - 1U); //Tx Ring length
  // ETH_DMACRXDLAR = dia chi rx_desc[0]
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_LIST_ADDR,
      (uint32_t)(uintptr_t)&rx_desc[0]); //Rx List Address
  // ETH_DMACRXRLR = 8 - 1 = 7
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_RING_LEN,
      EQOS_RX_DESC_COUNT - 1U);  //Rx Ring length
  // ETH_DMACTXDTPR (tail TX) = &tx_desc[0]: tail trung dau ring -> chua co gi de gui
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR,
      (uint32_t)(uintptr_t)&tx_desc[0]); //Tx Tail Pointer

  /* Setting tail Rx Desc tail pointer: initial tail=&rx_desc[N-1];
   * recycling later writes the returned descriptor's address.
   * On a stall, inspect OWN, current RX descriptor and tail together;
   * exercise more than N packets to verify wraparound on the actual hardware. */
  // ETH_DMACRXDTPR (tail RX) = &rx_desc[7] (descriptor cuoi)
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_TAIL_PTR,
      (uint32_t)(uintptr_t)&rx_desc[EQOS_RX_DESC_COUNT - 1U]);//Rx Tail Pointer
  // Con tro phia phan mem: slot TX tiep theo = 0, slot RX se kiem tra dau tien = 0
  eth.tx_head = 0;
  eth.rx_head = 0;
  // [FLOW] Doc lai cac thanh ghi ring; DMADSR/DMACSR cho thay trang thai DMA luc nay
  FLOW("[6] Dang ky ring voi DMA:\r\n");
  flow_reg("ETH_DMACTXDLAR (&tx_desc[0])", EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_LIST_ADDR);
  flow_reg("ETH_DMACTXRLR (3)", EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_RING_LEN);
  flow_reg("ETH_DMACRXDLAR (&rx_desc[0])", EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_LIST_ADDR);
  flow_reg("ETH_DMACRXRLR (7)", EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_RING_LEN);
  flow_reg("ETH_DMACTXDTPR (TX tail)", EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR);
  flow_reg("ETH_DMACRXDTPR (RX tail)", EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_TAIL_PTR);
  flow_reg("ETH_DMADSR (TPS/RPS)", EQOS_DMA_BASE, 0x00CU);
  flow_reg("ETH_DMACSR (status)", EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS);
  // [FLOW2] Trang thai DMA sau khi co ring, va toan bo 2 ring
  flow_dma_state();
  flow_rings("sau khi khoi tao");
}

/* [10] TX: copy packet frame to tx_buf, publish descriptor, increase tail
 * pointer, wait for completion.
 * Tra ve EQOS_OK, EQOS_EINVAL (do dai sai), EQOS_EAGAIN (descriptor van thuoc DMA),
 * EQOS_ETIMEDOUT (DMA khong tra OWN sau ~1 s) hoac EQOS_EIO (DMA bao loi, ES = 1). */
int eqos_send(const uint8_t *data, uint32_t len)
{
  // Slot TX se dung
  uint32_t idx = eth.tx_head;
  /* tx_head stored the next descriptor to send in Tx process (for software-side easier monitor).
   * The DMA has its own current-descriptor pointer. After a timeout the old slot may still be
   * owned by DMA; the OWN check below prevents unsafe reuse on ring wrap. */
  // Con tro toi descriptor do
  struct eqos_desc *tx = &tx_desc[idx];
  // Ket qua mac dinh = thanh cong
  int tx_status = EQOS_OK;
  // Do dai khong hop le -> EINVAL (review #6)
  if (len == 0U || len > EQOS_BUF_SIZE) {
    tx_status = EQOS_EINVAL;
    // [FLOW]
    FLOW("[10] frame_len = %lu khong hop le -> EINVAL\r\n", (unsigned long)len);
  // Descriptor van thuoc DMA (vi du lan truoc timeout) -> EAGAIN, khong ghi de
  } else if ((tx->des3 & EQOS_TDES3_OWN) != 0U) {
    tx_status = EQOS_EAGAIN; /* Never overwrite a buffer still owned by DMA. */
    // [FLOW]
    FLOW("[10] tx_desc[%lu] van OWN = 1 (DMA giu) -> EAGAIN\r\n", (unsigned long)idx);
  // Moi thu on -> gui
  } else {
    /* Copy into DMA-accessible RAM before publishing ownership.
     * DES0 points to the packet, DES1 is unused, and DES2.B1L equals the
     * number of valid bytes in that buffer. No interrupt-on-completion bit
     * is requested; completion is observed by polling DES3. */
    // Chep 22 byte vao buffer cua slot
    memcpy(tx_buf[idx], data, len);
    // TDES0 = dia chi buffer
    tx->des0 = (uint32_t)(uintptr_t)tx_buf[idx];
    // TDES1 khong dung
    tx->des1 = 0;
    // TDES2.B1L = so byte trong buffer (mask 14 bit)
    tx->des2 = len & EQOS_TDES2_B1L_MASK;
    // Cho cac lenh ghi tren vao RAM TRUOC khi set OWN
    __DSB();
    /* One packet fits in one descriptor: FD(First descriptor) and LD(Last descriptor) are both set.
     * FL is the total submitted frame length excluding generated FCS/pad.
     * CTXT=0 means normal descriptor; CIC=0 disables IP checksum insertion;
     * CPC=0 requests CRC and pad insertion in the H755 TX descriptor format.
     * Publish OWN LAST. First DSB orders data/address writes before OWN;
     * second DSB orders OWN before the MMIO tail update that wakes DMA. */
    // TDES3 = OWN (bit 31) | FD (29) | LD (28) | FL = 22 (bit 14:0) = 0xB0000016 -> trao cho DMA
    tx->des3 = EQOS_TDES3_OWN | EQOS_TDES3_FD | EQOS_TDES3_LD |
               (len & EQOS_TDES3_FL_MASK); // Mark descriptor as FD, LD , frame length and set OWN bit
    // Cho OWN vao RAM truoc khi danh thuc DMA
    __DSB();
    /* Increse the tx_head to the next descriptor to process. TX tail points at the
     * NEXT descriptor: this submits the preceding owned descriptor.
     * Tail advancement is necessary even if the descriptor is already OWN=1
     * when the engine is suspended at its previous work boundary. */
    // Slot tiep theo, quay vong 0->1->2->3->0 nho "& 3"
    eth.tx_head = (idx + 1U) & (EQOS_TX_DESC_COUNT - 1U);
    // [FLOW] In descriptor TRUOC khi danh thuc DMA (sau do DMA se ghi de write-back)
    FLOW("[10] TDES0=0x%08lX TDES1=0x%08lX TDES2=0x%08lX TDES3=0x%08lX (OWN=1 -> giao cho DMA)\r\n",
         (unsigned long)tx->des0, (unsigned long)tx->des1,
         (unsigned long)tx->des2, (unsigned long)tx->des3);
    // [FLOW2] Giai ma TDES3 dang read format (CPU vua ghi) va buffer DMA se doc
    flow_fields(F_TDES3_RD, FLOW_N(F_TDES3_RD), tx->des3);
    flow_frame_dump("[10] tx_buf (DMA se doc tu day)", tx_buf[idx], len, len - 14U);
    FLOW("[10] Ghi TX tail = &tx_desc[%lu] de danh thuc DMA, roi poll OWN...\r\n",
         (unsigned long)eth.tx_head);
    // Ghi tail TX ETH_DMACTXDTPR = &tx_desc[tx_head] -> danh thuc DMA
    AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR,
        (uint32_t)(uintptr_t)&tx_desc[eth.tx_head]); // Update Tail pointer
#if FLOW_LOG >= 2
    // [FLOW2] Moc thoi gian ngay sau khi danh thuc DMA (KHONG in gi o day)
    uint32_t flow_t_tail = DWT->CYCCNT;
#endif
    /* Wait for THIS submitted descriptor, not eth.tx_head (which advanced).
     * Approximately one million 1-us delays plus loop overhead is the bound.
     * Do not print inside this poll: UART latency would distort completion.
     * OWN clears before software interprets the TX write-back error summary. */
    // Bo dem so lan cho (~1 us moi lan)
    uint32_t elapsed = 0;
    // Luu gia tri DES3 doc duoc
    uint32_t tx_result;
    // Vong cho (KHONG printf o day): doc DES3 den khi OWN = 0, toi da ~1 giay
    do {
      tx_result = tx->des3;
      if ((tx_result & EQOS_TDES3_OWN) == 0U)
        break;
      eqos_udelay(1);
    } while (++elapsed < 1000000U);
#if FLOW_LOG >= 2
    // [FLOW2] Moc thoi gian TX xong; roi cho (CHI DOC, toi da 2 ms, khong in) den khi frame
    // loopback ve toi rx_desc[rx_head] de do thoi gian loopback. Khong thay doi trang thai nao.
    uint32_t flow_t_txdone = DWT->CYCCNT;
    uint32_t flow_rx_wait = 0;
    while ((rx_desc[eth.rx_head].des3 & EQOS_RDES3_OWN) != 0U && flow_rx_wait++ < 2000U)
      eqos_udelay(1);
    uint32_t flow_t_rx = DWT->CYCCNT;
    uint32_t flow_rx_seen = ((rx_desc[eth.rx_head].des3 & EQOS_RDES3_OWN) == 0U) ? 1U : 0U;
#endif
    /* Timeout does not cancel DMA or give ownership back to the CPU.
     * ES means hardware completed with an error; otherwise count local TX
     * completion.*/
    // Van OWN = 1 sau 1 giay -> timeout
    if ((tx_result & EQOS_TDES3_OWN) != 0U) {
      ++eth.stats.tx_timeouts;
      tx_status = EQOS_ETIMEDOUT;
    // ES (bit 15) = 1 -> phan cung bao loi
    } else if ((tx_result & EQOS_TDES3_WB_ES) != 0U) {
      ++eth.stats.tx_errors;
      tx_status = EQOS_EIO;
    // Khong loi -> dem goi gui thanh cong
    } else {
      ++eth.stats.tx_packets;
    }
    // [FLOW] Ket qua poll: thoi gian cho va TDES3 dang write-back
    FLOW("[10] Poll xong sau %lu us: TDES3 write-back = 0x%08lX (OWN=%lu ES=%lu)\r\n",
         (unsigned long)elapsed, (unsigned long)tx_result,
         (unsigned long)((tx_result >> 31) & 1U), (unsigned long)((tx_result >> 15) & 1U));
#if FLOW_LOG >= 2
    // [FLOW2] Giai ma TDES3 write-back + cac word con lai, thoi gian do bang DWT
    flow_fields(F_TDES3_WB, FLOW_N(F_TDES3_WB), tx_result);
    printf("[10] TDES0=0x%08lX TDES1=0x%08lX TDES2=0x%08lX (sau write-back; TDES0/1 = timestamp neu TTSS=1)\r\n",
           (unsigned long)tx->des0, (unsigned long)tx->des1, (unsigned long)tx->des2);
    printf("[10] Thoi gian tu luc ghi tail den khi DMA tra OWN (TX xong): ");
    flow_print_cycles(flow_t_txdone - flow_t_tail);
    printf("\r\n[10] Thoi gian tu luc ghi tail den khi frame loopback ve RX:  ");
    if (flow_rx_seen)
      flow_print_cycles(flow_t_rx - flow_t_tail);
    else
      printf("chua thay sau 2 ms (RX chua ve)");
    // 64 byte frame + 8 byte preamble/SFD = 576 bit: ~5.76 us o 100 Mbit/s, ~57.6 us o 10 Mbit/s (ETH_MACCR.FES)
    uint32_t flow_fes = AX_BASE_READ_REG32(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION) & EQOS_MAC_CFG_FES;
    printf("\r\n     (64 byte frame + 8 byte preamble/SFD o %s mat ~%s us tren duong truyen)\r\n",
           flow_fes ? "100 Mbit/s" : "10 Mbit/s", flow_fes ? "5.76" : "57.6");
    flow_dma_state();
    flow_mtl_state();
    flow_mac_state();
#endif
  }
  return tx_status;
}

/* [11] RX: polling for bit OWN set, check write-back Descriptor status.
 * Tra ve:  > 0         = do dai goi hop le; *data tro toi rx_buf cua goi do
 *          0           = descriptor da dung nhung khong co goi de xu ly (context, loi, bi bo).
 *                        Rieng goi loi (ES = 1): *data van tro toi rx_buf de nguoi goi in ra xem
 *                        (do dai = PL trong RDES3); goi do khong duoc tinh la hop le.
 *          EQOS_EAGAIN = DMA chua ghi xong (OWN = 1), khong co gi de lam
 * Moi lan tra ve khac EQOS_EAGAIN: phai goi eqos_recv_done() sau khi xu ly xong. */
int eqos_recv(uint8_t **data)
{
  //rx_head stored the next descriptor to receive in Rx process
  // Slot RX can xem
  uint32_t idx = eth.rx_head;
  // Con tro toi descriptor do
  struct eqos_desc *rx = &rx_desc[idx];

  /* Read a fresh volatile status for the current RX head on each main-loop
   * iteration. OWN=1 means no CPU-owned packet at this slot yet; no release
   * or head advancement is allowed in that case. Poll the ring in order. */
  // Doc DES3 mot lan (anh chup trang thai). Khong FLOW o day vi ham nay chay lien tuc
  uint32_t rx_status = rx->des3;
  // OWN = 1 -> DMA chua ghi frame nao vao slot nay
  if ((rx_status & EQOS_RDES3_OWN) != 0U)
    return EQOS_EAGAIN;
  // OWN = 0 -> DMA da ghi xong mot frame vao slot nay
  // Barrier truoc khi doc du lieu buffer
  __DSB(); /* Acquire packet bytes after DMA returns ownership. */
  /* OWN=0 mean Descriptor switches to RX write-back format.
   * CTXT is "Context type descriptor", not the "Normal" one.
   * ES summarizes receive errors. FD|LD must both be present because this
   * application cannot assemble frames spanning multiple descriptors.
   * PL is the hardware-reported length after applicable MAC stripping;
   * check it before using it as the limit for a memory read/UART dump. */
  // PL (bit 14:0) = so byte nhan duoc
  uint32_t len = rx_status & EQOS_RDES3_WB_PL_MASK;
  // [FLOW2] Tieu de buoc 11 (chi khi thuc su co frame ve)
  FLOW_STEP("[11]", "Nhan 1 frame (RX)");
  // [FLOW] Giai ma RDES3 write-back
  FLOW("\r\n[11] rx_desc[%lu] OWN = 0 -> DMA da ghi xong: RDES3=0x%08lX (CTXT=%lu FD=%lu LD=%lu ES=%lu PL=%lu)\r\n",
       (unsigned long)idx, (unsigned long)rx_status,
       (unsigned long)((rx_status >> 30) & 1U), (unsigned long)((rx_status >> 29) & 1U),
       (unsigned long)((rx_status >> 28) & 1U), (unsigned long)((rx_status >> 15) & 1U),
       (unsigned long)len);
  // [FLOW2] Giai ma day du RDES3 write-back; RDES0/1/2 luc nay chua status phu (VLAN, IP, filter)
  flow_fields(F_RDES3_WB, FLOW_N(F_RDES3_WB), rx_status);
  FLOW2("[11] RDES0=0x%08lX RDES1=0x%08lX RDES2=0x%08lX (write-back, khong con la dia chi buffer)\r\n",
        (unsigned long)rx->des0, (unsigned long)rx->des1, (unsigned long)rx->des2);
  // Context descriptor (cho timestamp, khong phai goi) -> khong lam gi
  if ((rx_status & EQOS_RDES3_WB_CTXT) != 0U) {
    /* Timestamp context is not a packet; recycle without printing. */
  // ES = 1 -> goi loi -> dem va in
  } else if ((rx_status & EQOS_RDES3_WB_ES) != 0U) {
    ++eth.stats.rx_errors;
    printf("RX descriptor error: %08lX\r\n", (unsigned long)rx_status);
    // Du lieu goi loi (vd. bi watchdog cat) van nam trong rx_buf: cho nguoi goi xem (van tra 0)
    *data = rx_buf[idx];
  // Thieu FD/LD (goi trai nhieu descriptor) hoac do dai 0 / > 1600 -> bo
  } else if ((rx_status & (EQOS_RDES3_WB_FD | EQOS_RDES3_WB_LD)) !=
             (EQOS_RDES3_WB_FD | EQOS_RDES3_WB_LD) || len == 0U || len > EQOS_BUF_SIZE) {
    ++eth.stats.rx_dropped;
    printf("RX dropped: descriptor=%08lX length=%lu\r\n",
           (unsigned long)rx_status, (unsigned long)len);
  // Goi hop le
  } else {
    // Dem goi nhan
    ++eth.stats.rx_packets;
    // Tra cho nguoi goi: du lieu nam o rx_buf cua slot nay
    *data = rx_buf[idx];
    return (int)len;
  }
  return 0;
}

/* [11] Tra descriptor RX tai rx_head lai cho DMA, roi sang slot tiep theo.
 * Both successful and discarded descriptors return to DMA same time.
 * Rebuild ALL four words(des0:3): DMA write-back may have overwritten address
 * fields with status. Returning only OWN would resubmit invalid contents.
 * Reset even rejected/error/context descriptors so one bad frame does not
 * permanently consume a ring entry. Do not access rx_buf[idx] after OWN.
 * DSB then tail write makes the rearmed descriptor visible before DMA is
 * notified; software advances rx_head only after returning this slot. */
void eqos_recv_done(void)
{
  // Slot vua xu ly
  uint32_t idx = eth.rx_head;
  // Con tro toi descriptor do
  struct eqos_desc *rx = &rx_desc[idx];
  // Dung lai descriptor: DES0 = dia chi buffer (bi write-back ghi de), DES1 = DES2 = 0
  rx->des0 = (uint32_t)(uintptr_t)rx_buf[idx];
  rx->des1 = 0;
  rx->des2 = 0;
  // Cho ghi xong truoc khi trao OWN
  __DSB();
  // Trao lai cho DMA
  rx->des3 = EQOS_RDES3_OWN | EQOS_RDES3_BUF1V;
  // Cho OWN vao RAM truoc khi ghi tail
  __DSB();
  // Ghi tail RX ETH_DMACRXDTPR = dia chi descriptor vua tra
  AX_BASE_WRITE_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_RXDESC_TAIL_PTR,
      (uint32_t)(uintptr_t)rx);
  // Sang slot tiep theo, quay vong 0..7
  eth.rx_head = (idx + 1U) & (EQOS_RX_DESC_COUNT - 1U);
  // [FLOW]
  FLOW("[11] Tra rx_desc[%lu] lai cho DMA (RDES3=0x%08lX), RX tail = &rx_desc[%lu], rx_head tiep theo = %lu\r\n",
       (unsigned long)idx, (unsigned long)rx->des3, (unsigned long)idx, (unsigned long)eth.rx_head);
}
