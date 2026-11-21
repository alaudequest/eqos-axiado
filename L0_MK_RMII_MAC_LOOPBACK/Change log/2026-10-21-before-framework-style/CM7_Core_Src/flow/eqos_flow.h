/* SPDX-License-Identifier: GPL-2.0 */
/*
 * eqos_flow.h - log FLOW: in tung buoc khoi tao / gui / nhan ra UART.
 * Dung chung cho main.c, test/test_loopback.c va cac file eqos_*.c.
 * Than cac ham nam o eqos_flow.c (cung thu muc); bang giai ma bit cua tung
 * thanh ghi nam ngay trong file dung no (vi du F_MACCR o mac/eqos_mac.c).
 */
#ifndef EQOS_FLOW_H
#define EQOS_FLOW_H

#include <stdint.h>
#include <stdio.h>

/* ================================ FLOW LOG ==================================
 * In ra UART tung buoc cua chuong trinh de theo doi code flow khi chay.
 * Moi dong log bat dau bang [so buoc], khop voi so buoc trong
 * docs/MAC_LOOPBACK_code_flow.html (tai lieu 1). Moi buoc nam trong mot ham
 * rieng (xem BAN DO FILE o dau main.c). Doi muc log o dong #define FLOW_LOG
 * ben duoi, hoac them -DFLOW_LOG=n vao tuy chon bien dich.
 *   FLOW_LOG = 0 : tat toan bo log FLOW (cac printf goc cua chuong trinh van giu)
 *   FLOW_LOG = 1 : log co ban: moi buoc 1-2 dong + doc lai thanh ghi (hex)
 *   FLOW_LOG = 2 : log CHI TIET, them:
 *                  - tieu de tung buoc kem thoi gian (ms tu luc khoi dong)
 *                  - thong tin chip (ID, revision, CPUID) va tat ca clock bus
 *                  - gia tri thanh ghi TRUOC khi ghi va GIAI MA TUNG BIT sau khi ghi
 *                  - trang thai may DMA (Stopped/Running/Suspended), FIFO MTL, MAC
 *                  - toan bo ring descriptor (ai dang giu, head, tail, DMA dang o dau)
 *                  - giai ma TDES3/RDES3 tung bit, bo dem MMC
 *                  - hex dump frame co chu thich tung truong
 *                  - thoi gian SWR, thoi gian TX, thoi gian loopback do bang DWT (chinh xac ~5 ns)
 * Luu y: UART 115200 baud mat ~87 us cho moi ky tu -> log cang chi tiet khoi dong cang cham
 * (muc 2 in vai nghin ky tu, mat khoang 0.5-1 giay). KHONG dat FLOW ben trong
 * vong poll TX va KHONG dat trong ham ngat (BSP_PB_Callback). */
#ifndef FLOW_LOG
#define FLOW_LOG 2
#endif
#if FLOW_LOG
#define FLOW(...) printf(__VA_ARGS__)
#else
#define FLOW(...) ((void)0)
#endif
// FLOW2: chi in khi FLOW_LOG >= 2 (log chi tiet)
#if FLOW_LOG >= 2
#define FLOW2(...) printf(__VA_ARGS__)
#else
#define FLOW2(...) ((void)0)
#endif

/* In ten thanh ghi, dia chi tuyet doi (base + offset) va gia tri DOC LAI tu phan cung.
 * Doc lai sau khi ghi giup thay phan cung that su giu gia tri gi
 * (vi du bit tu xoa nhu SWR trong ETH_DMAMR se doc ra 0). */
void flow_reg(const char *name, uintptr_t base, uint32_t offset);

#if FLOW_LOG >= 2
// Mo ta 1 truong bit: ten, bit cao, bit thap
typedef struct { const char *name; uint8_t hi; uint8_t lo; } flow_field_t;
// So phan tu cua mot bang truong bit (chi dung trong file dinh nghia bang do)
#define FLOW_N(a) (sizeof(a) / sizeof((a)[0]))

void flow_fields(const flow_field_t *f, uint32_t n, uint32_t v);   // giai ma tung truong bit cua v
void flow_reg_f(const char *name, uintptr_t base, uint32_t offset,
                const flow_field_t *f, uint32_t n);                // flow_reg + giai ma tung bit
void flow_step(const char *tag, const char *title);                // tieu de buoc + thoi gian
void flow_print_cycles(uint32_t cyc);                              // chu ky DWT -> "x.yy us"
void flow_dma_state(void);                                         // may trang thai DMA, ETH_DMACSR
void flow_mtl_state(void);                                         // 2 FIFO cua MTL
void flow_mac_state(void);                                         // bo phat/thu cua MAC
void flow_mmc(void);                                               // bo dem MMC
void flow_rings(const char *why);                                  // ca 2 ring descriptor
void flow_frame_dump(const char *title, const uint8_t *p, uint32_t len, uint32_t payload_len);
void flow_chip_info(void);                                         // ID chip, clock bus, cache

#define FLOW_STEP(tag, title)  flow_step((tag), (title))
// flow_reg2: doc + giai ma thanh ghi, CHI o muc 2 (muc 1 khong in)
#define flow_reg2(name, base, off, f, n) flow_reg_f((name), (base), (off), (f), (n))
#else
// FLOW_LOG < 2: moi ham chi tiet bien thanh lenh rong (khong ton thoi gian, khong ton flash)
#define flow_reg_f(name, base, off, f, n) flow_reg((name), (base), (off))
#define flow_fields(f, n, v)              ((void)0)
#define flow_dma_state()                  ((void)0)
#define flow_mtl_state()                  ((void)0)
#define flow_mac_state()                  ((void)0)
#define flow_mmc()                        ((void)0)
#define flow_rings(why)                   ((void)0)
#define flow_frame_dump(t, p, len, pl)    ((void)0)
#define flow_chip_info()                  ((void)0)
#define FLOW_STEP(tag, title)             ((void)0)
#define flow_reg2(name, base, off, f, n)  ((void)0)
#endif

#endif /* EQOS_FLOW_H */
