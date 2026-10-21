#ifndef ETH_FUNCTIONS_UTILS_H
#define ETH_FUNCTIONS_UTILS_H

#include "../../baremetal/eqos.h"
#include "lan8742.h"

#define ETH_TEST_FRAME_MIN_SIZE 20U
#define ETH_TEST_FRAME_MAX_SIZE 9014U /* 14-byte header + 9000-byte payload. */
#define ETH_TEST_VLAN_FRAME_MAX_SIZE 9018U /* 18-byte header + 9000-byte payload. */

/* Size includes the Ethernet header, excludes FCS/padding/preamble.
 * Repeat payload_pattern to fill the requested size. Return size on success,
 * zero on invalid arguments; invalid requests leave the buffer untouched.
 * Construction does not configure MAC jumbo support or RX buffers. */
uint32_t eth_construct_frame(uint8_t *buffer, size_t capacity,
                             uint32_t size, const uint8_t dest_mac[6],
                             const uint8_t src_mac[6], uint16_t ether_type,
                             const uint8_t *payload_pattern, size_t pattern_size);

/* Single 802.1Q tag (TPID 0x8100). Size includes the 18-byte header,
 * excludes generated FCS/padding. VID 0..4094, PCP 0..7, DEI 0..1.
 * Returns size, or zero without modifying buffer on invalid arguments.
 * Input arrays must not overlap the output buffer. */
uint32_t eth_construct_vlan_frame(uint8_t *buffer, size_t capacity,
                                  uint32_t size, const uint8_t dest_mac[6],
                                  const uint8_t src_mac[6], uint16_t ether_type,
                                  uint16_t vlan_id, uint8_t pcp, uint8_t dei,
                                  const uint8_t *payload_pattern, size_t pattern_size);

/* Prepare frame and filtering before starting the MAC. Returns frame length. */
uint32_t eth_test_vlan_filter(struct eqos_dev *eth, uint8_t *buffer,
                              size_t capacity, uint16_t tx_vlan_id);

void eth_mac_init();
/* Print HWF1 selected fields and all HWF2 fields; requires ETH clocks and UART. */
void eth_print_hw_features(void);
uint32_t eth_test_drop_non_tcp_udp(struct eqos_dev *eth, uint8_t *buffer,
                                  size_t capacity, uint8_t protocol, bool enable);
uint32_t eth_test_mac_rx_crc_error(struct eqos_dev *eth, uint8_t *buffer,
                                  size_t capacity);
uint32_t eth_test_crc_tx_control(void);
void eth_test_crc_report(void);
void eth_dma_init();
void eth_mtl_init();
void eth_test_enable_promiscuous(struct eqos_cfg* eth);
/* Call after LAN8742_Init and before configuring the MAC. */
int32_t eth_test_enable_phy_loopback(struct eqos_dev *eth, lan8742_Object_t *phy);

/* Enable ARP offload for DUT_IP_ADDR; call before TE/RE and keep ARPEN set. */
void eth_test_enable_arp_offload(void);
/* DMA normal descriptor, 4 x 32-bit words (16 bytes). Shared by main.c,
 * eth_functions.c and the test cases. volatile prevents compiler-cached status
 * reads; it is NOT cache coherency (D-cache stays disabled). */
struct eqos_desc {
    volatile uint32_t des0, des1, des2, des3;
};

/* Result of eth_rx_frame_receive(): status words come from the LD descriptor. */
struct eth_rx_frame {
    uint32_t len;           /* total length from LD PL (after CST/ACS stripping) */
    uint32_t rdes1;         /* LD RDES1: IP/checksum status */
    uint32_t rdes2;         /* LD RDES2: valid only when RDES3.RS2V=1 */
    uint32_t rdes3;         /* LD RDES3: ES, GP, CE, RWT, OE, LT, PL */
    uint32_t desc_used;     /* data descriptors consumed by this frame */
    uint32_t desc_dropped;  /* stray/truncated descriptors discarded */
};

/* Receive one frame of any size into out[]. Returns EQOS_OK (check info->rdes3
 * for errors), EQOS_ETIMEDOUT (no frame started), or EQOS_EIO (frame stalled
 * mid-way). first_timeout_us = 0 polls once without waiting.
 * See the comment above the definition in eth_functions.c for the sequence. */
int eth_rx_frame_receive(struct eqos_dev *eth, struct eqos_desc *rx_desc,
                         uint8_t rx_buf[][EQOS_BUF_SIZE],
                         uint8_t *out, size_t out_size,
                         uint32_t first_timeout_us, struct eth_rx_frame *info);

/* Result of eth_tx_frame_send(): status is the LD descriptor write-back. */
struct eth_tx_frame {
    uint32_t tdes3;      /* LD TDES3 write-back: ES, JT, FF, LoC, NC, LC, EC, UF, IHE */
    uint32_t desc_used;  /* descriptors used for this frame */
};

/* Send one frame of any size directly from frame[] (zero copy) and wait for
 * completion. piece_max = 0 uses the 16383-byte maximum; fd_ctrl is ORed into
 * the first descriptor (CPC, SAIC, CIC). Returns EQOS_OK (check info->tdes3)
 * or EQOS_ETIMEDOUT. See the comment above the definition in eth_functions.c. */
int eth_tx_frame_send(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                      const uint8_t *frame, uint32_t len, uint32_t piece_max,
                      uint32_t fd_ctrl, uint32_t timeout_us,
                      struct eth_tx_frame *info);

/* Read-modify-write one register: (old & ~clear_mask) | set_mask, one write,
 * one readback; compare only the touched bits and print the result.
 * Returns true if the readback matches. */
bool eth_reg_update(const char *name, uint32_t addr,
                    uint32_t clear_mask, uint32_t set_mask);

/* Usage: ETH_REG_UPDATE(EQOS_MAC_BASE, EQOS_MAC_CONFIGURATION, clr, set)
 * prints "EQOS_MAC_CONFIGURATION [0x40028000]: old=... write=... readback=... OK" */
#define ETH_REG_UPDATE(base, offset, clear_mask, set_mask) \
    eth_reg_update(#offset, (base) + (offset), (clear_mask), (set_mask))
/* SARC[30:28] test: MAC loopback, cases 010/011/110/111, checks RX SA.
 * Call after TE/RE and DMA ST/SR are started. Returns failure count, -1 on setup error. */
int eth_test_mac_ctrl_sarc(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                           struct eqos_desc *rx_desc,
                           uint8_t tx_buf[][EQOS_BUF_SIZE],
                           uint8_t rx_buf[][EQOS_BUF_SIZE]);
/* GPSLCE test: GPSL=3000, MAC loopback, 3000 B (GP=0) and 3001 B (GP=1) on wire.
 * Call after TE/RE and DMA ST/SR are started. Returns failure count, -1 on setup error. */
int eth_test_mac_ctrl_gpslce(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                             struct eqos_desc *rx_desc,
                             uint8_t rx_buf[][EQOS_BUF_SIZE]);
/* MACECR USP test: PHY loopback, SPEN=1, unicast Slow Protocol frame with
 * USP=0 (LT=type) and USP=1 (LT=OAM). Returns failure count, -1 on setup error. */
int eth_test_mac_ecr_usp(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                         struct eqos_desc *rx_desc,
                         uint8_t rx_buf[][EQOS_BUF_SIZE]);
/* MACPFR HUC test: MAC loopback, unicast DA via hash table only; hash hit
 * (received, HF=1) and miss (dropped). Returns failure count, -1 on setup error. */
int eth_test_mac_pfr_hash_uc(struct eqos_dev *eth, struct eqos_desc *tx_desc,
                             struct eqos_desc *rx_desc,
                             uint8_t rx_buf[][EQOS_BUF_SIZE]);

/* 64-bit hash table index of a MAC address: CRC-32 (Ethernet polynomial) of
 * the 6 address bytes, then the upper 6 bits of the bit-reversed CRC.
 * Index 0..31 -> ETH_MACHT0R bit idx, 32..63 -> ETH_MACHT1R bit idx-32. */
uint32_t eth_mac_hash_index(const uint8_t addr[6]);
// /* MAC control smoke-test flow selected by ACTIVE_TEST. */
// uint32_t eth_mac_ctrl_configure(uint32_t control);
// void eth_mac_ctrl_test_run(struct eqos_dev *eth, struct eqos_desc *tx_desc,
//                            struct eqos_desc *rx_desc,
//                            uint8_t tx_buf[][EQOS_BUF_SIZE],
//                            uint8_t rx_buf[][EQOS_BUF_SIZE],
//                            uint8_t *frame, uint32_t *frame_len);
void eth_dump_rx_status(uint32_t rdes2,
                        uint32_t rdes3);

#endif
