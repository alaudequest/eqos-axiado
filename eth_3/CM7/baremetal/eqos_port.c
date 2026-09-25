/* SPDX-License-Identifier: GPL-2.0 */
/*
 * eqos_port.c - LOP PORT: day la file DUY NHAT ban phai sua khi doi board.
 *
 * Moi thu phu thuoc SoC (clock, reset, pinmux, glue register) nam o day.
 * Trong driver U-Boot, phan nay chinh la cac file dwc_eth_qos_<soc>.c va
 * struct eqos_ops.
 *
 * Ban duoi la khung + vi du cho 3 SoC. Xoa phan khong dung.
 */

#include <stdarg.h>
#include <stdio.h>
#include "eqos.h"
#include "eqos_hw.h"

/* ================================================================== */
/* 1. Delay                                                            */
/* ================================================================== */

/*
 * Thay bang timer that cua ban. Vong lap rong nhu duoi day KHONG chinh
 * xac va se sai khi doi toc do CPU / muc toi uu compiler.
 * Vi du ARMv8 co the dung CNTVCT_EL0, ARMv7-A dung generic timer,
 * Cortex-M dung DWT->CYCCNT hoac SysTick.
 */
void eqos_udelay(uint32_t us)
{
#if defined(__aarch64__)
    uint64_t freq, start, now, ticks;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
    ticks = ((uint64_t)us * freq) / 1000000u;

    do {
        __asm__ volatile("isb");
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    } while (now - start < ticks);
#else
    /* TODO: thay bang timer that */
    volatile uint32_t n = us * 100u;
    while (n--)
        __asm__ volatile("nop");
#endif
}

/* ================================================================== */
/* 2. Cache maintenance                                                */
/* ================================================================== */

/*
 * clean      = day du lieu tu cache ra DRAM (truoc khi DMA DOC)
 * invalidate = bo du lieu cu trong cache  (truoc khi CPU DOC cai DMA vua ghi)
 *
 * Neu ban chay khong bat MMU/D-cache, hoac dat toan bo vung DMA vao bo nho
 * khong cache, thi de rong ca hai ham va set EQOS_BUF_CACHED = 0.
 */

#if defined(__aarch64__)
#define CACHE_LINE 64

void eqos_dcache_clean(void *addr, size_t size)
{
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(CACHE_LINE - 1);
    uintptr_t end   = ((uintptr_t)addr + size + CACHE_LINE - 1) &
                      ~(uintptr_t)(CACHE_LINE - 1);

    for (uintptr_t p = start; p < end; p += CACHE_LINE)
        __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

void eqos_dcache_invalidate(void *addr, size_t size)
{
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(CACHE_LINE - 1);
    uintptr_t end   = ((uintptr_t)addr + size + CACHE_LINE - 1) &
                      ~(uintptr_t)(CACHE_LINE - 1);

    /*
     * Luu y: neu vung nho khong can le cache line, `dc ivac` co the vut bo
     * du lieu ke ben. Cac buffer o day da duoc align nen an toan.
     */
    for (uintptr_t p = start; p < end; p += CACHE_LINE)
        __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
#else
void eqos_dcache_clean(void *addr, size_t size)      { (void)addr; (void)size; }
void eqos_dcache_invalidate(void *addr, size_t size) { (void)addr; (void)size; }
#endif

/* ================================================================== */
/* 3. Log                                                              */
/* ================================================================== */

void eqos_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);   /* doi sang uart_puts() cua ban neu chua co printf */
    va_end(ap);
}

/* ================================================================== */
/* 4. Khoi tao board                                                   */
/* ================================================================== */

/*
 * Goi MOT LAN o dau eqos_init(), truoc khi cham vao thanh ghi EQoS.
 * Phai lam xong, theo thu tu:
 *
 *   a) Bat cac clock cap cho MAC:
 *        - CSR / AHB / APB clock  (cai nay quyet dinh cfg.csr_clk_hz)
 *        - AXI / master bus clock
 *        - TX clock, RX clock
 *        - PTP ref clock (thuong 125 MHz - can cho US_TIC_COUNTER)
 *   b) Thoat reset cua khoi MAC
 *   c) Cau hinh pinmux cho cac chan RGMII/RMII
 *   d) Ghi glue register cua SoC de chon che do PHY interface
 *   e) Reset PHY bang chan GPIO (assert -> delay -> deassert -> delay)
 *
 * Neu bo qua (a) hoac (b): thanh ghi doc ra toan 0x00000000 / 0xffffffff,
 * hoac bus hang. Neu bo qua (d): MDIO doc duoc PHY nhung khong bao gio
 * co du lieu chay.
 */
int eqos_board_init(struct eqos_dev *dev)
{
    (void)dev;

    /* ---------------------------------------------------------------
     * VI DU A - i.MX8MP  (tuong ung dwc_eth_qos_imx.c)
     * ---------------------------------------------------------------
     * CCM: bat clock ENET_QOS
     *   writel(BIT(0), CCM_CCGR(59));
     *
     * IOMUXC_GPR: chon RGMII + bat TX clock ra chan
     *   #define GPR1_ENET_QOS_INTF_SEL_RGMII  (1 << 16)
     *   #define GPR1_ENET_QOS_CLK_GEN_EN      (1 << 19)
     *   #define GPR1_ENET_QOS_RGMII_EN        (1 << 21)
     *   clrsetbits(IOMUXC_GPR + 0x04, GPR1_ENET_QOS_INTF_SEL_MASK,
     *              GPR1_ENET_QOS_INTF_SEL_RGMII |
     *              GPR1_ENET_QOS_CLK_GEN_EN |
     *              GPR1_ENET_QOS_RGMII_EN);
     *
     * GPIO reset PHY:
     *   gpio_set(PHY_RESET_PIN, 0);
     *   eqos_udelay(20000);
     *   gpio_set(PHY_RESET_PIN, 1);
     *   eqos_udelay(150000);   // theo datasheet PHY
     */

    /* ---------------------------------------------------------------
     * VI DU B - STM32MP15  (tuong ung dwc_eth_qos_stm32.c)
     * ---------------------------------------------------------------
     * RCC: bat ETHCK/ETHTX/ETHRX/ETHMAC, thoat reset
     *
     * SYSCFG_PMCSETR (0x50020004) - chon PHY interface:
     *   #define ETH_SEL_GMII_MII   (0 << 21)
     *   #define ETH_SEL_RGMII      (1 << 21)
     *   #define ETH_SEL_RMII       (4 << 21)
     *   #define ETH_CLK_SEL        (1 << 16)   // dung clock noi tu RCC
     *   #define ETH_REF_CLK_SEL    (1 << 17)   // RMII 50MHz tu RCC
     *   writel(ETH_SEL_MASK, SYSCFG_PMCCLRR);  // xoa truoc
     *   writel(ETH_SEL_RGMII | ETH_CLK_SEL, SYSCFG_PMCSETR);
     */

    /* ---------------------------------------------------------------
     * VI DU C - RK3568  (tuong ung dwc_eth_qos_rockchip.c)
     * ---------------------------------------------------------------
     * GRF dung "hiword mask": bit cao 16 la mask cho bit thap 16.
     *   #define GRF_BIT(n)      (BIT(n) | BIT((n) + 16))
     *   #define GRF_CLR_BIT(n)  (BIT((n) + 16))
     *   #define HIWORD(val, mask, sh) ((val) << (sh) | (mask) << ((sh)+16))
     *
     * Chon RGMII cho gmac0:
     *   writel(GRF_BIT(4) | GRF_CLR_BIT(5) | GRF_CLR_BIT(6),
     *          GRF + RK3568_GRF_GMAC0_CON1);
     * Dat delay line TX/RX (gia tri tuy board, tra device tree cua Linux):
     *   writel(HIWORD(0x30, 0x7f, 0) | HIWORD(0x10, 0x7f, 8) |
     *          GRF_BIT(0) | GRF_BIT(1),
     *          GRF + RK3568_GRF_GMAC0_CON0);
     */

    return 0;
}

/* ================================================================== */
/* 5. TX clock theo toc do                                             */
/* ================================================================== */

/*
 * Goi sau khi PHY thoa thuan xong, moi lan toc do doi.
 *
 *   RGMII: 1000M -> 125 MHz, 100M -> 25 MHz, 10M -> 2.5 MHz
 *   RMII :          100M -> 50 MHz, 10M -> 5 MHz   (chi 1 clock 50MHz,
 *                   MAC tu chia; nhieu SoC khong can lam gi)
 *
 * Neu clock tree cua ban tu xu ly (STM32) thi cu return 0.
 */
int eqos_board_set_tx_clk(struct eqos_dev *dev, enum eqos_speed speed)
{
    (void)dev;

    switch (speed) {
    case EQOS_SPEED_1000:
        /* TODO: dat tx clock = 125000000 */
        break;
    case EQOS_SPEED_100:
        /* TODO: dat tx clock = 25000000 (RGMII) hoac 50000000 (RMII) */
        break;
    case EQOS_SPEED_10:
        /* TODO: dat tx clock = 2500000 (RGMII) hoac 5000000 (RMII) */
        break;
    default:
        return EQOS_EINVAL;
    }

    return 0;
}

/* ================================================================== */
/* 6. Quirk reset                                                      */
/* ================================================================== */

/*
 * Chen vao GIUA luc ghi DMA_MODE.SWR = 1 va luc cho bit do tu clear.
 * Hau het SoC de rong ham nay.
 */
void eqos_board_fix_soc_reset(struct eqos_dev *dev)
{
    (void)dev;

#if 0 /* i.MX93 errata ERR051683: che do RMII can set PS|FES thi SWR moi xong */
    if (dev->cfg.phy_iface == EQOS_IFACE_RMII) {
        eqos_udelay(200);
        *(volatile uint32_t *)(dev->base + EQOS_MAC_CONFIGURATION) |=
            EQOS_MAC_CFG_PS | EQOS_MAC_CFG_FES;
    }
#endif
}
