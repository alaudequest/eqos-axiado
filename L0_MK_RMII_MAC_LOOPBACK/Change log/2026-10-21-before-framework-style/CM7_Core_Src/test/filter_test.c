/* SPDX-License-Identifier: GPL-2.0 */
/*
 * filter_test.c - BAI THUC HANH bo loc dia chi cua MAC (ETH_MACPFR: perfect, hash,
 * broadcast, multicast, dao nguoc, Receive All, loc SA) bang MAC loopback.
 *
 * File DOC LAP: khong sua file nao khac. Chi goi API co san cua driver (eqos_*,
 * stm32_*) va BSP Nucleo. Mac dinh TAT (FILTER_TEST_ENABLE = 0): khi do file nay
 * khong sinh ra code nao va chuong trinh loopback trong main.c chay nhu cu.
 *
 * CACH CHAY
 *  1. Doi FILTER_TEST_ENABLE ben duoi thanh 1, build, nap.
 *  2. Mo COM 115200-8N1. filter_test_boot() chay TRUOC main(): ham co
 *     __attribute__((constructor)) nen startup goi no trong __libc_init_array,
 *     ngay truoc "bl main" (Core/Startup/startup_stm32h755zitx.s). Ham nay tu
 *     khoi dong board + Ethernet, chay het bang CASES[], in bang ket qua, in
 *     lai frame TX da gui + frame RX nhan duoc cua tung dong (FT_DUMP_PACKETS),
 *     roi cho nut USER va KHONG BAO GIO return -> main() trong main.c khong chay.
 *  3. Nhan nut USER de chay lai ca bang.
 *  4. Doi lai 0 de quay ve chuong trinh loopback binh thuong.
 *  Goi y: dat FLOW_LOG 0 trong flow/eqos_flow.h de log gon (khong bat buoc,
 *  bang ket qua cuoi cung luon duoc in).
 *
 * MOI DONG CUA CASES[]: dat bo loc -> gui 1 frame co DA (va SA) chon san qua
 * MAC loopback -> xem frame co ve RX khong -> so voi cot "du kien".
 * Tu thu: sua/them dong trong CASES[] (doi bit MACPFR, them dia chi vao bang
 * hash, doi DA...), dien "du kien" theo suy luan cua ban roi chay lai. Dong
 * nao in SAI la cho can xem lai.
 *
 * Tra cuu RM0399 Rev 4: ETH_MACPFR tr.3119-3120; bang loc DA/SA (Table 549/550)
 * tr.2956-2957; ETH_MACHT0R/HT1R va cach tinh ma hash tr.3121; ETH_MACAxHR/LR
 * tr.3155; RDES2 write-back (HF, MADRM, DAF, SAF) tr.3068.
 */

#ifndef FILTER_TEST_ENABLE
#define FILTER_TEST_ENABLE 0   /* 1 = chay bai thuc hanh nay thay cho main() */
#endif

#if FILTER_TEST_ENABLE

#include "main.h"                 /* HAL, BSP Nucleo (COM1, nut USER), Error_Handler */
#include "../core/eqos.h"         /* eqos_*, rx_desc, eth, cac struct cau hinh */
#include "../core/eqos_hw.h"      /* EQOS_MAC_BASE, AX_BASE_READ/WRITE_REG32, bit RDES3 */
#include "../port/eqos_port.h"    /* struct board_cfg, stm32_precheck, stm32_config_eth */
#include <stdio.h>
#include <string.h>

// Ham cua main.c (khong static): HSI 64 MHz -> PLL1 -> CPU/HCLK 224 MHz
void SystemClock_Config(void);

#ifndef FT_DUMP_PACKETS
#define FT_DUMP_PACKETS 1   /* 1 = cuoi bai in lai frame TX/RX cua tung tinh huong */
#endif
#define FT_FRAME_LEN  22U   // frame gui: 14 byte header + "FILT" + so thu tu + 3 byte 0
#define FT_RX_KEEP    64U   // so byte dau cua frame RX duoc giu lai de in (frame ve dai 60)

/* ---- Thanh ghi bo loc, offset tinh tu EQOS_MAC_BASE (0x40028000) ---- */
#define REG_MACPFR    0x008U      // Packet filtering control  (tr.3119)
#define REG_MACHT0R   0x010U      // Bang hash, ma 0..31       (tr.3121)
#define REG_MACHT1R   0x014U      // Bang hash, ma 32..63      (tr.3121)
#define REG_MACA1HR   0x308U      // Dia chi MAC so 1, byte 5:4 (tr.3155)
#define REG_MACA1LR   0x30CU      // Dia chi MAC so 1, byte 3:0 (tr.3155)

/* Bit cua ETH_MACPFR */
#define PFR_PR    (1U << 0)       // Promiscuous: cho qua het
#define PFR_HUC   (1U << 1)       // Unicast loc bang hash (0 = perfect)
#define PFR_HMC   (1U << 2)       // Multicast loc bang hash (0 = perfect)
#define PFR_DAIF  (1U << 3)       // Dao nguoc ket qua loc DA
#define PFR_PM    (1U << 4)       // Cho qua moi multicast
#define PFR_DBF   (1U << 5)       // Chan moi broadcast
#define PFR_SAF   (1U << 9)       // Loc SA: truot thi vut
#define PFR_HPF   (1U << 10)      // Khi HUC/HMC = 1: qua neu khop hash HOAC perfect
#define PFR_RA    (1U << 31)      // Receive All: cho qua het nhung van ghi ket qua loc vao RDES2

/* Bit cua ETH_MACA1HR */
#define A_AE            (1U << 31)          // Address Enable: dung dia chi nay de loc
#define A_SA            (1U << 30)          // 1 = so voi SA cua frame, 0 = so voi DA
#define A_MASK_BYTE(k)  (1U << (24U + (k))) // MBC: bo qua byte k (0 = byte dau, 5 = byte cuoi)

/* Trang thai bo loc trong descriptor RX (write-back) */
#define RDES3_RS2V      (1U << 27)                  // RDES2 co trang thai hop le
#define RDES2_HF        (1U << 18)                  // qua bang bo loc HASH
#define RDES2_DAF       (1U << 17)                  // truot loc DA (chi thay duoc khi RA = 1)
#define RDES2_SAF       (1U << 16)                  // truot loc SA
#define RDES2_MADRM(v)  (((v) >> 19) & 0xFFU)       // HF = 0: so MACAx khop; HF = 1: ma hash

/* ---- Cac dia chi dung trong bai ---- */
static const uint8_t BOARD[6]    = {0x00, 0x80, 0xE1, 0x00, 0x00, 0x00}; // MACA0 = dia chi board
static const uint8_t BOARD_1[6]  = {0x00, 0x80, 0xE1, 0x00, 0x00, 0x01}; // khac board dung byte cuoi
static const uint8_t PEER[6]     = {0x6C, 0x1F, 0xF7, 0xCA, 0x2B, 0xFD}; // unicast la (dich cua test chinh)
static const uint8_t BCAST[6]    = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // broadcast
static const uint8_t MDNS[6]     = {0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB}; // multicast mDNS (224.0.0.251)
static const uint8_t ALLHOST[6]  = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01}; // multicast all-hosts (224.0.0.1)
static const uint8_t ALLRTR[6]   = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x02}; // multicast all-routers (224.0.0.2)
static const uint8_t LEAK32[6]   = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x18}; // ma hash trung voi ALLHOST
static const uint8_t LEAK48[6]   = {0x01, 0x00, 0x5E, 0x00, 0x00, 0xC9}; // ma hash trung voi MDNS
static const uint8_t IPV6ALL[6]  = {0x33, 0x33, 0x00, 0x00, 0x00, 0x01}; // multicast IPv6 all-nodes
static const uint8_t OTHER_SA[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x99}; // nguon la (bai loc SA)

/* Mot tinh huong: cau hinh bo loc + frame gui + ket qua du kien */
struct filter_case {
  const char    *id;        // ten ngan
  const char    *what;      // y nghia
  uint32_t       pfr;       // gia tri ghi vao ETH_MACPFR
  const uint8_t *hash[3];   // dia chi dua vao bang hash (NULL = het)
  const uint8_t *a1;        // dia chi cho MACA1 (NULL = tat MACA1)
  uint32_t       a1_flags;  // A_SA, A_MASK_BYTE(k) cho MACA1 (AE tu them)
  const uint8_t *da;        // DA cua frame gui
  const uint8_t *sa;        // SA cua frame gui (NULL = BOARD)
  int            expect;    // du kien: 1 = QUA, 0 = VUT
};

static const struct filter_case CASES[] = {
  /* A. PR = 1: bo qua moi bo loc dia chi */
  {"A1", "PR=1: DA la van qua",                     PFR_PR,   {0},             NULL,  0,              PEER,    NULL,     1},
  /* B. Perfect (HUC = 0): so du 48 bit voi MACA0..MACA3 */
  {"B1", "perfect: DA = board (MACA0)",             0,        {0},             NULL,  0,              BOARD,   NULL,     1},
  {"B2", "perfect: DA la",                          0,        {0},             NULL,  0,              PEER,    NULL,     0},
  {"B3", "perfect: sai dung byte cuoi",             0,        {0},             NULL,  0,              BOARD_1, NULL,     0},
  {"B4", "perfect: them PEER vao MACA1",            0,        {0},             PEER,  0,              PEER,    NULL,     1},
  {"B5", "group: MACA1 = board, bo qua byte cuoi",  0,        {0},             BOARD, A_MASK_BYTE(5), BOARD_1, NULL,     1},
  /* C. Broadcast */
  {"C1", "broadcast, DBF=0",                        0,        {0},             NULL,  0,              BCAST,   NULL,     1},
  {"C2", "broadcast, DBF=1",                        PFR_DBF,  {0},             NULL,  0,              BCAST,   NULL,     0},
  /* D. Multicast */
  {"D1", "multicast perfect: khong co trong MACAx",  0,        {0},             NULL,  0,              MDNS,    NULL,     0},
  {"D2", "PM=1: moi multicast deu qua",             PFR_PM,   {0},             NULL,  0,              MDNS,    NULL,     1},
  {"D3", "hash {mDNS, all-hosts}: gui mDNS",        PFR_HMC,  {MDNS, ALLHOST}, NULL,  0,              MDNS,    NULL,     1},
  {"D4", "hash {mDNS, all-hosts}: gui all-hosts",   PFR_HMC,  {MDNS, ALLHOST}, NULL,  0,              ALLHOST, NULL,     1},
  {"D5", "hash {mDNS, all-hosts}: gui all-routers", PFR_HMC,  {MDNS, ALLHOST}, NULL,  0,              ALLRTR,  NULL,     0},
  {"D6", "hash: ..:00:18 LOT (trung ma all-hosts)", PFR_HMC,  {MDNS, ALLHOST}, NULL,  0,              LEAK32,  NULL,     1},
  {"D7", "hash: ..:00:C9 LOT (trung ma mDNS)",      PFR_HMC,  {MDNS, ALLHOST}, NULL,  0,              LEAK48,  NULL,     1},
  {"D8", "hash: gui IPv6 33:33:00:00:00:01",        PFR_HMC,  {MDNS, ALLHOST}, NULL,  0,              IPV6ALL, NULL,     0},
  /* E. Unicast bang hash, va HPF */
  {"E1", "HUC=1, hash {PEER}: gui PEER",            PFR_HUC,  {PEER},          NULL,  0,              PEER,    NULL,     1},
  {"E2", "HUC=1, HPF=0: gui board (chi hash)",      PFR_HUC,  {PEER},          NULL,  0,              BOARD,   NULL,     0},
  {"E3", "HUC=1, HPF=1: gui board (hash HOAC perf)",PFR_HUC | PFR_HPF, {PEER}, NULL,  0,              BOARD,   NULL,     1},
  /* F. Dao nguoc ket qua loc DA */
  {"F1", "DAIF=1: DA = board -> bi vut",            PFR_DAIF, {0},             NULL,  0,              BOARD,   NULL,     0},
  {"F2", "DAIF=1: DA la -> qua",                    PFR_DAIF, {0},             NULL,  0,              PEER,    NULL,     1},
  /* G. Receive All: qua het, RDES2.DAF cho biet frame le ra bi loc */
  {"G1", "RA=1: DA la van qua, DAF=1",              PFR_RA,   {0},             NULL,  0,              PEER,    NULL,     1},
  /* H. Loc dia chi nguon: MACA1 dung lam dia chi SA */
  {"H1", "SAF=1, MACA1(SA)=board: SA = board",      PFR_SAF,  {0},             BOARD, A_SA,           BOARD,   BOARD,    1},
  {"H2", "SAF=1, MACA1(SA)=board: SA la",           PFR_SAF,  {0},             BOARD, A_SA,           BOARD,   OTHER_SA, 0},
};
#define N_CASES (sizeof CASES / sizeof CASES[0])

/* Ket qua do duoc cua mot tinh huong */
struct case_result {
  int      tx;                 // ma tra ve cua eqos_send()
  int      got;                // 1 = frame da ve RX
  uint32_t ht[2];              // HT0R, HT1R da ghi
  uint32_t rdes2;              // RDES2 write-back cua frame nhan duoc
  uint32_t rdes3;              // RDES3 write-back cua frame nhan duoc
  uint8_t  txf[FT_FRAME_LEN];  // frame da gui (chua co pad/FCS)
  uint32_t rx_len;             // do dai frame nhan duoc (RDES3.PL: co pad, khong co FCS)
  uint8_t  rxf[FT_RX_KEEP];    // FT_RX_KEEP byte dau cua frame nhan duoc
};

/* Cau hinh khoi dong: giong CFG trong main.c, nhung PR = 0 (bo loc do tung tinh huong dat) */
static const struct board_cfg FT_BOARD = {
  .uart_baud = 115200,
  .rmii_pins = {
    {GPIOA, 1}, {GPIOA, 2}, {GPIOA, 7},    /* REF_CLK, MDIO, CRS_DV */
    {GPIOC, 1}, {GPIOC, 4}, {GPIOC, 5},    /* MDC, RXD0, RXD1 */
    {GPIOB, 13}, {GPIOG, 11}, {GPIOG, 13}  /* TXD1, TX_EN, TXD0 */
  },
};
static const struct dma_cfg  FT_DMA  = { .osf = true, .txpbl = 8, .rxpbl = 8, .pblx8 = true, .aal = true, .fb = true };
static const struct mtl_cfg  FT_MTL  = { .tsf = true, .rsf = true, .dis_tcp_ef = true };
static const struct mac_cfg  FT_MAC  = { .addr = {0x00, 0x80, 0xE1, 0x00, 0x00, 0x00}, .promiscuous = false,
                                         .loopback = true, .cst = true, .acs = true };
static const struct link_cfg FT_LINK = { .speed = EQOS_SPEED_100, .full_duplex = true };

/* CRC-32 chuan IEEE 802.3 (cung la FCS cua Ethernet) */
static uint32_t crc32_ieee(const uint8_t *p, uint32_t n)
{
  uint32_t c = 0xFFFFFFFFU;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; ++k)
      c = (c >> 1) ^ (0xEDB88320U & (0U - (c & 1U)));
  }
  return ~c;
}

/* Ma hash 0..63 cua mot dia chi (RM tr.3121): (1) CRC-32 cua 6 byte,
 * (2) dao nguoc thu tu 32 bit (lenh RBIT cua Cortex-M7), (3) lay 6 bit cao */
static uint32_t hash_index(const uint8_t mac[6])
{
  return __RBIT(crc32_ieee(mac, 6U)) >> 26;
}

static void print_mac(const uint8_t *m)
{
  printf("%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* Ghi mot thanh ghi bo loc roi cho 1 us: bo loc chay o mien clock RX (RMII),
 * nen de cac lan ghi lien tiep cach nhau vai chu ky clock do */
static void write_filter_reg(uint32_t off, uint32_t v)
{
  AX_BASE_WRITE_REG32(EQOS_MAC_BASE, off, v);
  eqos_udelay(1);
}

/* Dat toan bo bo loc cho mot tinh huong: bang hash, MACA1, roi ETH_MACPFR */
static void apply_filter(const struct filter_case *c, uint32_t ht[2])
{
  // Bang hash: moi dia chi bat 1 bit. Bit 5 cua ma chon HT0R/HT1R, 5 bit thap chon vi tri bit
  ht[0] = 0U;
  ht[1] = 0U;
  for (uint32_t i = 0; i < 3U && c->hash[i] != NULL; ++i) {
    uint32_t h = hash_index(c->hash[i]);
    ht[h >> 5] |= 1U << (h & 31U);
  }
  write_filter_reg(REG_MACHT0R, ht[0]);
  write_filter_reg(REG_MACHT1R, ht[1]);

  // MACA1: cung cach xep byte nhu MACA0 (eqos_config_mac): HR = byte 5:4, LR = byte 3:0
  if (c->a1 != NULL) {
    write_filter_reg(REG_MACA1HR, A_AE | c->a1_flags | ((uint32_t)c->a1[5] << 8) | c->a1[4]);
    write_filter_reg(REG_MACA1LR, ((uint32_t)c->a1[3] << 24) | ((uint32_t)c->a1[2] << 16) |
                                  ((uint32_t)c->a1[1] << 8) | c->a1[0]);
  } else {
    write_filter_reg(REG_MACA1HR, 0x0000FFFFU);   // gia tri reset: AE = 0 -> MACA1 khong dung
    write_filter_reg(REG_MACA1LR, 0xFFFFFFFFU);
  }

  write_filter_reg(REG_MACPFR, c->pfr);
}

/* Tra lai moi descriptor RX dang giu (frame cu con sot) truoc moi tinh huong */
static void drain_rx(void)
{
  uint8_t *data;
  while (eqos_recv(&data) != EQOS_EAGAIN)
    eqos_recv_done();
}

/* Chay mot tinh huong: dat bo loc, gui 1 frame, cho toi da 5 ms xem frame co ve khong */
static void run_case(uint8_t n, const struct filter_case *c, struct case_result *r)
{
  uint8_t *frame = r->txf;   // dung frame ngay trong r de cuoi bai in lai duoc

  drain_rx();
  apply_filter(c, r->ht);

  // Frame: DA | SA | EtherType 0x88B5 | "FILT" + so thu tu (de nhan ra dung frame cua minh)
  memcpy(frame, c->da, 6);
  memcpy(frame + 6, (c->sa != NULL) ? c->sa : BOARD, 6);
  frame[12] = 0x88;
  frame[13] = 0xB5;
  memcpy(frame + 14, "FILT", 4);
  frame[18] = n;
  frame[19] = frame[20] = frame[21] = 0U;

  r->got = 0;
  r->rdes2 = 0U;
  r->rdes3 = 0U;
  r->rx_len = 0U;
  r->tx = eqos_send(frame, FT_FRAME_LEN);
  if (r->tx != EQOS_OK)
    return;

  for (uint32_t us = 0; us < 5000U && !r->got; ++us) {
    struct eqos_desc *d = &rx_desc[eth.rx_head];
    uint32_t des3 = d->des3;
    if ((des3 & EQOS_RDES3_OWN) != 0U) {
      eqos_udelay(1);
      continue;
    }
    // Doc RDES2 truoc eqos_recv_done() (ham do ghi lai DES2 = 0)
    uint32_t des2 = d->des2;
    uint8_t *data = NULL;
    int len = eqos_recv(&data);
    if (len >= 19 && memcmp(data + 14, "FILT", 4) == 0 && data[18] == n) {
      r->got = 1;
      r->rdes2 = des2;
      r->rdes3 = des3;
      // Chep frame ra truoc eqos_recv_done(): sau ham do DMA duoc ghi de rx_buf
      r->rx_len = (uint32_t)len;
      memcpy(r->rxf, data, (r->rx_len < FT_RX_KEEP) ? r->rx_len : FT_RX_KEEP);
    }
    eqos_recv_done();
  }
}

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

/* Cuoi bai: in lai frame da gui va frame nhan duoc cua tung tinh huong */
static void print_packets(const struct case_result res[])
{
  printf("\r\n================ DU LIEU GOI TIN: TX da gui / RX nhan duoc ================\r\n");
  printf("TX = frame dua cho eqos_send() (chua co pad/FCS).\r\n");
  printf("RX = rx_buf DMA ghi: MAC them pad 0x00 cho du 60 byte, FCS da bo (CST=1).\r\n");
  for (uint32_t i = 0; i < N_CASES; ++i) {
    const struct filter_case *c = &CASES[i];
    const struct case_result *r = &res[i];

    printf("\r\n%-3s %s\r\n", c->id, c->what);
    printf("  TX %2lu byte: ", (unsigned long)FT_FRAME_LEN);
    print_hex(r->txf, FT_FRAME_LEN, 14);
    if (r->tx != EQOS_OK) {
      printf("  RX: khong co (eqos_send() loi %d)\r\n", r->tx);
    } else if (!r->got) {
      printf("  RX: khong co (bo loc da vut frame)\r\n");
    } else {
      uint32_t shown = (r->rx_len < FT_RX_KEEP) ? r->rx_len : FT_RX_KEEP;
      printf("  RX %2lu byte: ", (unsigned long)r->rx_len);
      print_hex(r->rxf, shown, 14);
      if (shown < r->rx_len)
        printf("  (chi giu %lu byte dau)\r\n", (unsigned long)shown);
      printf("  %lu byte dau cua RX %s TX\r\n", (unsigned long)FT_FRAME_LEN,
             (r->rx_len >= FT_FRAME_LEN && memcmp(r->rxf, r->txf, FT_FRAME_LEN) == 0) ? "GIONG" : "KHAC");
    }
  }
}

/* Chay het CASES[], in tung dong va tong ket */
static void run_all(void)
{
  static struct case_result res[N_CASES];   // giu ket qua tung dong de cuoi bai in lai frame
  uint32_t ok = 0;

  printf("\r\n================ BAI THUC HANH BO LOC DIA CHI MAC (filter_test.c) ================\r\n");
  printf("Board: MACA0 = "); print_mac(BOARD); printf(". Moi dong: dat bo loc -> gui 1 frame qua MAC loopback -> xem co ve RX.\r\n");
  printf("hashDA      = ma hash phan mem tinh cho DA (RM tr.3121), bit 5 chon HT1R/HT0R.\r\n");
  printf("HF/MADRM    = RDES2 do PHAN CUNG ghi: HF=1 -> MADRM la ma hash phan cung tinh (phai = hashDA);\r\n");
  printf("              HF=0 -> MADRM la so thanh ghi MACAx da khop. DAF/SAF = 1: truot loc DA/SA.\r\n\r\n");
  printf("ID  DA gui             MACPFR     HT1R     HT0R     hashDA | du kien thuc te | HF MADRM DAF SAF | ket qua\r\n");

  for (uint32_t i = 0; i < N_CASES; ++i) {
    const struct filter_case *c = &CASES[i];
    struct case_result r;
    run_case((uint8_t)(i + 1U), c, &r);

    uint32_t h = hash_index(c->da);
    int pass = r.got;
    int match = (r.tx == EQOS_OK) && (pass == c->expect);
    ok += (uint32_t)match;

    printf("%-3s ", c->id);
    print_mac(c->da);
    printf("  0x%08lX %08lX %08lX %2lu     | %-7s %-7s | ", (unsigned long)c->pfr,
           (unsigned long)r.ht[1], (unsigned long)r.ht[0], (unsigned long)h,
           c->expect ? "QUA" : "VUT", (r.tx != EQOS_OK) ? "TX LOI" : (pass ? "QUA" : "VUT"));
    if (pass && (r.rdes3 & RDES3_RS2V) != 0U) {
      uint32_t hf = (r.rdes2 & RDES2_HF) ? 1U : 0U;
      uint32_t madrm = RDES2_MADRM(r.rdes2);
      printf("%2lu %5lu %3lu %3lu | ", (unsigned long)hf, (unsigned long)madrm,
             (unsigned long)((r.rdes2 & RDES2_DAF) ? 1U : 0U), (unsigned long)((r.rdes2 & RDES2_SAF) ? 1U : 0U));
      printf("%s", match ? "OK " : "SAI");
      if (hf != 0U)
        printf("%s", (madrm == h) ? " (ma hash khop)" : " (ma hash KHAC!)");
    } else {
      printf(" -     -   -   - | %s", match ? "OK " : "SAI");
    }
    printf("  %s\r\n", c->what);
    res[i] = r;
  }

  printf("\r\nKet qua: %lu/%lu dong dung du kien.\r\n", (unsigned long)ok, (unsigned long)N_CASES);
  if (ok != N_CASES)
    printf("Dong SAI: hoac du kien sai (xem lai Table 549/550, RM tr.2956-2957), hoac phan cung lam khac RM.\r\n");
  if (FT_DUMP_PACKETS)
    print_packets(res);

  // Tra bo loc ve trang thai don gian: PR = 1, bang hash rong, MACA1 tat
  static const struct filter_case reset_filter = {"--", "", PFR_PR, {0}, NULL, 0, BOARD, NULL, 1};
  uint32_t ht[2];
  apply_filter(&reset_filter, ht);
}

/* Cho mot lan nhan + nha nut USER (khong phu thuoc nut tich cuc muc cao hay thap) */
static void wait_button(void)
{
  int32_t idle = BSP_PB_GetState(BUTTON_USER);
  while (BSP_PB_GetState(BUTTON_USER) == idle) { }
  HAL_Delay(30);
  while (BSP_PB_GetState(BUTTON_USER) != idle) { }
  HAL_Delay(30);
}

/* Diem vao: chay truoc main() (constructor), khong bao gio return */
__attribute__((constructor)) static void filter_test_boot(void)
{
  COM_InitTypeDef com = { 115200, COM_WORDLENGTH_8B, COM_STOPBITS_1, COM_PARITY_NONE, COM_HWCONTROL_NONE };

  // Board: HAL + clock + nut USER (che do GPIO, khong ngat) + UART COM1
  HAL_Init();
  SystemClock_Config();
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_GPIO);
  if (BSP_COM_Init(COM1, &com) != BSP_ERROR_NONE)
    Error_Handler();
  printf("\r\n ============ filter_test.c: bai thuc hanh bo loc (main() se KHONG chay) ============\r\n");

  // Ethernet: cung thu tu nhu main() (RM0399 61.9), MAC loopback, 100 Mbit/s full duplex
  if (stm32_precheck() != EQOS_OK)
    Error_Handler();
  stm32_config_eth(&FT_BOARD);
  if (eqos_reset() != EQOS_OK)
    Error_Handler();
  eqos_config_dma(&FT_DMA);
  eqos_setup_rings();
  eqos_config_mtl(&FT_MTL);
  eqos_config_mac(&FT_MAC, &FT_LINK);
  eqos_start_dma();
  eqos_start_mac();

  for (;;) {
    run_all();
    printf("Nhan nut USER de chay lai.\r\n");
    wait_button();
  }
}

#else
typedef int filter_test_disabled_t;   /* file dang tat: tranh translation unit rong */
#endif /* FILTER_TEST_ENABLE */
