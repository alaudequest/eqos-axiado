/* SPDX-License-Identifier: GPL-2.0 */
/*
 * eqos.h - Driver baremetal cho Synopsys DesignWare Ethernet QoS
 *
 * Viet lai tu driver U-Boot dwc_eth_qos.c, bo toan bo device model / clk
 * framework / malloc. Chi con:
 *   - truy cap thanh ghi truc tiep
 *   - descriptor ring cap phat tinh
 *   - polling (khong dung interrupt)
 *   - mot lop "port" nho ban phai tu hien thuc cho board cua minh
 *
 * Cach dung dien hinh:
 *
 *      static struct eqos_dev eth;
 *
 *      struct eqos_cfg cfg = {
 *          .base        = 0x30bf0000,     // base cua khoi EQoS
 *          .csr_clk_hz  = 250000000,      // clock CSR/AHB cap cho MAC
 *          .phy_addr    = 0,              // hoac EQOS_PHY_ADDR_AUTO
 *          .phy_iface   = EQOS_IFACE_RGMII_ID,
 *          .mac_addr    = { 0x00,0x11,0x22,0x33,0x44,0x55 },
 *      };
 *
 *      eqos_init(&eth, &cfg);      // reset + PHY + MAC/MTL/DMA + enable
 *      eqos_send(&eth, buf, len);
 *      len = eqos_recv(&eth, &pkt);
 *      if (len > 0) { ...; eqos_recv_done(&eth); }
 */

#ifndef EQOS_H
#define EQOS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

/*
 * Khoi tao day du: reset HW -> do PHY -> cau hinh MAC/MTL/DMA ->
 * dung descriptor ring -> bat TX/RX.
 * Tra EQOS_OK, hoac ma loi am. Blocking cho toi khi PHY len link.
 */
int eqos_init(struct eqos_dev *dev, const struct eqos_cfg *cfg);

/* Tat TX/RX, xa MTL queue. Sau do co the goi lai eqos_init(). */
void eqos_stop(struct eqos_dev *dev);

/*
 * Gui mot packet. Blocking cho toi khi HW nha quyen so huu descriptor.
 * len tinh bang byte, khong bao gom CRC (HW tu them).
 * Tra EQOS_OK / EQOS_ETIMEDOUT / EQOS_EIO / EQOS_EINVAL.
 */
int eqos_send(struct eqos_dev *dev, const void *packet, uint32_t len);

/*
 * Lay packet da nhan (khong copy - tra con tro vao buffer DMA).
 * Tra: > 0  = do dai packet, *packet tro toi du lieu
 *      EQOS_EAGAIN = chua co gi
 *      < 0  = loi (descriptor bao ES, packet bi bo)
 *
 * Sau khi xu ly xong PHAI goi eqos_recv_done() de tra buffer ve cho DMA.
 */
int eqos_recv(struct eqos_dev *dev, uint8_t **packet);

/* Tra descriptor/buffer vua dung ve cho DMA. */
void eqos_recv_done(struct eqos_dev *dev);

/* Doc lai trang thai link tu PHY va cap nhat MAC neu doi toc do.
 * Goi dinh ky neu ban muon xu ly cap rut/cam lai. */
int eqos_poll_link(struct eqos_dev *dev);

/* Doi MAC address luc dang chay. */
void eqos_set_mac_addr(struct eqos_dev *dev, const uint8_t mac[6]);

/* Truy cap MDIO clause 22 - huu ich khi debug PHY. */
int  eqos_mdio_read(struct eqos_dev *dev, int phy, int reg);   /* < 0 = loi */
int  eqos_mdio_write(struct eqos_dev *dev, int phy, int reg, uint16_t val);

/* In mot loat thanh ghi quan trong qua eqos_printf(). */
void eqos_dump_regs(struct eqos_dev *dev);

/* ================================================================== */
/* LOP PORT - ban phai hien thuc trong eqos_port.c                     */
/* ================================================================== */

/* Cho it nhat `us` micro giay. */
void eqos_udelay(uint32_t us);

/*
 * Bat clock, thoat reset, cau hinh pinmux/glue cua SoC (GRF cua Rockchip,
 * SYSCFG cua STM32, IOMUX cua i.MX...). Goi mot lan dau eqos_init(),
 * TRUOC khi cham vao bat ky thanh ghi EQoS nao.
 * Tra 0 neu OK.
 */
int eqos_board_init(struct eqos_dev *dev);

/*
 * Dat tan so TX clock theo toc do da thoa thuan. Goi sau khi PHY len link.
 * RGMII: 125MHz @1G, 25MHz @100M, 2.5MHz @10M
 * RMII : 50MHz  @100M, 5MHz @10M
 * Tra 0 neu OK. Neu clock tree cua ban tu lo (nhu STM32) thi return 0 luon.
 */
int eqos_board_set_tx_clk(struct eqos_dev *dev, enum eqos_speed speed);

/*
 * Quirk sau khi ghi DMA_MODE.SWR = 1, truoc khi cho bit do tu clear.
 * Vi du i.MX93 RMII (ERR051683) can set PS|FES thi reset moi hoan tat.
 * De rong neu SoC cua ban khong can.
 */
void eqos_board_fix_soc_reset(struct eqos_dev *dev);

/* Cache maintenance. Neu vung nho khong cache thi de rong. */
void eqos_dcache_clean(void *addr, size_t size);       /* CPU -> DRAM */
void eqos_dcache_invalidate(void *addr, size_t size);  /* DRAM -> CPU */

/* Log. Co the map sang printf() hoac de rong. */
void eqos_printf(const char *fmt, ...);

#endif /* EQOS_H */
