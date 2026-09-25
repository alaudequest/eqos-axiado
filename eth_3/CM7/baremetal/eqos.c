/* SPDX-License-Identifier: GPL-2.0 */
/*
 * eqos.c - Driver baremetal cho Synopsys DesignWare Ethernet QoS
 *
 * Viet lai tu drivers/net/dwc_eth_qos.c cua U-Boot.
 * Copyright (c) 2016, NVIDIA CORPORATION (code goc).
 */

#include <string.h>
#include "eqos.h"
#include "eqos_hw.h"

/* ================================================================== */
/* Kiem tra cau hinh luc bien dich                                     */
/* ================================================================== */

#if (EQOS_TX_DESC_COUNT & (EQOS_TX_DESC_COUNT - 1)) != 0
#error "EQOS_TX_DESC_COUNT phai la luy thua cua 2"
#endif
#if (EQOS_RX_DESC_COUNT & (EQOS_RX_DESC_COUNT - 1)) != 0
#error "EQOS_RX_DESC_COUNT phai la luy thua cua 2"
#endif
#if (EQOS_BUF_SIZE % EQOS_AXI_WIDTH) != 0
#error "EQOS_BUF_SIZE phai la boi so cua EQOS_AXI_WIDTH"
#endif

#if EQOS_DESC_CACHED
  #define EQOS_DESC_STRIDE  EQOS_CACHELINE
  #if ((EQOS_CACHELINE - 16) / EQOS_AXI_WIDTH) > 7
  #error "Khong the gian descriptor ra 1 cache line: truong DSL chi rong 3 bit. \
Hay dat descriptor ring vao bo nho khong cache (EQOS_DESC_CACHED = 0)."
  #endif
#else
  /* Bo nho khong cache -> xep sat nhau, DSL = 0 */
  #define EQOS_DESC_STRIDE  16
#endif

#define EQOS_DSL  ((EQOS_DESC_STRIDE - 16) / EQOS_AXI_WIDTH)

/* ================================================================== */
/* Tien ich                                                            */
/* ================================================================== */

#if defined(__aarch64__)
#define eqos_barrier()  __asm__ volatile("dsb sy" ::: "memory")
#elif defined(__arm__)
#define eqos_barrier()  __asm__ volatile("dsb" ::: "memory")
#else
#define eqos_barrier()  __asm__ volatile("" ::: "memory")
#endif

#define EQOS_LO32(a)  ((uint32_t)((uintptr_t)(a) & 0xffffffffu))
#define EQOS_HI32(a)  ((uint32_t)((uint64_t)(uintptr_t)(a) >> 32))

#define EQOS_MIN(a, b)  ((a) < (b) ? (a) : (b))

static inline uint32_t eqos_rd(struct eqos_dev *dev, uint32_t off)
{
    uint32_t v = *(volatile uint32_t *)(dev->base + off);
    __asm__ volatile("" ::: "memory");
    return v;
}

static inline void eqos_wr(struct eqos_dev *dev, uint32_t off, uint32_t val)
{
    __asm__ volatile("" ::: "memory");
    *(volatile uint32_t *)(dev->base + off) = val;
}

static inline void eqos_setbits(struct eqos_dev *dev, uint32_t off, uint32_t bits)
{
    eqos_wr(dev, off, eqos_rd(dev, off) | bits);
}

static inline void eqos_clrbits(struct eqos_dev *dev, uint32_t off, uint32_t bits)
{
    eqos_wr(dev, off, eqos_rd(dev, off) & ~bits);
}

static inline void eqos_clrsetbits(struct eqos_dev *dev, uint32_t off,
                                   uint32_t clr, uint32_t set)
{
    eqos_wr(dev, off, (eqos_rd(dev, off) & ~clr) | set);
}

/* Cho `off` co (hoac khong co) `mask`. Tra EQOS_OK / EQOS_ETIMEDOUT. */
static int eqos_wait_bits(struct eqos_dev *dev, uint32_t off, uint32_t mask,
                          bool set, uint32_t timeout_us)
{
    uint32_t elapsed = 0;

    for (;;) {
        uint32_t v = eqos_rd(dev, off) & mask;
        if (set ? (v == mask) : (v == 0))
            return EQOS_OK;
        if (elapsed >= timeout_us)
            return EQOS_ETIMEDOUT;
        eqos_udelay(1);
        elapsed++;
    }
}

/* ================================================================== */
/* Bo nho DMA cap phat tinh                                            */
/* ================================================================== */

struct eqos_desc {
    volatile uint32_t des0;
    volatile uint32_t des1;
    volatile uint32_t des2;
    volatile uint32_t des3;
};

#if EQOS_DESC_CACHED
#define EQOS_DESC_SECTION
#else
/* Section nay PHAI duoc map la Device / Normal-NonCacheable trong MMU. */
#define EQOS_DESC_SECTION  __attribute__((section(".eqos_nocache")))
#endif

static uint8_t eqos_tx_desc_mem[EQOS_TX_DESC_COUNT * EQOS_DESC_STRIDE]
    __attribute__((aligned(EQOS_CACHELINE))) EQOS_DESC_SECTION;
static uint8_t eqos_rx_desc_mem[EQOS_RX_DESC_COUNT * EQOS_DESC_STRIDE]
    __attribute__((aligned(EQOS_CACHELINE))) EQOS_DESC_SECTION;

static uint8_t eqos_tx_buf[EQOS_TX_DESC_COUNT][EQOS_BUF_SIZE]
    __attribute__((aligned(EQOS_CACHELINE)));
static uint8_t eqos_rx_buf[EQOS_RX_DESC_COUNT][EQOS_BUF_SIZE]
    __attribute__((aligned(EQOS_CACHELINE)));

static inline struct eqos_desc *eqos_txd(uint32_t i)
{
    return (struct eqos_desc *)(eqos_tx_desc_mem + i * EQOS_DESC_STRIDE);
}

static inline struct eqos_desc *eqos_rxd(uint32_t i)
{
    return (struct eqos_desc *)(eqos_rx_desc_mem + i * EQOS_DESC_STRIDE);
}

/* Cache op cho descriptor - rong khi ring nam trong bo nho khong cache. */
#if EQOS_DESC_CACHED
static inline void eqos_desc_flush(void *d) { eqos_dcache_clean(d, EQOS_DESC_STRIDE); }
static inline void eqos_desc_inval(void *d) { eqos_dcache_invalidate(d, EQOS_DESC_STRIDE); }
#else
static inline void eqos_desc_flush(void *d) { (void)d; }
static inline void eqos_desc_inval(void *d) { (void)d; }
#endif

#if EQOS_BUF_CACHED
static inline void eqos_buf_flush(void *b, size_t n) { eqos_dcache_clean(b, n); }
static inline void eqos_buf_inval(void *b, size_t n) { eqos_dcache_invalidate(b, n); }
#else
static inline void eqos_buf_flush(void *b, size_t n) { (void)b; (void)n; }
static inline void eqos_buf_inval(void *b, size_t n) { (void)b; (void)n; }
#endif

/* ================================================================== */
/* MDIO                                                                */
/* ================================================================== */

/* Chon truong CR theo tan so clock CSR. */
static uint32_t eqos_mdio_cr_from_clk(uint32_t hz)
{
    uint32_t mhz = hz / 1000000u;

    if (mhz >= 20  && mhz < 35)  return EQOS_MDIO_CR_20_35;
    if (mhz >= 35  && mhz < 60)  return EQOS_MDIO_CR_35_60;
    if (mhz >= 60  && mhz < 100) return EQOS_MDIO_CR_60_100;
    if (mhz >= 100 && mhz < 150) return EQOS_MDIO_CR_100_150;
    if (mhz >= 150 && mhz < 250) return EQOS_MDIO_CR_150_250;
    if (mhz >= 250 && mhz < 300) return EQOS_MDIO_CR_250_300;
    if (mhz >= 300 && mhz < 500) return EQOS_MDIO_CR_300_500;
    if (mhz >= 500 && mhz < 800) return EQOS_MDIO_CR_500_800;

    eqos_printf("eqos: csr_clk %u Hz ngoai bang CR, dung 250-300\n", hz);
    return EQOS_MDIO_CR_250_300;
}

static int eqos_mdio_wait_idle(struct eqos_dev *dev)
{
    return eqos_wait_bits(dev, EQOS_MAC_MDIO_ADDRESS, EQOS_MDIO_ADDR_GB,
                          false, 1000000);
}

int eqos_mdio_read(struct eqos_dev *dev, int phy, int reg)
{
    uint32_t val;
    int ret;

    ret = eqos_mdio_wait_idle(dev);
    if (ret) {
        eqos_printf("eqos: MDIO ban truoc khi doc\n");
        return ret;
    }

    /* Giu nguyen bit SKAP, ghi de phan con lai. */
    val = eqos_rd(dev, EQOS_MAC_MDIO_ADDRESS) & EQOS_MDIO_ADDR_SKAP;
    val |= ((uint32_t)phy & EQOS_MDIO_ADDR_PA_MASK)  << EQOS_MDIO_ADDR_PA_SHIFT;
    val |= ((uint32_t)reg & EQOS_MDIO_ADDR_RDA_MASK) << EQOS_MDIO_ADDR_RDA_SHIFT;
    val |= dev->mdio_cr                              << EQOS_MDIO_ADDR_CR_SHIFT;
    val |= EQOS_MDIO_ADDR_GOC_READ                   << EQOS_MDIO_ADDR_GOC_SHIFT;
    val |= EQOS_MDIO_ADDR_GB;

    eqos_wr(dev, EQOS_MAC_MDIO_ADDRESS, val);

    ret = eqos_mdio_wait_idle(dev);
    if (ret) {
        eqos_printf("eqos: MDIO doc khong hoan tat\n");
        return ret;
    }

    return (int)(eqos_rd(dev, EQOS_MAC_MDIO_DATA) & EQOS_MDIO_DATA_GD_MASK);
}

int eqos_mdio_write(struct eqos_dev *dev, int phy, int reg, uint16_t data)
{
    uint32_t val;
    int ret;

    ret = eqos_mdio_wait_idle(dev);
    if (ret) {
        eqos_printf("eqos: MDIO ban truoc khi ghi\n");
        return ret;
    }

    eqos_wr(dev, EQOS_MAC_MDIO_DATA, data);

    val = eqos_rd(dev, EQOS_MAC_MDIO_ADDRESS) & EQOS_MDIO_ADDR_SKAP;
    val |= ((uint32_t)phy & EQOS_MDIO_ADDR_PA_MASK)  << EQOS_MDIO_ADDR_PA_SHIFT;
    val |= ((uint32_t)reg & EQOS_MDIO_ADDR_RDA_MASK) << EQOS_MDIO_ADDR_RDA_SHIFT;
    val |= dev->mdio_cr                              << EQOS_MDIO_ADDR_CR_SHIFT;
    val |= EQOS_MDIO_ADDR_GOC_WRITE                  << EQOS_MDIO_ADDR_GOC_SHIFT;
    val |= EQOS_MDIO_ADDR_GB;

    eqos_wr(dev, EQOS_MAC_MDIO_ADDRESS, val);

    ret = eqos_mdio_wait_idle(dev);
    if (ret)
        eqos_printf("eqos: MDIO ghi khong hoan tat\n");

    return ret;
}

/* ================================================================== */
/* PHY - clause 22 chuan, khong can driver rieng cho tung con PHY      */
/* ================================================================== */

#define MII_BMCR            0x00
#define MII_BMSR            0x01
#define MII_PHYSID1         0x02
#define MII_PHYSID2         0x03
#define MII_ADVERTISE       0x04
#define MII_LPA             0x05
#define MII_CTRL1000        0x09
#define MII_STAT1000        0x0a

#define BMCR_RESET          0x8000
#define BMCR_LOOPBACK       0x4000
#define BMCR_SPEED100       0x2000
#define BMCR_ANENABLE       0x1000
#define BMCR_POWERDOWN      0x0800
#define BMCR_ANRESTART      0x0200
#define BMCR_FULLDPLX       0x0100
#define BMCR_SPEED1000      0x0040

#define BMSR_ANEGCOMPLETE   0x0020
#define BMSR_ANEGCAPABLE    0x0008
#define BMSR_LSTATUS        0x0004

#define ADVERTISE_CSMA      0x0001
#define ADVERTISE_10HALF    0x0020
#define ADVERTISE_10FULL    0x0040
#define ADVERTISE_100HALF   0x0080
#define ADVERTISE_100FULL   0x0100
#define ADVERTISE_PAUSE     0x0400

#define ADVERTISE_1000HALF  0x0100
#define ADVERTISE_1000FULL  0x0200
#define LPA_1000HALF        0x0400
#define LPA_1000FULL        0x0800

static int eqos_phy_find(struct eqos_dev *dev)
{
    for (int addr = 0; addr < 32; addr++) {
        int id1 = eqos_mdio_read(dev, addr, MII_PHYSID1);
        int id2 = eqos_mdio_read(dev, addr, MII_PHYSID2);

        if (id1 < 0 || id2 < 0)
            continue;
        if ((id1 == 0xffff && id2 == 0xffff) || (id1 == 0 && id2 == 0))
            continue;

        eqos_printf("eqos: tim thay PHY tai dia chi %d (id %04x:%04x)\n",
                    addr, id1, id2);
        return addr;
    }
    return EQOS_ENODEV;
}

static int eqos_phy_reset(struct eqos_dev *dev)
{
    int ret = eqos_mdio_write(dev, dev->phy_addr, MII_BMCR, BMCR_RESET);
    if (ret)
        return ret;

    /* Databook 802.3: reset phai xong trong 0.5s */
    for (int i = 0; i < 500; i++) {
        int bmcr = eqos_mdio_read(dev, dev->phy_addr, MII_BMCR);
        if (bmcr < 0)
            return bmcr;
        if (!(bmcr & BMCR_RESET))
            return EQOS_OK;
        eqos_udelay(1000);
    }

    eqos_printf("eqos: PHY reset khong tu clear\n");
    return EQOS_ETIMEDOUT;
}

static int eqos_phy_start_aneg(struct eqos_dev *dev)
{
    uint32_t max = dev->cfg.max_speed ? dev->cfg.max_speed : 1000;
    uint16_t adv = ADVERTISE_CSMA | ADVERTISE_PAUSE;
    uint16_t ctrl1000 = 0;
    int ret;

    if (max >= 10)  adv |= ADVERTISE_10HALF  | ADVERTISE_10FULL;
    if (max >= 100) adv |= ADVERTISE_100HALF | ADVERTISE_100FULL;
    if (max >= 1000 && dev->cfg.phy_iface != EQOS_IFACE_RMII &&
        dev->cfg.phy_iface != EQOS_IFACE_MII)
        ctrl1000 |= ADVERTISE_1000FULL | ADVERTISE_1000HALF;

    ret = eqos_mdio_write(dev, dev->phy_addr, MII_ADVERTISE, adv);
    if (ret)
        return ret;

    /* Ghi reg 9 ngay ca khi = 0, de xoa quang cao gigabit cu. */
    ret = eqos_mdio_write(dev, dev->phy_addr, MII_CTRL1000, ctrl1000);
    if (ret)
        return ret;

    return eqos_mdio_write(dev, dev->phy_addr, MII_BMCR,
                           BMCR_ANENABLE | BMCR_ANRESTART);
}

static int eqos_phy_wait_link(struct eqos_dev *dev)
{
    uint32_t timeout_ms = dev->cfg.aneg_timeout_ms ? dev->cfg.aneg_timeout_ms : 5000;

    for (uint32_t i = 0; i < timeout_ms; i++) {
        int bmsr;

        /* LSTATUS la bit latch-low: doc 2 lan de lay trang thai hien tai. */
        (void)eqos_mdio_read(dev, dev->phy_addr, MII_BMSR);
        bmsr = eqos_mdio_read(dev, dev->phy_addr, MII_BMSR);
        if (bmsr < 0)
            return bmsr;

        if ((bmsr & BMSR_LSTATUS) && (bmsr & BMSR_ANEGCOMPLETE))
            return EQOS_OK;

        eqos_udelay(1000);
    }

    eqos_printf("eqos: het gio cho auto-negotiation\n");
    return EQOS_ENOLINK;
}

/* Doc ket qua thoa thuan -> dev->speed / dev->duplex_full */
static int eqos_phy_read_result(struct eqos_dev *dev)
{
    int adv, lpa, ctrl1000, stat1000, common;

    ctrl1000 = eqos_mdio_read(dev, dev->phy_addr, MII_CTRL1000);
    stat1000 = eqos_mdio_read(dev, dev->phy_addr, MII_STAT1000);
    if (ctrl1000 >= 0 && stat1000 >= 0) {
        if ((ctrl1000 & ADVERTISE_1000FULL) && (stat1000 & LPA_1000FULL)) {
            dev->speed = EQOS_SPEED_1000;
            dev->duplex_full = true;
            return EQOS_OK;
        }
        if ((ctrl1000 & ADVERTISE_1000HALF) && (stat1000 & LPA_1000HALF)) {
            dev->speed = EQOS_SPEED_1000;
            dev->duplex_full = false;
            return EQOS_OK;
        }
    }

    adv = eqos_mdio_read(dev, dev->phy_addr, MII_ADVERTISE);
    lpa = eqos_mdio_read(dev, dev->phy_addr, MII_LPA);
    if (adv < 0 || lpa < 0)
        return EQOS_EIO;

    common = adv & lpa;

    if (common & ADVERTISE_100FULL)      { dev->speed = EQOS_SPEED_100; dev->duplex_full = true;  }
    else if (common & ADVERTISE_100HALF) { dev->speed = EQOS_SPEED_100; dev->duplex_full = false; }
    else if (common & ADVERTISE_10FULL)  { dev->speed = EQOS_SPEED_10;  dev->duplex_full = true;  }
    else if (common & ADVERTISE_10HALF)  { dev->speed = EQOS_SPEED_10;  dev->duplex_full = false; }
    else {
        eqos_printf("eqos: khong tim duoc che do chung (adv=%04x lpa=%04x)\n",
                    adv, lpa);
        return EQOS_ENOLINK;
    }

    return EQOS_OK;
}

static int eqos_phy_bringup(struct eqos_dev *dev)
{
    int ret;

    if (dev->cfg.phy_addr == EQOS_PHY_ADDR_AUTO) {
        ret = eqos_phy_find(dev);
        if (ret < 0) {
            eqos_printf("eqos: khong tim thay PHY nao tren bus MDIO\n");
            return EQOS_ENODEV;
        }
        dev->phy_addr = ret;
    } else {
        dev->phy_addr = dev->cfg.phy_addr;
    }

    ret = eqos_phy_reset(dev);
    if (ret)
        return ret;

    ret = eqos_phy_start_aneg(dev);
    if (ret)
        return ret;

    ret = eqos_phy_wait_link(dev);
    if (ret)
        return ret;

    ret = eqos_phy_read_result(dev);
    if (ret)
        return ret;

    dev->link_up = true;
    eqos_printf("eqos: link up %d Mbps %s duplex\n",
                (int)dev->speed, dev->duplex_full ? "full" : "half");
    return EQOS_OK;
}

/* ================================================================== */
/* Cau hinh MAC theo toc do / duplex  (tuong ung eqos_adjust_link)     */
/* ================================================================== */

static int eqos_adjust_link(struct eqos_dev *dev)
{
    uint32_t clr = 0, set = 0;

    /* Duplex */
    if (dev->duplex_full) {
        set |= EQOS_MAC_CFG_DM;
    } else {
        clr |= EQOS_MAC_CFG_DM;
    }

    /* Toc do:
     *   1000M -> PS=0, FES=0   (GMII)
     *    100M -> PS=1, FES=1   (MII 100)
     *     10M -> PS=1, FES=0   (MII 10)
     */
    switch (dev->speed) {
    case EQOS_SPEED_1000:
        clr |= EQOS_MAC_CFG_PS | EQOS_MAC_CFG_FES;
        break;
    case EQOS_SPEED_100:
        set |= EQOS_MAC_CFG_PS | EQOS_MAC_CFG_FES;
        break;
    case EQOS_SPEED_10:
        set |= EQOS_MAC_CFG_PS;
        clr |= EQOS_MAC_CFG_FES;
        break;
    default:
        eqos_printf("eqos: toc do khong hop le %d\n", (int)dev->speed);
        return EQOS_EINVAL;
    }

    eqos_clrsetbits(dev, EQOS_MAC_CONFIGURATION, clr, set);

    /* Workaround tu driver goc: khi chuyen sang half-duplex phai xa TX queue */
    if (!dev->duplex_full)
        eqos_setbits(dev, EQOS_MTL_TXQ0_OPERATION_MODE, EQOS_MTL_TXQ_FTQ);

    return eqos_board_set_tx_clk(dev, dev->speed);
}

/* ================================================================== */
/* Cac buoc cau hinh MTL / MAC / DMA                                   */
/* ================================================================== */

/* Tra ve TQS de dung tiep cho TXPBL. */
static uint32_t eqos_config_mtl(struct eqos_dev *dev)
{
    uint32_t hwf1, tx_fifo, rx_fifo, tqs, rqs;

    /* --- TX queue: store & forward, bat queue --- */
    eqos_setbits(dev, EQOS_MTL_TXQ0_OPERATION_MODE,
                 EQOS_MTL_TXQ_TSF |
                 (EQOS_MTL_TXQ_TXQEN_ENABLED << EQOS_MTL_TXQ_TXQEN_SHIFT));

    eqos_wr(dev, EQOS_MTL_TXQ0_QUANTUM_WEIGHT, 0x10);

    /* --- RX queue: store & forward (khong dung jumbo frame) --- */
    eqos_setbits(dev, EQOS_MTL_RXQ0_OPERATION_MODE, EQOS_MTL_RXQ_RSF);

    /* --- Kich thuoc FIFO: dua het cho queue 0 --- */
    hwf1 = eqos_rd(dev, EQOS_MAC_HW_FEATURE1);
    tx_fifo = (hwf1 >> EQOS_HWF1_TXFIFOSIZE_SHIFT) & EQOS_HWF1_TXFIFOSIZE_MASK;
    rx_fifo = (hwf1 >> EQOS_HWF1_RXFIFOSIZE_SHIFT) & EQOS_HWF1_RXFIFOSIZE_MASK;

    /* Truong nay ma hoa log2(n / 128) */
    tx_fifo = 128u << tx_fifo;
    rx_fifo = 128u << rx_fifo;

    if (dev->cfg.tx_fifo_sz)
        tx_fifo = dev->cfg.tx_fifo_sz;
    if (dev->cfg.rx_fifo_sz)
        rx_fifo = dev->cfg.rx_fifo_sz;

    eqos_printf("eqos: FIFO tx=%u rx=%u byte\n", tx_fifo, rx_fifo);

    /* Chan duoi: cong thuc (n/256)-1 se tran nguoc neu FIFO < 512 byte */
    if (tx_fifo < 512u) tx_fifo = 512u;
    if (rx_fifo < 512u) rx_fifo = 512u;

    /* TQS/RQS ma hoa (n / 256) - 1 */
    tqs = tx_fifo / 256u - 1u;
    rqs = rx_fifo / 256u - 1u;

    eqos_clrsetbits(dev, EQOS_MTL_TXQ0_OPERATION_MODE,
                    EQOS_MTL_TXQ_TQS_MASK << EQOS_MTL_TXQ_TQS_SHIFT,
                    tqs << EQOS_MTL_TXQ_TQS_SHIFT);
    eqos_clrsetbits(dev, EQOS_MTL_RXQ0_OPERATION_MODE,
                    EQOS_MTL_RXQ_RQS_MASK << EQOS_MTL_RXQ_RQS_SHIFT,
                    rqs << EQOS_MTL_RXQ_RQS_SHIFT);

    // /* --- Flow control: chi bat khi RX FIFO >= 4KB --- */
    // if (rqs >= (4096u / 256u - 1u)) {
    //     uint32_t rfd, rfa;

    //     eqos_setbits(dev, EQOS_MTL_RXQ0_OPERATION_MODE, EQOS_MTL_RXQ_EHFC);

    //     /* RFA: nguong con lai bao nhieu thi GUI pause frame
    //      * RFD: nguong con lai bao nhieu thi NGUNG gui pause frame */
    //     if (rqs == (4096u / 256u - 1u)) {
    //         /* 4KB khong du cho cong thuc chuan, van co the tran */
    //         rfd = 0x3;  /* full - 3K  */
    //         rfa = 0x1;  /* full - 1.5K */
    //     } else if (rqs == (8192u / 256u - 1u)) {
    //         rfd = 0x6;  /* full - 4K */
    //         rfa = 0xa;  /* full - 6K */
    //     } else if (rqs == (16384u / 256u - 1u)) {
    //         rfd = 0x6;  /* full - 4K  */
    //         rfa = 0x12; /* full - 10K */
    //     } else {
    //         rfd = 0x6;  /* full - 4K  */
    //         rfa = 0x1e; /* full - 16K */
    //     }

    //     eqos_clrsetbits(dev, EQOS_MTL_RXQ0_OPERATION_MODE,
    //                     (EQOS_MTL_RXQ_RFD_MASK << EQOS_MTL_RXQ_RFD_SHIFT) |
    //                     (EQOS_MTL_RXQ_RFA_MASK << EQOS_MTL_RXQ_RFA_SHIFT),
    //                     (rfd << EQOS_MTL_RXQ_RFD_SHIFT) |
    //                     (rfa << EQOS_MTL_RXQ_RFA_SHIFT));
    // }

    return tqs;
}

static void eqos_config_mac(struct eqos_dev *dev)
{
    uint32_t rxq_mode = dev->cfg.rxq_mode ? (uint32_t)dev->cfg.rxq_mode
                                          : (uint32_t)EQOS_RXQ_MODE_DCB;

    /* Bat RX queue 0 */
    eqos_clrsetbits(dev, EQOS_MAC_RXQ_CTRL0,
                    EQOS_MAC_RXQ0EN_MASK << EQOS_MAC_RXQ0EN_SHIFT,
                    rxq_mode << EQOS_MAC_RXQ0EN_SHIFT);

    /* Multicast va broadcast -> queue 0 */
    eqos_setbits(dev, EQOS_MAC_RXQ_CTRL1, EQOS_MAC_RXQ_CTRL1_MCBCQEN);

    /* Promiscuous: mac dinh TAT (khac U-Boot goc - luon bat). Bat khi can sniff. */
    if (dev->cfg.promiscuous)
        eqos_setbits(dev, EQOS_MAC_PACKET_FILTER, EQOS_MAC_PKT_FILTER_PR);
    else
        eqos_clrbits(dev, EQOS_MAC_PACKET_FILTER, EQOS_MAC_PKT_FILTER_PR);

    /* --- Flow control --- */
    /* Pause time toi da */
    eqos_setbits(dev, EQOS_MAC_Q0_TX_FLOW_CTRL,
                 EQOS_MAC_TXFC_PT_MASK << EQOS_MAC_TXFC_PT_SHIFT);
    /* Priority cho TX flow control -> queue 0 */
    eqos_clrbits(dev, EQOS_MAC_TXQ_PRTY_MAP0,
                 EQOS_MAC_TXQ_PRTY_PSTQ0_MASK << EQOS_MAC_TXQ_PRTY_PSTQ0_SHIFT);
    /* Priority cho RX flow control -> queue 0 */
    eqos_clrbits(dev, EQOS_MAC_RXQ_CTRL2,
                 EQOS_MAC_RXQ_CTRL2_PSRQ0_MASK << EQOS_MAC_RXQ_CTRL2_PSRQ0_SHIFT);

    eqos_setbits(dev, EQOS_MAC_Q0_TX_FLOW_CTRL, EQOS_MAC_TXFC_TFE);
    eqos_setbits(dev, EQOS_MAC_RX_FLOW_CTRL,   EQOS_MAC_RXFC_RFE);

    /*
     * CST + ACS: HW tu bo CRC va padding khi giao packet len driver.
     * Xoa GPSLCE/WD/JD/JE: khong dung jumbo, bat watchdog & jabber timer.
     */
    eqos_clrsetbits(dev, EQOS_MAC_CONFIGURATION,
                    EQOS_MAC_CFG_GPSLCE | EQOS_MAC_CFG_WD |
                    EQOS_MAC_CFG_JD | EQOS_MAC_CFG_JE,
                    EQOS_MAC_CFG_CST | EQOS_MAC_CFG_ACS);

    if (dev->cfg.loopback)
        eqos_setbits(dev, EQOS_MAC_CONFIGURATION, EQOS_MAC_CFG_LM);

    eqos_set_mac_addr(dev, dev->cfg.mac_addr);
}

static void eqos_config_dma(struct eqos_dev *dev, uint32_t tqs)
{
    uint32_t pbl;

    /* Operate on second packet: khong doi packet truoc hoan tat */
    eqos_setbits(dev, EQOS_DMA_CH0_TX_CONTROL, EQOS_DMA_CH_TX_OSP);

    /* Kich thuoc buffer RX. Phai la boi so cua do rong bus. */
    eqos_clrsetbits(dev, EQOS_DMA_CH0_RX_CONTROL,
                    EQOS_DMA_CH_RX_RBSZ_MASK << EQOS_DMA_CH_RX_RBSZ_SHIFT,
                    (uint32_t)EQOS_BUF_SIZE << EQOS_DMA_CH_RX_RBSZ_SHIFT);

    /* DSL = so don vi bus width can nhay giua 2 descriptor */
    eqos_setbits(dev, EQOS_DMA_CH0_CONTROL,
                 EQOS_DMA_CH_CTRL_PBLX8 |
                 ((uint32_t)EQOS_DSL << EQOS_DMA_CH_CTRL_DSL_SHIFT));

    /*
     * Burst phai < 1/2 kich thuoc FIFO.
     * TQS ma hoa (n/256)-1. Moi burst = pbl * 8 (PBLX8) * axi_width byte.
     * => pbl = tqs + 1, chan tren 32.
     */
    pbl = EQOS_MIN(tqs + 1u, 32u);
    eqos_clrsetbits(dev, EQOS_DMA_CH0_TX_CONTROL,
                    EQOS_DMA_CH_TX_TXPBL_MASK << EQOS_DMA_CH_TX_TXPBL_SHIFT,
                    pbl << EQOS_DMA_CH_TX_TXPBL_SHIFT);

    eqos_clrsetbits(dev, EQOS_DMA_CH0_RX_CONTROL,
                    EQOS_DMA_CH_RX_RXPBL_MASK << EQOS_DMA_CH_RX_RXPBL_SHIFT,
                    8u << EQOS_DMA_CH_RX_RXPBL_SHIFT);

    /* Tuning hieu nang bus */
    eqos_wr(dev, EQOS_DMA_SYSBUS_MODE,
            (2u << EQOS_DMA_SYSBUS_RD_OSR_LMT_SHIFT) |
            EQOS_DMA_SYSBUS_EAME |
            EQOS_DMA_SYSBUS_BLEN16 | EQOS_DMA_SYSBUS_BLEN8 |
            EQOS_DMA_SYSBUS_BLEN4);
}

/* Nap dia chi buffer vao mot RX descriptor va tra quyen cho DMA. */
static void eqos_rx_desc_arm(uint32_t idx)
{
    struct eqos_desc *d = eqos_rxd(idx);
    uintptr_t buf = (uintptr_t)eqos_rx_buf[idx];

    eqos_buf_inval(eqos_rx_buf[idx], EQOS_BUF_SIZE);

    d->des0 = EQOS_LO32(buf);
    d->des1 = EQOS_HI32(buf);
    d->des2 = 0;
    /* Barrier: neu HW thay bit OWN thi phai thay ca 3 word con lai */
    eqos_barrier();
    d->des3 = EQOS_RDES3_OWN | EQOS_RDES3_BUF1V;
    eqos_desc_flush(d);
}

/*
 * Xoa descriptor bang ghi 32-bit tuong minh thay vi memset(): neu ban map
 * vung descriptor la Device memory thay vi Normal-NonCacheable, memset() cua
 * libc co the dung truy cap khong can le hoac DC ZVA va gay fault.
 */
static void eqos_desc_zero(struct eqos_desc *d)
{
    d->des0 = 0;
    d->des1 = 0;
    d->des2 = 0;
    d->des3 = 0;
}

static void eqos_setup_rings(struct eqos_dev *dev)
{
    uintptr_t addr;

    for (uint32_t i = 0; i < EQOS_TX_DESC_COUNT; i++) {
        eqos_desc_zero(eqos_txd(i));
        eqos_desc_flush(eqos_txd(i));
    }
    /* eqos_rx_desc_arm() ghi ca 4 word nen khong can xoa truoc */
    for (uint32_t i = 0; i < EQOS_RX_DESC_COUNT; i++)
        eqos_rx_desc_arm(i);

    /* --- Dang ky ring voi DMA --- */
    addr = (uintptr_t)eqos_txd(0);
    eqos_wr(dev, EQOS_DMA_CH0_TXDESC_LIST_HADDR, EQOS_HI32(addr));
    eqos_wr(dev, EQOS_DMA_CH0_TXDESC_LIST_ADDR,  EQOS_LO32(addr));
    eqos_wr(dev, EQOS_DMA_CH0_TXDESC_RING_LEN,   EQOS_TX_DESC_COUNT - 1);

    addr = (uintptr_t)eqos_rxd(0);
    eqos_wr(dev, EQOS_DMA_CH0_RXDESC_LIST_HADDR, EQOS_HI32(addr));
    eqos_wr(dev, EQOS_DMA_CH0_RXDESC_LIST_ADDR,  EQOS_LO32(addr));
    eqos_wr(dev, EQOS_DMA_CH0_RXDESC_RING_LEN,   EQOS_RX_DESC_COUNT - 1);

    /*
     * TX tail tro vao descriptor DAU: chua co descriptor nao thuoc DMA.
     * eqos_send() se day tail len khi co packet.
     */
    eqos_wr(dev, EQOS_DMA_CH0_TXDESC_TAIL_PTR, EQOS_LO32(eqos_txd(0)));

    /*
     * RX tail tro vao descriptor CUOI: tat ca descriptor thuoc DMA.
     * (Day la quy uoc cua driver U-Boot goc; Linux/stmmac dung "mot o sau
     *  descriptor cuoi". Giu nguyen quy uoc goc vi da duoc kiem chung.)
     */
    eqos_wr(dev, EQOS_DMA_CH0_RXDESC_TAIL_PTR,
            EQOS_LO32(eqos_rxd(EQOS_RX_DESC_COUNT - 1)));

    dev->tx_head = 0;
    dev->rx_head = 0;
}

/* ================================================================== */
/* API cong khai                                                       */
/* ================================================================== */

void eqos_set_mac_addr(struct eqos_dev *dev, const uint8_t mac[6])
{
    eqos_wr(dev, EQOS_MAC_ADDRESS0_HIGH,
            ((uint32_t)mac[5] << 8) | (uint32_t)mac[4]);
    eqos_wr(dev, EQOS_MAC_ADDRESS0_LOW,
            ((uint32_t)mac[3] << 24) | ((uint32_t)mac[2] << 16) |
            ((uint32_t)mac[1] << 8)  |  (uint32_t)mac[0]);
}

int eqos_init(struct eqos_dev *dev, const struct eqos_cfg *cfg)
{
    uint32_t tqs, swr_timeout;
    int ret;

    if (!dev || !cfg || !cfg->base || !cfg->csr_clk_hz)
        return EQOS_EINVAL;

    memset(dev, 0, sizeof(*dev));
    dev->cfg  = *cfg;
    dev->base = cfg->base;
    dev->desc_stride = EQOS_DESC_STRIDE;
    dev->mdio_cr = eqos_mdio_cr_from_clk(cfg->csr_clk_hz);

    /* ---- 1. Board: clock, reset, pinmux, glue cua SoC ---- */
    ret = eqos_board_init(dev);
    if (ret) {
        eqos_printf("eqos: eqos_board_init() loi: %d\n", ret);
        return ret;
    }
    eqos_udelay(10);

    eqos_printf("eqos: MAC version %08x\n", eqos_rd(dev, EQOS_MAC_VERSION));

    /* ---- 2. Software reset toan bo IP ---- */
    eqos_setbits(dev, EQOS_DMA_MODE, EQOS_DMA_MODE_SWR);

    /* Quirk cua SoC phai chen vao GIUA luc set SWR va luc no tu clear
     * (vd i.MX93 RMII ERR051683). */
    eqos_board_fix_soc_reset(dev);

    swr_timeout = dev->cfg.swr_timeout_us ? dev->cfg.swr_timeout_us : 100000;
    ret = eqos_wait_bits(dev, EQOS_DMA_MODE, EQOS_DMA_MODE_SWR, false, swr_timeout);
    if (ret) {
        eqos_printf("eqos: DMA_MODE.SWR khong tu clear -> clock chua chay?\n");
        return ret;
    }

    /* ---- 3. Bo dem 1us cho cac timer noi bo ---- */
    eqos_wr(dev, EQOS_MAC_US_TIC_COUNTER, (dev->cfg.csr_clk_hz / 1000000u) - 1u);

    /* ---- 4. PHY: reset, auto-neg, doc toc do ---- */
    ret = eqos_phy_bringup(dev);
    if (ret)
        return ret;

    /* ---- 5. Ap toc do/duplex vao MAC + tx clock ---- */
    ret = eqos_adjust_link(dev);
    if (ret)
        return ret;

    /* ---- 6. MTL -> MAC -> DMA ---- */
    tqs = eqos_config_mtl(dev);
    eqos_config_mac(dev);
    eqos_config_dma(dev, tqs);

    /* ---- 7. Descriptor ring ---- */
    eqos_setup_rings(dev);

    /* ---- 8. Bat het ---- */
    eqos_setbits(dev, EQOS_DMA_CH0_TX_CONTROL, EQOS_DMA_CH_TX_ST);
    eqos_setbits(dev, EQOS_DMA_CH0_RX_CONTROL, EQOS_DMA_CH_RX_SR);
    eqos_setbits(dev, EQOS_MAC_CONFIGURATION,
                 EQOS_MAC_CFG_TE | EQOS_MAC_CFG_RE);

    dev->started = true;
    eqos_printf("eqos: san sang\n");
    return EQOS_OK;
}

void eqos_stop(struct eqos_dev *dev)
{
    if (!dev->started)
        return;
    dev->started = false;

    /* Tat TX DMA truoc, cho MTL xa het packet dang cho */
    eqos_clrbits(dev, EQOS_DMA_CH0_TX_CONTROL, EQOS_DMA_CH_TX_ST);

    for (int i = 0; i < 10000; i++) {
        uint32_t v = eqos_rd(dev, EQOS_MTL_TXQ0_DEBUG);
        uint32_t trcsts = (v >> EQOS_MTL_TXQ_DEBUG_TRCSTS_SHIFT) &
                          EQOS_MTL_TXQ_DEBUG_TRCSTS_MASK;
        if (trcsts != 1 && !(v & EQOS_MTL_TXQ_DEBUG_TXQSTS))
            break;
        eqos_udelay(10);
    }

    /* Tat MAC TX/RX, roi cho MTL xa het packet RX */
    eqos_clrbits(dev, EQOS_MAC_CONFIGURATION,
                 EQOS_MAC_CFG_TE | EQOS_MAC_CFG_RE);

    for (int i = 0; i < 10000; i++) {
        uint32_t v = eqos_rd(dev, EQOS_MTL_RXQ0_DEBUG);
        uint32_t prxq   = (v >> EQOS_MTL_RXQ_DEBUG_PRXQ_SHIFT) &
                          EQOS_MTL_RXQ_DEBUG_PRXQ_MASK;
        uint32_t rxqsts = (v >> EQOS_MTL_RXQ_DEBUG_RXQSTS_SHIFT) &
                          EQOS_MTL_RXQ_DEBUG_RXQSTS_MASK;
        if (!prxq && !rxqsts)
            break;
        eqos_udelay(10);
    }

    eqos_clrbits(dev, EQOS_DMA_CH0_RX_CONTROL, EQOS_DMA_CH_RX_SR);

    dev->link_up = false;
}

int eqos_send(struct eqos_dev *dev, const void *packet, uint32_t len)
{
    struct eqos_desc *d;
    uintptr_t buf;
    uint32_t idx = dev->tx_head;
    uint32_t des3 = 0;
    int i;

    if (!dev->started)
        return EQOS_EINVAL;
    if (len == 0 || len > EQOS_BUF_SIZE)
        return EQOS_EINVAL;

    d = eqos_txd(idx);

    /* Descriptor nay phai dang ranh (driver blocking nen luon dung). */
    eqos_desc_inval(d);
    if (d->des3 & EQOS_TDES3_OWN) {
        eqos_printf("eqos: TX descriptor %u van thuoc DMA\n", idx);
        return EQOS_EAGAIN;
    }

    memcpy(eqos_tx_buf[idx], packet, len);
    eqos_buf_flush(eqos_tx_buf[idx], len);

    buf = (uintptr_t)eqos_tx_buf[idx];
    d->des0 = EQOS_LO32(buf);
    d->des1 = EQOS_HI32(buf);
    d->des2 = len & EQOS_TDES2_B1L_MASK;
    /* Barrier: neu HW thay OWN thi phai thay ca des0/1/2 */
    eqos_barrier();
    d->des3 = EQOS_TDES3_OWN | EQOS_TDES3_FD | EQOS_TDES3_LD |
              (len & EQOS_TDES3_FL_MASK);
    eqos_desc_flush(d);

    /* Tail tro vao descriptor KE TIEP -> DMA xu ly toi truoc no */
    dev->tx_head = (idx + 1u) & (EQOS_TX_DESC_COUNT - 1u);
    eqos_wr(dev, EQOS_DMA_CH0_TXDESC_TAIL_PTR,
            EQOS_LO32(eqos_txd(dev->tx_head)));

    /* Cho HW nha quyen so huu (toi da ~1 giay) */
    for (i = 0; i < 1000000; i++) {
        eqos_desc_inval(d);
        des3 = d->des3;
        if (!(des3 & EQOS_TDES3_OWN))
            break;
        eqos_udelay(1);
    }

    if (des3 & EQOS_TDES3_OWN) {
        dev->stats.tx_timeouts++;
        eqos_printf("eqos: TX timeout (desc %u)\n", idx);
        return EQOS_ETIMEDOUT;
    }

    if (des3 & EQOS_TDES3_WB_ES) {
        dev->stats.tx_errors++;
        eqos_printf("eqos: TX loi, des3=%08x\n", des3);
        return EQOS_EIO;
    }

    dev->stats.tx_packets++;
    return EQOS_OK;
}

int eqos_recv(struct eqos_dev *dev, uint8_t **packet)
{
    struct eqos_desc *d;
    uint32_t des3, len;

    if (!dev->started)
        return EQOS_EINVAL;

    d = eqos_rxd(dev->rx_head);
    eqos_desc_inval(d);
    des3 = d->des3;

    if (des3 & EQOS_RDES3_OWN)
        return EQOS_EAGAIN;         /* chua co packet */

    /* --- Tu day la write-back format --- */

    /* Context descriptor (timestamp...): khong phai du lieu, nap lai luon */
    if (des3 & EQOS_RDES3_WB_CTXT) {
        eqos_recv_done(dev);
        return EQOS_EAGAIN;
    }

    /* Driver goc cua U-Boot KHONG kiem tra cac bit duoi day. */
    if (des3 & EQOS_RDES3_WB_ES) {
        dev->stats.rx_errors++;
        eqos_recv_done(dev);
        return EQOS_EIO;
    }

    /* Chi ho tro packet nam gon trong 1 descriptor (buffer 1600 > MTU) */
    if ((des3 & (EQOS_RDES3_WB_FD | EQOS_RDES3_WB_LD)) !=
        (EQOS_RDES3_WB_FD | EQOS_RDES3_WB_LD)) {
        dev->stats.rx_dropped++;
        eqos_recv_done(dev);
        return EQOS_EIO;
    }

    len = des3 & EQOS_RDES3_WB_PL_MASK;
    if (len == 0 || len > EQOS_BUF_SIZE) {
        dev->stats.rx_dropped++;
        eqos_recv_done(dev);
        return EQOS_EIO;
    }

    eqos_buf_inval(eqos_rx_buf[dev->rx_head], len);
    *packet = eqos_rx_buf[dev->rx_head];

    dev->stats.rx_packets++;
    return (int)len;
}

void eqos_recv_done(struct eqos_dev *dev)
{
    uint32_t idx = dev->rx_head;

    eqos_rx_desc_arm(idx);

    /* Day tail toi descriptor vua nap lai (quy uoc cua driver goc) */
    eqos_wr(dev, EQOS_DMA_CH0_RXDESC_TAIL_PTR, EQOS_LO32(eqos_rxd(idx)));

    dev->rx_head = (idx + 1u) & (EQOS_RX_DESC_COUNT - 1u);
}

int eqos_poll_link(struct eqos_dev *dev)
{
    enum eqos_speed old_speed = dev->speed;
    bool old_duplex = dev->duplex_full;
    int bmsr, ret;

    (void)eqos_mdio_read(dev, dev->phy_addr, MII_BMSR);
    bmsr = eqos_mdio_read(dev, dev->phy_addr, MII_BMSR);
    if (bmsr < 0)
        return bmsr;

    if (!(bmsr & BMSR_LSTATUS)) {
        if (dev->link_up) {
            eqos_printf("eqos: link down\n");
            dev->link_up = false;
        }
        return EQOS_ENOLINK;
    }

    ret = eqos_phy_read_result(dev);
    if (ret)
        return ret;

    if (!dev->link_up || old_speed != dev->speed || old_duplex != dev->duplex_full) {
        eqos_printf("eqos: link up %d Mbps %s duplex\n",
                    (int)dev->speed, dev->duplex_full ? "full" : "half");
        dev->link_up = true;
        return eqos_adjust_link(dev);
    }

    return EQOS_OK;
}

void eqos_dump_regs(struct eqos_dev *dev)
{
    eqos_printf("--- EQoS @ %08x ---\n", (uint32_t)dev->base);
    eqos_printf("MAC_VERSION      %08x\n", eqos_rd(dev, EQOS_MAC_VERSION));
    eqos_printf("MAC_CONFIG       %08x\n", eqos_rd(dev, EQOS_MAC_CONFIGURATION));
    eqos_printf("MAC_PKT_FILTER   %08x\n", eqos_rd(dev, EQOS_MAC_PACKET_FILTER));
    eqos_printf("MAC_RXQ_CTRL0    %08x\n", eqos_rd(dev, EQOS_MAC_RXQ_CTRL0));
    eqos_printf("MAC_HW_FEATURE0  %08x\n", eqos_rd(dev, EQOS_MAC_HW_FEATURE0));
    eqos_printf("MAC_HW_FEATURE1  %08x\n", eqos_rd(dev, EQOS_MAC_HW_FEATURE1));
    eqos_printf("MAC_DEBUG        %08x\n", eqos_rd(dev, EQOS_MAC_DEBUG));
    eqos_printf("MTL_TXQ0_OPMODE  %08x\n", eqos_rd(dev, EQOS_MTL_TXQ0_OPERATION_MODE));
    eqos_printf("MTL_TXQ0_DEBUG   %08x\n", eqos_rd(dev, EQOS_MTL_TXQ0_DEBUG));
    eqos_printf("MTL_RXQ0_OPMODE  %08x\n", eqos_rd(dev, EQOS_MTL_RXQ0_OPERATION_MODE));
    eqos_printf("MTL_RXQ0_DEBUG   %08x\n", eqos_rd(dev, EQOS_MTL_RXQ0_DEBUG));
    eqos_printf("DMA_MODE         %08x\n", eqos_rd(dev, EQOS_DMA_MODE));
    eqos_printf("DMA_SYSBUS_MODE  %08x\n", eqos_rd(dev, EQOS_DMA_SYSBUS_MODE));
    eqos_printf("DMA_CH0_CONTROL  %08x\n", eqos_rd(dev, EQOS_DMA_CH0_CONTROL));
    eqos_printf("DMA_CH0_TX_CTRL  %08x\n", eqos_rd(dev, EQOS_DMA_CH0_TX_CONTROL));
    eqos_printf("DMA_CH0_RX_CTRL  %08x\n", eqos_rd(dev, EQOS_DMA_CH0_RX_CONTROL));
    eqos_printf("DMA_CH0_STATUS   %08x\n", eqos_rd(dev, EQOS_DMA_CH0_STATUS));
    eqos_printf("stats: tx %u/%u/%u  rx %u/%u/%u\n",
                dev->stats.tx_packets, dev->stats.tx_errors, dev->stats.tx_timeouts,
                dev->stats.rx_packets, dev->stats.rx_errors, dev->stats.rx_dropped);
}
