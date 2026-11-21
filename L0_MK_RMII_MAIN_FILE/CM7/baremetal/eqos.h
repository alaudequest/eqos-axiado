/* SPDX-License-Identifier: GPL-2.0 */
/* Shared configuration, debug state and delay declaration.
 * The active STM32H755 Ethernet sequence is inline in main().
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
 * 1600 = 1522 (frame toi da co VLAN) lam tron len boi cua 64. */
#ifndef EQOS_BUF_SIZE
#define EQOS_BUF_SIZE           1600
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

/* Microsecond delay only; init/TX/RX are statements in main(). */
void eqos_udelay(uint32_t us);

#endif /* EQOS_H */
