/*
 * test_loopback.c - phan TEST cua bai MAC loopback (khai bao o test_loopback.h).
 * Dung driver qua eqos_send() / eqos_recv() / eqos_recv_done() (desc/eqos_desc.c).
 */
#include "test_loopback.h"
#include "../core/eqos.h"
#include "../core/eqos_hw.h"
#include "../flow/eqos_flow.h"
#include <stdio.h>
#include <string.h>

// Bo dem de dung frame (TEST_FRAME_MAX byte, test dung 2300)
static uint8_t frame[TEST_FRAME_MAX];
// So byte hop le trong frame
static uint32_t frame_len;
// Ban sao frame RX vua nhan, de in va so sanh o cuoi [11]. Bang EQOS_BUF_SIZE de chua tron
// moi frame eqos_recv() tra ve (truoc la 128 byte: frame 2300 byte bi memcmp doc tran -> bao KHAC sai)
#define RX_COPY_MAX EQOS_BUF_SIZE
static uint8_t rx_copy[RX_COPY_MAX];

/* In n byte dang hex, 16 byte mot dong; cac dong sau thut vao indent dau cach */
static void print_hex(const uint8_t *p, uint32_t n, int indent)
{
  for (uint32_t i = 0; i < n; ++i) {
    if (i != 0U && (i % 16U) == 0U)
      printf("\r\n%*s", indent, "");
    printf("%02X ", (unsigned int)p[i]);
  }
  printf("\r\n");
}

/* Cuoi [11]: in frame da gui (frame[]) va frame nhan duoc (rx_copy[]) de so sanh.
 * err_rdes3 = 0: goi hop le. Khac 0: RDES3 cua goi loi (ES = 1) - van in ra de xem. */
static void print_tx_rx(uint32_t rx_len, uint32_t err_rdes3)
{
  uint32_t shown = (rx_len < RX_COPY_MAX) ? rx_len : RX_COPY_MAX;
  // Goi bi cat (ngan hon TX) thi chi so sanh duoc phan RX dang co
  uint32_t cmp = (shown < frame_len) ? shown : frame_len;
  int same = (memcmp(rx_copy, frame, cmp) == 0);

  printf("\r\n===== KET QUA TEST: TX da gui / RX nhan duoc =====\r\n");
  printf("TX packet %3lu byte: ", (unsigned long)frame_len);
  print_hex(frame, frame_len, 20);
  if (err_rdes3 != 0U)
    printf("RX packet LOI (ES=1, RDES3=0x%08lX: OE=%lu RWT=%lu GP=%lu CE=%lu) - eqos_recv() da bo, in de xem:\r\n",
           (unsigned long)err_rdes3, (unsigned long)((err_rdes3 >> 21) & 1U),
           (unsigned long)((err_rdes3 >> 22) & 1U), (unsigned long)((err_rdes3 >> 23) & 1U),
           (unsigned long)((err_rdes3 >> 24) & 1U));
  printf("RX packet %3lu byte: ", (unsigned long)rx_len);
  print_hex(rx_copy, shown, 20);
  if (shown < rx_len)
    printf("(chi in %lu byte dau)\r\n", (unsigned long)shown);
  if (rx_len < frame_len) {
    // Vd. watchdog RX cat frame: RX ngan hon TX
    printf("%lu byte dau: RX %s\r\n", (unsigned long)cmp, same ? "GIONG TX" : "KHAC TX");
    printf("RX ngan hon TX %lu byte -> frame bi cat (byte %lu..%lu cua TX khong toi RX)\r\n",
           (unsigned long)(frame_len - rx_len), (unsigned long)rx_len, (unsigned long)(frame_len - 1U));
    return;
  }
  printf("%lu byte dau: RX %s\r\n", (unsigned long)frame_len,
         same ? "GIONG TX -> loopback OK" : "KHAC TX");
  // MAC tu them pad 0x00 cho du 60 byte; FCS da bi bo (CST=1) nen khong co trong rx_buf
  if (same && rx_len > frame_len)
    printf("Byte %lu..%lu cua RX = pad 0x00 do MAC them (frame toi thieu 60 byte, chua tinh FCS)\r\n",
           (unsigned long)frame_len, (unsigned long)(rx_len - 1U));
}

/* [9] Raw Ethernet-II frame: 6 destination bytes + 6 source bytes + 2 EtherType
 * bytes + payload. 88 B5 is stored in network byte order; no IP header follows.
 * Current payload has eight literal bytes, so frame_len=22 (no trailing NUL).
 * DES3 CRC/padding controls remain zero in the TX path: hardware supplies
 * minimum-frame padding and the FCS; neither is included in frame_len.
 * In MAC loopback the RX length need not equal these 22 submitted bytes. */
void test_build_frame(const struct test_cfg *c, const uint8_t *src_mac)
{
  // [FLOW2]
  FLOW_STEP("[9]", "Dung frame thu nghiem");
  // Byte 0-5 = dia chi dich
  memcpy(frame, c->dest, 6);
  // Byte 6-11 = dia chi nguon (board, CFG.mac.addr)
  memcpy(frame + 6, src_mac, 6);
  // Byte 12-13 = EtherType 0x88B5, byte cao ghi truoc (big-endian / network byte order)
  frame[12] = (uint8_t)(c->ethertype >> 8);
  frame[13] = (uint8_t)(c->ethertype & 0xFFU);
  // Byte 14-21 = payload
  memcpy(frame + 14, c->payload, c->payload_len);
  // = 14 + 8 = 22 (chua tinh pad va FCS do MAC tu them)
  frame_len = 14U + c->payload_len;
#if FLOW_LOG == 1
  // [FLOW] (muc 1) In tung byte cua frame se gui
  FLOW("[9] Frame thu nghiem %lu byte (dich | nguon | EtherType | payload):\r\n    ", (unsigned long)frame_len);
  for (uint32_t i = 0; i < frame_len; ++i)
    FLOW("%02X ", (unsigned int)frame[i]);
  FLOW("\r\n");
#endif
  // [FLOW2] (muc 2) Hex dump co chu thich tung truong
  flow_frame_dump("[9] Frame se gui (chua co pad/FCS)", frame, frame_len, c->payload_len);
}

/* [10] Gui frame thu nghiem bang eqos_send(), roi in ket qua va bo dem MMC */
void test_send_once(void)
{
  // Slot TX se dung (eqos_send() dung slot nay roi tang eth.tx_head)
  uint32_t idx = eth.tx_head;
  // [FLOW]
  // [FLOW2]
  FLOW_STEP("[10]", "Gui 1 frame (TX)");
  FLOW("\r\n[10] Co yeu cau gui -> dung tx_desc[%lu] @%p\r\n", (unsigned long)idx, (void *)&tx_desc[idx]);
  // Gui va cho DMA tra OWN; ket qua la ma EQOS_*
  int tx_status = eqos_send(frame, frame_len);
  // In ket qua: "complete"/"failed", ma loi (0 = OK), do dai truoc pad/FCS
  printf("TX: %s (%d), %lu bytes before padding/FCS\r\n",
         tx_status == EQOS_OK ? "complete" : "failed", tx_status,
         (unsigned long)frame_len);
  /* MMC counters count packets, not status bits. Read each counter once.
   * STM32H755 reserves 0x718; only generic EQoS targets may expose it.
   * Counts depend on MMC reset/reset-on-read settings. A good TX count
   * does not prove loopback RX: check the RX descriptor and packet too. */
  // Tren H755 EQOS_STM32H7 = 1 -> chi bien dich dong printf "unsupported"; nhanh #else khong bien dich
#if EQOS_STM32H7
  printf("Tx_Packet_Count_Good_bad [0x718]: unsupported on STM32H755 (reserved)\r\n");
#else
  uint32_t tx_count_good_bad = AX_BASE_READ_REG32(EQOS_MAC_BASE, 0x718U);
  printf("Tx_Packet_Count_Good_bad [0x718] = 0x%08lX (%lu): good + bad TX packets\r\n",
         (unsigned long)tx_count_good_bad, (unsigned long)tx_count_good_bad);
#endif
  // Doc bo dem MMC ETH_TX_PACKET_COUNT_GOOD (0x40028768) va in
  uint32_t tx_count_good = AX_BASE_READ_REG32(EQOS_MAC_BASE, 0x768U);
  printf("Tx_Packet_Count_Good [0x768] = 0x%08lX (%lu): TX packets without errors\r\n",
         (unsigned long)tx_count_good, (unsigned long)tx_count_good);
  // Nhac: phai xem khoi RX moi biet loopback thanh cong
  printf("Loopback status: check RX below; TX count alone does not confirm RX.\r\n");
  // Neu gui loi -> in DMACSR, TDES3 va TX tail de debug
  if (tx_status != EQOS_OK)
    printf("DMACSR=%08lX TXDESC3=%08lX TXTAIL=%08lX\r\n",
           (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_STATUS), (unsigned long)tx_desc[idx].des3,
           (unsigned long)AX_BASE_READ_REG32(EQOS_DMA_BASE, EQOS_DMA_CH0_TXDESC_TAIL_PTR));
  // [FLOW] Thong ke phan mem sau lan gui
  FLOW("[10] Thong ke: tx_packets=%lu tx_errors=%lu tx_timeouts=%lu, tx_head tiep theo=%lu\r\n",
       (unsigned long)eth.stats.tx_packets, (unsigned long)eth.stats.tx_errors,
       (unsigned long)eth.stats.tx_timeouts, (unsigned long)eth.tx_head);
  // [FLOW2] Bo dem MMC va 2 ring sau khi gui
  flow_mmc();
  flow_rings("sau khi gui");
}

/* [11] Moi vong lap chinh: neu DMA da tra 1 frame thi in ra, so voi frame da gui,
 * roi tra descriptor cho DMA (ca goi tot lan goi loi/bi bo/context). */
void test_poll_rx(void)
{
  // Con tro toi du lieu goi (eqos_recv() dien khi co goi hop le, hoac goi loi ES = 1)
  uint8_t *data = NULL;
  // > 0: do dai goi hop le; 0: descriptor khong co goi; EQOS_EAGAIN: chua co gi
  int ret = eqos_recv(&data);
  // Chua co frame -> thoat, vong lap chinh se goi lai
  if (ret == EQOS_EAGAIN)
    return;
  // So byte RX nhan duoc (0 = descriptor khong co goi hop le)
  uint32_t len = 0U;
  // RDES3 cua goi loi (0 = khong co goi loi), de in o cuoi [11]
  uint32_t err_rdes3 = 0U;
  // Goi hop le
  if (ret > 0) {
    len = (uint32_t)ret;
    /* Keep this buffer CPU-owned while reading it. Returning OWN early would
     * let DMA overwrite bytes while printf is still reading them, so the
     * frame is copied now and the copy is printed at the end of [11].
     * The FLOW dumps at 115200 baud are slow; other descriptors can fill
     * and RX can suspend/drop under traffic. This path favors inspection,
     * not sustained line-rate throughput. */
    memcpy(rx_copy, data, (len < RX_COPY_MAX) ? len : RX_COPY_MAX);
    // [FLOW] So sanh 22 byte dau voi frame da gui: 0 = giong het (phan con lai la pad)
    FLOW("[11] So voi frame da gui (%lu byte dau): %s\r\n", (unsigned long)frame_len,
         (len >= frame_len && memcmp(data, frame, frame_len) == 0) ? "GIONG -> loopback OK" : "KHAC");
    // [FLOW2] Hex dump co chu thich: thay ro phan pad 0x00 do MAC them
    flow_frame_dump("[11] rx_buf nhan duoc", data, len, frame_len - 14U);
  // Goi loi (ES = 1, vd. bi watchdog cat): eqos_recv() tra 0 nhung van cho data tro toi rx_buf.
  // Chep ra truoc eqos_recv_done() de cuoi [11] in so sanh. Do dai = PL (chan toi da RX_COPY_MAX).
  } else if (data != NULL) {
    err_rdes3 = rx_desc[eth.rx_head].des3;
    len = err_rdes3 & EQOS_RDES3_WB_PL_MASK;
    if (len > RX_COPY_MAX)
      len = RX_COPY_MAX;
    memcpy(rx_copy, data, len);
  }
  // Tra descriptor cho DMA. Sau dong nay khong duoc doc data[] nua
  eqos_recv_done();
  // [FLOW]
  FLOW("[11] Thong ke: rx_packets=%lu rx_errors=%lu rx_dropped=%lu. Nhan nut USER de gui them.\r\n",
       (unsigned long)eth.stats.rx_packets, (unsigned long)eth.stats.rx_errors,
       (unsigned long)eth.stats.rx_dropped);
  // [FLOW2] Trang thai DMA/FIFO, bo dem MMC va 2 ring sau khi tra descriptor
  flow_dma_state();
  flow_mtl_state();
  flow_mmc();
  flow_rings("sau khi tra RX");
  // Cuoi test: in frame da gui va frame nhan duoc (luon in, khong phu thuoc FLOW_LOG)
  if (len > 0U)
    print_tx_rx(len, err_rdes3);
}
