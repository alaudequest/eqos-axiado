
/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
// Header chinh cua CubeMX: keo theo HAL, BSP Nucleo (nut, COM) va CMSIS (ETH, RCC, GPIO, SCB...)
#include "main.h"
// Driver PHY LAN8742 - hien KHONG dung (phan cau hinh PHY da bo)
#include "lan8742.h"
// Driver EQoS: kieu cau hinh DMA/MTL/MAC/link, struct descriptor, ham eqos_*, ma loi EQOS_*
#include "core/eqos.h"
// Base address tung khoi, offset thanh ghi, ten bit, macro AX_BASE_READ/WRITE_REG32
#include "core/eqos_hw.h"
// Phan STM32 cua driver: struct board_cfg (chan RMII), stm32_precheck(), stm32_config_eth()
#include "port/eqos_port.h"
// Log FLOW: FLOW_LOG, FLOW(), FLOW_STEP()...
#include "flow/eqos_flow.h"
// Phan TEST: struct test_cfg, test_build_frame(), test_send_once(), test_poll_rx()
#include "test/test_loopback.h"
// printf (BSP chuyen ky tu ra UART COM1)
#include <stdio.h>

/* Ethernet sequence derived from the supplied GPL-2.0 EQoS driver.
 * Original U-Boot code: Copyright (c) 2016, NVIDIA CORPORATION.
 * GPIO/reset, MAC/MTL/DMA, rings, TX and RX are split into one folder per
 * functional block under Core/Src (BAN DO FILE below); main() calls them in order. */
/*
 * EQoS bring-up map (STM32H755 instance of Synopsys DesignWare Ethernet QoS):
 *
 *   TX: application frame -> RAM TX buffer -> DMA -> MTL TX FIFO -> MAC -> RMII
 *   RX: RMII -> MAC -> MTL RX FIFO -> DMA -> RAM RX buffer -> application
 *
 * The MTL base here selects queue 0, not the global MTL window at offset 0xC00.
 * eqos_hw.h includes generic EQoS definitions; not every generic feature is
 * implemented by the H755. This file uses its single 10/100-Mbit MAC and AHB DMA.
 *
 * CURRENT EXPERIMENTAL SEQUENCE -- these comments describe the code as saved:
 * - CFG.mac.loopback is true; the LAN8742 initialization/link wait is disabled,
 *   so CFG.link fixes the speed/duplex the PHY would normally report
 *   (100 Mbit/s, full duplex: RM0399 61.5.8 allows loopback only in full duplex).
 * - Bring-up order follows RM0399 61.9: reset -> configure DMA while stopped ->
 *   prepare rings and publish ring addresses -> MTL -> MAC -> start DMA (ST/SR)
 *   -> start MAC (TE/RE). The DMA takes DLAR when ST/SR go 0 -> 1 (tr.3083).
 *
 */

/* ============================================================================
 * BAN DO FILE - moi buoc [n] (so trong log FLOW) nam o dau.
 * Moi khoi mot thu muc con cua Core/Src (duong dan duoi day tinh tu Core/Src):
 *   [1]  board_config()                main.c
 *   [2]  print_config()                main.c
 *        stm32_precheck()              port/eqos_port.c
 *   [3]  stm32_config_eth()            port/eqos_port.c
 *   [4]  eqos_reset()                  core/eqos.c
 *   [5]  eqos_config_dma()             dma/eqos_dma.c
 *   [6]  eqos_setup_rings()            desc/eqos_desc.c
 *   [7]  eqos_config_mtl()             mtl/eqos_mtl.c
 *   [8]  eqos_config_mac()             mac/eqos_mac.c
 *        eqos_start_dma()              dma/eqos_dma.c
 *        eqos_start_mac()              mac/eqos_mac.c
 *   [9]  test_build_frame()            test/test_loopback.c
 *   [10] test_send_once()              test/test_loopback.c -> eqos_send()  desc/eqos_desc.c
 *   [11] test_poll_rx()                test/test_loopback.c -> eqos_recv(), eqos_recv_done()  desc/eqos_desc.c
 *   Log FLOW (muc log: FLOW_LOG)       flow/eqos_flow.h, flow/eqos_flow.c
 *   Kieu dung chung + khai bao ham     core/eqos.h
 *   Dia chi + bit thanh ghi            core/eqos_hw.h
 * ============================================================================ */

// Struct cau hinh UART cua BSP (CubeMX sinh), dien o buoc 1
COM_InitTypeDef BspCOMInit;
/* The button trigger interrupt sets the send_requested flag. */
// Co yeu cau gui: ngat nut nhan dat = 1, vong lap chinh doc. volatile vi bi ISR thay doi
static volatile uint8_t send_requested;

/* ============================================================================
 * CAU HINH (CFG) - moi gia tri cau hinh cua test nam o day, chia theo PART.
 * Kieu cua tung phan: board_cfg o port/eqos_port.h; dma_cfg, mtl_cfg, mac_cfg,
 * link_cfg o core/eqos.h; test_cfg o test/test_loopback.h. Moi ham cau hinh chi nhan
 * phan CFG cua part minh (vi du eqos_config_dma() nhan &CFG.dma).
 * Gia tri thanh ghi "mong doi" ghi trong log (vi du ETH_MACCR 0x00307003)
 * ung voi CFG duoi day; doi CFG thi cac gia tri mong doi do cung doi theo.
 * So descriptor va kich thuoc buffer van la macro trong eqos.h
 * (EQOS_TX_DESC_COUNT, EQOS_RX_DESC_COUNT, EQOS_BUF_SIZE) vi cac mang
 * tx_desc/rx_desc/tx_buf/rx_buf can biet kich thuoc luc bien dich.
 * ============================================================================ */
// Ca bang cau hinh
struct app_cfg {
  struct board_cfg board;
  struct dma_cfg   dma;
  struct mtl_cfg   mtl;
  struct mac_cfg   mac;
  struct link_cfg  link;
  struct test_cfg  test;
};

// Payload 2286 byte -> frame = 14 byte header + 2286 = 2300 byte (chua tinh FCS; tren day 2304 byte).
// Khung > 2048 byte nen can: CFG.mac.je = true (giant 9018, watchdog/jabber 10240), CFG.mtl
// tsf = rsf = false (FIFO TX/RX chi 2 KB < 2304), EQOS_BUF_SIZE >= 2304 (eqos.h), TEST_FRAME_MAX >= 2300.
// Gia tri duoc dien luc chay trong main(): 01 02 03 ... FF 00 01 ... (tang dan, de thay byte bi lech).
static uint8_t TEST_PAYLOAD[2286];
_Static_assert(14U + sizeof(TEST_PAYLOAD) == 2300U, "test frame must be 2300 bytes");
// (Payload 66 byte cu: frame 80 byte, tang dan 0x01..0x42, da tat)
// static const uint8_t TEST_PAYLOAD[] = {
//   0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
//   0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
//   0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30,
//   0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40,
//   0x41, 0x42,
// };
// (Payload 8 byte cu, da tat)
// static const uint8_t TEST_PAYLOAD[] = {0x17, 0x17, 0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
// (Payload chu cu, da tat)
// static const uint8_t TEST_PAYLOAD[] = "Hello world hehe lmao";

static const struct app_cfg CFG = {
  .board = {
    .uart_baud = 115200,
    .rmii_pins = {
      {GPIOA, 1}, {GPIOA, 2}, {GPIOA, 7}, /* REF_CLK, MDIO, CRS_DV */
      {GPIOC, 1}, {GPIOC, 4}, {GPIOC, 5}, /* MDC, RXD0, RXD1 */
      {GPIOB, 13}, {GPIOG, 11}, {GPIOG, 13} /* TXD1, TX_EN, TXD0 */
    },
  },
  // TXPBL = RXPBL = 8, PBLX8 = 1 -> burst toi da 8 x 8 = 64 beat = 256 byte
  .dma = { .osf = true, .txpbl = 8, .rxpbl = 8, .pblx8 = true, .aal = true, .fb = true },
  // TSF = RSF = false: threshold mode, vi frame 2304 byte lon hon FIFO TX/RX 2 KB (RM0399 tr.2933)
  .mtl = { .tsf = false, .rsf = false, .dis_tcp_ef = true },
  .mac = {
    // Dia chi MAC cua board; 00:80:E1 la tien to (OUI) cua STMicroelectronics
    .addr = {0x00, 0x80, 0xE1, 0x00, 0x00, 0x00},
    // PR=true requests promiscuous reception, useful with the test's peer MAC.
    .promiscuous = true,
    // LM=true requests MAC-internal loopback when MACCR is written.
    .loopback = true,
    .cst = true,
    .acs = true,
    // JE=true: giant 1518 -> 9018, watchdog RX + jabber TX 2048 -> 10240 (can cho frame 2300 byte)
    .je = true,
    // Test watchdog RX (frame 2300 byte = 2304 byte tren day). JE giu true de jabber TX (10240)
    // khong cat frame truoc; chi watchdog RX duoc doi nguong qua ETH_MACWTR:
    //   wd=false, pwe=true,  wto=0x0 -> nguong 2048 < 2304 -> RX bi CAT: RDES3 RWT=1, ES=1, rx_errors++
    //   wd=false, pwe=true,  wto=0x1 -> nguong 3072 > 2304 -> RX binh thuong 2300 byte
    //   wd=true  (pwe/wto bi bo qua) -> TAT watchdog         -> RX binh thuong 2300 byte
    //   wd=false, pwe=false          -> nguong theo JE 10240 -> RX binh thuong 2300 byte
    .wd = false,
    .pwe = true,
    .wto = 0x0,
  },
  // Loopback phai chay o full duplex: RM0399 61.5.8 (tr.2998) - da sua review #2 (code cu chay half
  // duplex, 10 Mbit/s vi eth.duplex_full/eth.speed = 0 khi phan PHY bi bo).
  // 100 Mbit/s: giong toc do link that LAN8742 <-> switch se dung.
  .link = { .speed = EQOS_SPEED_100, .full_duplex = true },
  .test = {
    // Dia chi dich (khac dia chi board -> can promiscuous)
    .dest = {0x6C, 0x1F, 0xF7, 0xCA, 0x2B, 0xFD},
    // 0x88B5: EtherType danh cho thu nghiem, khong co header IP phia sau
    .ethertype = 0x88B5,
    .payload = TEST_PAYLOAD,
    .payload_len = sizeof(TEST_PAYLOAD),
  },
};
// Frame = 14 byte header + payload phai vua bo dem frame[TEST_FRAME_MAX] cua test/test_loopback.c
_Static_assert(14U + sizeof(TEST_PAYLOAD) <= TEST_FRAME_MAX, "test payload too large for frame[]");

// Ham cua file nay (dinh nghia sau main())
static void board_config(const struct board_cfg *c);            // [1] HAL, clock 224 MHz, UART COM1, nut USER
static void print_config(void);                                 // [2] in bang CFG
void SystemClock_Config(void);                                  //     (CubeMX) HSI 64 MHz -> PLL1 -> 224 MHz

// Ham chinh. Startup da chep .data, xoa .bss truoc khi vao day. Khong bao gio return (co while(1)).
// Moi dong goi = mot buoc trong log FLOW; file chua tung buoc: xem BAN DO FILE o tren.
int main(void)
{
  /* PART 1 - BOARD (file nay) */
  board_config(&CFG.board);                  // [1] HAL, clock 224 MHz, UART COM1, nut USER

  /* PART 2 - STM32 (port/eqos_port.c) */
  print_config();                            // [2] in bang CFG (file nay)
  if (stm32_precheck() != EQOS_OK)           // [2] D-cache phai tat
    Error_Handler();
  stm32_config_eth(&CFG.board);              // [3] DWT, 9 chan RMII, SYSCFG = RMII, clock + reset ETH, tat IRQ ETH

  /* PART 3 - EQoS (core/, dma/, desc/, mtl/, mac/ - RM0399 61.9, tr.3031-3033) */
  if (eqos_reset() != EQOS_OK)               // [4] core/eqos.c: MACVR, SWR + cho, MAC1USTCR
    Error_Handler();
  eqos_config_dma(&CFG.dma);                 // [5] dma/eqos_dma.c: DMACTXCR, DMACRXCR, DMACCR, DMASBMR, DMACIER (chua ST/SR)
  eqos_setup_rings();                        // [6] desc/eqos_desc.c: descriptor + DLAR / RLR / tail (DMA van dang dung)
  eqos_config_mtl(&CFG.mtl);                 // [7] mtl/eqos_mtl.c: MTLTXQOMR, MTLRXQOMR
  eqos_config_mac(&CFG.mac, &CFG.link);      // [8] mac/eqos_mac.c: MACPFR, MACA0HR/LR, MACCR (chua TE/RE)
  // Khoi dong theo RM0399: DMA truoc (61.9.1 buoc 9, tr.3032), bo thu MAC sau (61.9.3, tr.3032).
  // DMA doc DLAR dung luc ST/SR chuyen 0 -> 1 (tr.3083) nen ring phai co truoc (da sua review #1).
  eqos_start_dma();                          // [8] dma/eqos_dma.c: ST = 1, SR = 1
  eqos_start_mac();                          // [8] mac/eqos_mac.c: TE = 1, RE = 1

  /* PART 4 - TEST (test/test_loopback.c) */
  // Dien payload 2286 byte tang dan 01 02 ... FF 00 01 ... (phai truoc test_build_frame vi ham do chep payload)
  for (uint32_t i = 0; i < sizeof(TEST_PAYLOAD); ++i)
    TEST_PAYLOAD[i] = (uint8_t)(i + 1U);
  test_build_frame(&CFG.test, CFG.mac.addr); // [9] dung frame thu nghiem
  // Dat co de vong lap tu gui frame dau tien, khong can nhan nut
  send_requested = 1U; /* Startup send uses the same TX path (test_send_once) as USER. */
  printf("Ethernet ready\r\n");
  // [FLOW]
  FLOW("[10/11] Vao vong lap chinh: TX khi co yeu cau (luc khoi dong / nut USER), RX kiem tra lien tuc\r\n");

  // Vong lap vo han: moi vong = TX (neu co co) + RX (luon kiem tra)
  while (1) {
    // Co yeu cau gui (luc khoi dong hoac nut nhan)
    if (send_requested != 0U) {
      // Xoa co ngay (neu ngat den dung giua dong tren va dong nay thi mat 1 lan nhan - review #5)
      send_requested = 0U;
      test_send_once();                      // [10] gui 1 frame, in ket qua
    }
    test_poll_rx();                          // [11] neu co frame ve: in, so sanh, tra descriptor cho DMA
  }
}

/* ============================================================================
 * PART 1 - BOARD (Nucleo-H755ZI-Q)
 * Clock he thong, UART qua ST-LINK, nut USER.
 * ============================================================================ */

/* [1] Existing system clock, UART and button.
 * UART is initialized before EQoS so failures are visible.
 * SystemClock_Config currently selects 224 MHz CPU/HCLK from the 64 MHz HSI. */
static void board_config(const struct board_cfg *c)
{
  // Khoi tao HAL: SysTick tick 1 ms (cho HAL_Delay/timeout), nhom uu tien ngat
  HAL_Init();
  // Bat PLL, chuyen CPU/HCLK sang 224 MHz (xem SystemClock_Config ngay duoi)
  SystemClock_Config();
  // Bat clock GPIOC (nut USER o PC13)
  __HAL_RCC_GPIOC_CLK_ENABLE();
  // Bat clock GPIOA (PA1 REF_CLK, PA2 MDIO, PA7 CRS_DV)
  __HAL_RCC_GPIOA_CLK_ENABLE();
  // Bat clock GPIOB (PB13 TXD1)
  __HAL_RCC_GPIOB_CLK_ENABLE();
  // Bat clock GPIOG (PG11 TX_EN, PG13 TXD0). Buoc 3 bat lai lan nua bang thanh ghi (thua nhung vo hai)
  __HAL_RCC_GPIOG_CLK_ENABLE();
  // Nut USER -> ngat EXTI; khi nhan, BSP goi BSP_PB_Callback() (cuoi file nay)
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);
  // UART: CFG.board.uart_baud (115200) baud, 8 bit du lieu, 1 stop bit, khong parity, khong flow control
  BspCOMInit.BaudRate = c->uart_baud;
  BspCOMInit.WordLength = COM_WORDLENGTH_8B;
  BspCOMInit.StopBits = COM_STOPBITS_1;
  BspCOMInit.Parity = COM_PARITY_NONE;
  BspCOMInit.HwFlowCtl = COM_HWCONTROL_NONE;
  // Khoi tao COM1 (UART qua ST-LINK -> cong COM ao tren PC); loi -> treo
  if (BSP_COM_Init(COM1, &BspCOMInit) != BSP_ERROR_NONE)
    Error_Handler();
  // Tu day printf moi in duoc ra UART
  printf("\n ============ MAC Loopback test ============ \n");
  printf("\n ============ New test ============ \n");
  printf("Inline Ethernet sequence starting\r\n");
  // [FLOW2] Tieu de buoc 1 kem thoi gian (ms tu luc khoi dong)
  FLOW_STEP("[1]", "Khoi dong he thong: clock, UART, nut USER");
  // [FLOW] Xac nhan clock: ca hai gia tri phai = 224000000
  FLOW("[1] HAL + SystemClock_Config xong: SystemCoreClock = %lu Hz, HCLK = %lu Hz\r\n",
       (unsigned long)SystemCoreClock, (unsigned long)HAL_RCC_GetHCLKFreq());
  FLOW("[1] UART COM1 %lu-8N1 OK, nut USER = ngat EXTI (nhan de gui them 1 frame)\r\n",
       (unsigned long)c->uart_baud);
  // [FLOW2] ID chip, phien ban loi Cortex-M7, tat ca clock bus, trang thai I/D-cache
  flow_chip_info();
}

/* [2] In bang cau hinh CFG. Phan kiem tra bo nho DMA / D-cache cua buoc nay
 * nam o stm32_precheck() (port/eqos_port.c), goi ngay sau ham nay. */
static void print_config(void)
{
  // [FLOW2]
  FLOW_STEP("[2]", "In cau hinh (CFG), kiem tra D-cache");
  // [FLOW] In cau hinh phan mem (bang CFG o dau file). csr_clk = HCLK, dung o buoc 4 de tinh MAC1USTCR
  FLOW("[2] CFG: base=0x%08lX csr_clk=%lu Hz MAC=%02X:%02X:%02X:%02X:%02X:%02X promiscuous=%d loopback=%d\r\n",
       (unsigned long)EQOS_BASE_ADDR, (unsigned long)HAL_RCC_GetHCLKFreq(),
       CFG.mac.addr[0], CFG.mac.addr[1], CFG.mac.addr[2],
       CFG.mac.addr[3], CFG.mac.addr[4], CFG.mac.addr[5],
       (int)CFG.mac.promiscuous, (int)CFG.mac.loopback);
  // [FLOW] Toc do/duplex dat cung trong CFG.link (PHY dang tat nen khong co auto-negotiation)
  FLOW("[2] CFG.link: %lu Mbit/s, %s duplex\r\n", (unsigned long)CFG.link.speed,
       CFG.link.full_duplex ? "full" : "half");
}

/* Clock arithmetic for the CURRENT constants: HSI 64 MHz / PLLM 4 = 16 MHz;
 * VCO = 16 * PLLN 28 = 448 MHz; PLLP 2 -> SYSCLK 224 MHz.
 * SYSCLKDivider and AHBCLKDivider are both /1, hence CPU/HCLK 224 MHz.
 * MAC CSR/MDC selection follows HCLK; the PHY supplies a separate RMII 50 MHz.
 * Recheck the MDIO CR range and DWT delays if these divisors are changed. */
// Ham do CubeMX sinh. Chay TRUOC khi UART khoi tao nen khong printf duoc trong ham nay
void SystemClock_Config(void)
{
  // Struct cau hinh bo dao dong + PLL, khoi tao toan 0
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  // Struct cau hinh bo chia clock cho CPU va cac bus
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  // Cap nguon loi: bo SMPS noi, cap truc tiep - phai khop mach nguon cua board (RM0399 Table 33)
  HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  // Dien ap loi muc Scale 2 (RM0399 7.6.2)
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  // Cho dien ap on dinh (co VOSRDY)
  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  // Dung bo dao dong noi HSI 64 MHz, bat, khong chia, hieu chinh mac dinh
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  // Bat PLL1, nguon vao = HSI
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  // 64 / 4 = 16 MHz vao PLL
  RCC_OscInitStruct.PLL.PLLM = 4;
  // 16 x 28 = 448 MHz (VCO)
  RCC_OscInitStruct.PLL.PLLN = 28;
  // 448 / 2 = 224 MHz -> SYSCLK
  RCC_OscInitStruct.PLL.PLLP = 2;
  // 448 / 5 = 89.6 MHz (nhanh Q, test nay khong dung)
  RCC_OscInitStruct.PLL.PLLQ = 5;
  // 448 / 2 = 224 MHz (nhanh R, khong dung)
  RCC_OscInitStruct.PLL.PLLR = 2;
  // Dai tan dau vao PLL 8-16 MHz (dang la 16 MHz)
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  // Dai VCO rong (448 MHz vuot dai VCO hep)
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  // Khong dung phan le (fractional)
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  // Ap dung; loi (vi du PLL khong khoa duoc) -> treo
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  // Cac clock se cau hinh: HCLK, SYSCLK, PCLK1 (APB1), PCLK2 (APB2), D3PCLK1 (APB4), D1PCLK1 (APB3)
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  // SYSCLK lay tu PLL1 nhanh P = 224 MHz
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  // /1 -> CPU = 224 MHz
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  // /1 -> HCLK = 224 MHz (chinh la clock CSR cua Ethernet)
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
  // APB3 = 112 MHz, APB1 = 56 MHz, APB2 = 112 MHz, APB4 = 112 MHz
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  // Ap dung voi 3 wait state cho flash (RM0399 Table 16); loi -> treo
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* EXTI only latches a request; the blocking descriptor poll and UART output
 * execute in main. Multiple edges before the next poll can coalesce, and no
 * debounce/counting is implemented. A button press is not an Ethernet IRQ. */
// BSP khai bao ham nay la weak; dinh nghia lai o day thi ban nay duoc dung.
// Duoc goi tu ngat EXTI khi nhan nut -> KHONG printf o day (dang trong ngat)
void BSP_PB_Callback(Button_TypeDef Button)
{
  // Chi phan ung voi nut USER -> dat co roi thoat ngay
  if (Button == BUTTON_USER)
    send_requested = 1U; /* No blocking TX or UART output in the interrupt. */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
// Goi khi co loi khong go duoc: tat ngat va treo. Debug: Pause roi xem Call Stack de biet ai goi
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  // [FLOW] Bao loi truoc khi tat ngat (neu UART chua khoi tao duoc thi dong nay khong in gi)
  FLOW("\r\n[!] Error_Handler() -> chuong trinh dung tai day\r\n");
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
// Chi bien dich khi bat USE_FULL_ASSERT: HAL goi assert_failed khi nhan tham so sai
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
