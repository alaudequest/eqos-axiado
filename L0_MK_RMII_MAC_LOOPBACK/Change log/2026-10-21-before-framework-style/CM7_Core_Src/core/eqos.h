/* SPDX-License-Identifier: GPL-2.0 */
/* Shared types and API of the EQoS driver used by the MAC loopback test.
 * One folder per block under Core/Src (RM0399 61.9): core/ (this header,
 * eqos_hw.h, eqos.c = state + reset), dma/, desc/ (rings, TX/RX), mtl/, mac/.
 * STM32 glue: port/. Debug log: flow/. Call order: main() in Core/Src/main.c.
 */

#ifndef EQOS_H
#define EQOS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* STM32H755 bring-up: 32-bit bus, D-cache disabled. */
#if defined(STM32H755xx)
#define EQOS_STM32H7 1
#define EQOS_AXI_WIDTH 4
#define EQOS_CACHELINE 32
#define EQOS_BUF_CACHED 0
#endif

/* ================================================================== */
/* Cau hinh bien dich - chinh o day hoac dinh nghia tren command line  */
/* ================================================================== */

/* So descriptor moi ring. Phai la luy thua cua 2. */
#ifndef EQOS_TX_DESC_COUNT
#define EQOS_TX_DESC_COUNT      4
#endif
#ifndef EQOS_RX_DESC_COUNT
#define EQOS_RX_DESC_COUNT      8
#endif

/* Kich thuoc buffer RX. Phai la boi so cua do rong bus AXI (4/8/16 byte).
 * 2304 = frame test 2300 byte + 4 byte FCS, boi cua 32 (eqos_desc.c kiem tra % 32 va <= 0x3FFF).
 * (Gia tri cu 1600 = 1522, frame toi da co VLAN, lam tron len boi cua 64.) */
#ifndef EQOS_BUF_SIZE
#define EQOS_BUF_SIZE           2304
#endif

/* Do rong bus AXI cua khoi EQoS tren SoC cua ban, tinh bang BYTE.
 * 4 = 32-bit, 8 = 64-bit, 16 = 128-bit. Tra TRM. */
#ifndef EQOS_AXI_WIDTH
#define EQOS_AXI_WIDTH          8
#endif

/* Kich thuoc cache line cua CPU. Chi dung khi EQOS_DESC_CACHED = 1. */
#ifndef EQOS_CACHELINE
#define EQOS_CACHELINE          64
#endif

/*
 * 0 = descriptor ring nam trong bo nho KHONG cache (khuyen dung).
 *     Don gian nhat: khong can flush/invalidate descriptor, DSL = 0.
 *     Ban phai map section .eqos_nocache la Device hoac Normal-NonCacheable
 *     trong MMU/MPU (xem README).
 *
 * 1 = descriptor ring nam trong bo nho co cache.
 *     Driver se goi eqos_dcache_clean/inval va gian descriptor ra dung
 *     mot cache line. CHI dung duoc khi (EQOS_CACHELINE-16)/EQOS_AXI_WIDTH <= 7,
 *     vi truong DSL cua HW chi rong 3 bit.
 */
#ifndef EQOS_DESC_CACHED
#define EQOS_DESC_CACHED        0
#endif

/* Buffer du lieu co nam trong vung co cache khong.
 * Neu ban dat buffer o vung khong cache thi set = 0 de bo qua cache op. */
#ifndef EQOS_BUF_CACHED
#define EQOS_BUF_CACHED         1
#endif

/* ================================================================== */
/* Kieu du lieu                                                        */
/* ================================================================== */

enum eqos_iface {
    EQOS_IFACE_MII = 0,
    EQOS_IFACE_GMII,
    EQOS_IFACE_RMII,
    EQOS_IFACE_RGMII,       /* khong delay */
    EQOS_IFACE_RGMII_ID,    /* internal delay ca TX lan RX */
    EQOS_IFACE_RGMII_TXID,
    EQOS_IFACE_RGMII_RXID,
};

enum eqos_speed {
    EQOS_SPEED_10   = 10,
    EQOS_SPEED_100  = 100,
    EQOS_SPEED_1000 = 1000,
};

/* Che do RXQ0: DCB cho ethernet thong thuong, AV cho 802.1Qav.
 * U-Boot dung DCB cho tat ca tru STM32MP15 (dung AV). */
enum eqos_rxq_mode {
    EQOS_RXQ_MODE_DCB = 2,
    EQOS_RXQ_MODE_AV  = 1,
};

#define EQOS_PHY_ADDR_AUTO      (-1)    /* quet 0..31 tim PHY */

struct eqos_dev;

struct eqos_cfg {
    uintptr_t           base;           /* dia chi base cua khoi EQoS */
    uint32_t            csr_clk_hz;     /* clock CSR (AHB/APB) cap cho MAC */
    int                 phy_addr;       /* 0..31 hoac EQOS_PHY_ADDR_AUTO */
    enum eqos_iface     phy_iface;
    uint8_t             mac_addr[6];

    /* --- tuy chon, de 0 se dung mac dinh --- */
    enum eqos_rxq_mode  rxq_mode;       /* 0 -> DCB */
    uint32_t            tx_fifo_sz;     /* 0 -> doc tu HW_FEATURE1 */
    uint32_t            rx_fifo_sz;     /* 0 -> doc tu HW_FEATURE1 */
    uint32_t            max_speed;      /* 0 -> khong gioi han (1000) */
    /* true  = nhan MOI frame (giong U-Boot, tien khi debug/sniff)
     * false = HW loc theo mac_addr + broadcast/multicast (mac dinh) */
    bool                promiscuous;
    bool                loopback;       /* MAC loopback, de test khong can PHY */
    uint32_t            swr_timeout_us; /* 0 -> 100000 */
    uint32_t            aneg_timeout_ms;/* 0 -> 5000 */
    
};

struct eqos_stats {
    uint32_t tx_packets;
    uint32_t tx_errors;
    uint32_t tx_timeouts;
    uint32_t rx_packets;
    uint32_t rx_errors;      /* ES bit set trong write-back descriptor */
    uint32_t rx_dropped;     /* packet bi cat (khong co FD|LD) */
};

struct eqos_dev {
    struct eqos_cfg  cfg;
    uintptr_t        base;

    uint32_t         desc_stride;   /* byte giua 2 descriptor lien tiep */
    uint32_t         mdio_cr;       /* gia tri truong CR cho MDIO */

    int              phy_addr;
    enum eqos_speed  speed;
    bool             duplex_full;
    bool             link_up;

    uint32_t         tx_head;       /* descriptor tiep theo de ghi */
    uint32_t         rx_head;       /* descriptor tiep theo de doc */

    bool             started;
    struct eqos_stats stats;
};

/* ================================================================== */
/* API                                                                 */
/* ================================================================== */

/* Ma loi (< 0) */
#define EQOS_OK              0
#define EQOS_EINVAL         (-1)
#define EQOS_ETIMEDOUT      (-2)
#define EQOS_ENODEV         (-3)   /* khong tim thay PHY */
#define EQOS_ENOLINK        (-4)   /* PHY khong len link */
#define EQOS_EAGAIN         (-5)   /* chua co packet */
#define EQOS_EIO            (-6)

/* Microsecond delay (DWT), implemented in port/eqos_port.c. */
void eqos_udelay(uint32_t us);

/* ================================================================== */
/* Descriptor (RM0399 61.10)                                           */
/* ================================================================== */
/*
 * Normal descriptors occupy four 32-bit words (16 bytes). Descriptors have
 * 2 states: DMA read format (CPU prepared for DMA) and DMA write-back
 * format (hardware reports completion);
 *
 * The application submits:
 *   TX DES0 = buffer_1 address; DES1 = 0 (no second buffer);
 *      DES2 = B1L, buffer byte count; DES3 = OWN | FD | LD | total frame length.
 *   RX DES0 = buffer_1 address; DES1/DES2 = 0 (unused);
 *      DES3 = OWN | BUF1V, making that buffer available for DMA reception.
 * After DMA completed Tx or Rx, DES3 carries OWN=0 and write-back flags/length/error status.
 *
 * OWN=1: DMA owns the descriptor and associated buffer; CPU must not rewrite it.
 * OWN=0: CPU can inspect completion and later rebuild/reuse the descriptor.
 * volatile prevents compiler-cached status reads; it is NOT cache coherency.
 */
// Mot descriptor = 4 word 32-bit lien nhau = 16 byte, dung layout phan cung (RM0399 Figure 853/856)
struct eqos_desc {
  // volatile: moi lan doc des3 la doc RAM that, vi DMA co the doi gia tri bat cu luc nao
  volatile uint32_t des0, des1, des2, des3;
};

/* ================================================================== */
/* Cau hinh tung khoi - gia tri cu the nam o bang CFG trong main.c     */
/* ================================================================== */
// PART 3 - DMA (RM0399 61.9.1, tr.3031)
struct dma_cfg {
  bool    osf;          // ETH_DMACTXCR.OSF: xu ly goi thu hai khi goi truoc chua xong
  uint8_t txpbl;        // ETH_DMACTXCR.TXPBL: burst toi da (don vi beat) khi DMA doc RAM cho TX
  uint8_t rxpbl;        // ETH_DMACRXCR.RXPBL: burst toi da (don vi beat) khi DMA ghi RAM cho RX
  bool    pblx8;        // ETH_DMACCR.PBLX8: nhan TXPBL va RXPBL len 8
  bool    aal;          // ETH_DMASBMR.AAL: burst can theo dia chi
  bool    fb;           // ETH_DMASBMR.FB: burst co dinh
};
// PART 3 - MTL (RM0399 61.9.2, tr.3032)
struct mtl_cfg {
  bool tsf;             // ETH_MTLTXQOMR.TSF: TX store-and-forward
  bool rsf;             // ETH_MTLRXQOMR.RSF: RX store-and-forward
  bool dis_tcp_ef;      // ETH_MTLRXQOMR.DIS_TCP_EF: khong bo goi chi vi loi checksum TCP/IP
};
// PART 3 - MAC (RM0399 61.9.3, tr.3032)
struct mac_cfg {
  uint8_t addr[6];      // dia chi MAC cua board -> ETH_MACA0HR/ETH_MACA0LR
  bool promiscuous;     // ETH_MACPFR.PR: nhan frame bat ke dia chi dich
  bool loopback;        // ETH_MACCR.LM: MAC tu vong TX sang RX
  bool cst;             // ETH_MACCR.CST: bo FCS o frame kieu Type
  bool acs;             // ETH_MACCR.ACS: bo pad/FCS o frame kieu Length
  bool je;              // ETH_MACCR.JE: jumbo -> giant 9018, watchdog/jabber 10240
  bool wd;              // ETH_MACCR.WD: true = TAT watchdog RX (nhan toi 16383 byte)
  bool pwe;             // ETH_MACWTR.PWE: true = nguong watchdog lay tu wto (chi khi wd = false)
  uint8_t wto;          // ETH_MACWTR.WTO: 0 = 2 KB, 1 = 3 KB .. 0xD = 15 KB, 0xE = 16383 byte (0xF cam dung)
};
// PART 3 - LINK: toc do/duplex. Binh thuong PHY bao sau auto-negotiation; PHY dang tat nen dat cung o day
struct link_cfg {
  enum eqos_speed speed;  // EQOS_SPEED_100 -> ETH_MACCR.FES = 1; gia tri khac -> FES = 0 (10 Mbit/s)
  bool full_duplex;       // ETH_MACCR.DM: true = full duplex (bat buoc khi loopback)
};

/* ================================================================== */
/* Trang thai dung chung                                               */
/* ================================================================== */
// "So ghi chep" phia phan mem: tx_head/rx_head, thong ke, co started (dinh nghia o eqos.c)
extern struct eqos_dev eth;
// 2 ring descriptor (dinh nghia o desc/eqos_desc.c, section .eqos_nocache trong RAM_D1)
extern struct eqos_desc tx_desc[EQOS_TX_DESC_COUNT];
extern struct eqos_desc rx_desc[EQOS_RX_DESC_COUNT];

/* ================================================================== */
/* API - theo thu tu main() goi; [n] = so buoc trong log FLOW           */
/* ================================================================== */
/* In the eqos_*.c files:
 * Register snapshots below are local read/modify/write values. Combine bits
 * before writing to avoid access bus many times, but continue fresh reads
 * for hardware status polling. OR-ing a multi-bit field is not equivalent
 * to replacing it unless the old field bits are known to be zero.
 */
int  eqos_reset(void);                                            // [4]  core/eqos.c: MACVR, SWR + cho, MAC1USTCR
void eqos_config_dma(const struct dma_cfg *c);                    // [5]  dma/eqos_dma.c: DMACTXCR, DMACRXCR, DMACCR, DMASBMR, DMACIER
void eqos_setup_rings(void);                                      // [6]  desc/eqos_desc.c: descriptor + DLAR / RLR / tail
void eqos_config_mtl(const struct mtl_cfg *c);                    // [7]  mtl/eqos_mtl.c: MTLTXQOMR, MTLRXQOMR
void eqos_config_mac(const struct mac_cfg *c, const struct link_cfg *l); // [8] mac/eqos_mac.c: MACPFR, MACA0HR/LR, MACCR
void eqos_start_dma(void);                                        // [8]  dma/eqos_dma.c: ST = 1, SR = 1 (sau khi da co ring)
void eqos_start_mac(void);                                        // [8]  mac/eqos_mac.c: TE = 1, RE = 1 (sau DMA)
int  eqos_send(const uint8_t *data, uint32_t len);                // [10] desc/eqos_desc.c: dua 1 frame cho DMA, cho gui xong
int  eqos_recv(uint8_t **data);                                   // [11] desc/eqos_desc.c: lay frame tai rx_head (neu co)
void eqos_recv_done(void);                                        // [11] desc/eqos_desc.c: tra descriptor RX lai cho DMA

#endif /* EQOS_H */
