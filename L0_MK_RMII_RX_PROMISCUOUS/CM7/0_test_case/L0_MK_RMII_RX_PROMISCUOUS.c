/* Promiscuous-disabled external RJ45 loopback test.
 * A passive loopback preserves DA/SA; accepted frames must target this MAC.
 */
#include "eth_test_case_config.h"

#if defined(TEST_RMII_RX_PROMISCUOUS_DISABLE) && TEST_RMII_RX_PROMISCUOUS_DISABLE
#include "L0_MK_RMII_RX_PROMISCUOUS.h"
#include "stm32h7xx_hal.h"
#include "../baremetal/eqos.h"
#include "../baremetal/eqos_hw.h"
#include <stdio.h>
#include <string.h>

enum test_phase { TEST_IDLE, WAIT_OWN_BEFORE, WAIT_FOREIGN, WAIT_OWN_AFTER };
static struct eqos_dev eth;
static enum test_phase phase;
static volatile uint8_t run_requested;
static uint32_t run_number;
static uint32_t phase_start;
static uint8_t expected[60]; /* Minimum Ethernet frame excluding FCS. */
static uint8_t foreign_frame[60];
static uint8_t received[EQOS_BUF_SIZE];
static const uint8_t foreign_mac[6] = {0x6C, 0x1F, 0xF7, 0xCA, 0x2B, 0xFD};

static void make_frame(uint8_t *frame, const uint8_t *destination, uint8_t step)
{
  static const uint8_t payload[] = {0x17, 0x17, 0x17, 0x69, 0x29, 0x36, 0x98, 0x99};
  memset(frame, 0, sizeof(expected));
  memcpy(frame, destination, 6);
  memcpy(frame + 6, eth.cfg.mac_addr, 6);
  frame[12] = 0x88;
  frame[13] = 0xB5;
  memcpy(frame + 14, payload, sizeof(payload));
  memcpy(frame + 22, "PR0!", 4);
  /* Sequence/step prevent a stale packet from satisfying the next check. */
  frame[26] = (uint8_t)(run_number >> 24);
  frame[27] = (uint8_t)(run_number >> 16);
  frame[28] = (uint8_t)(run_number >> 8);
  frame[29] = (uint8_t)run_number;
  frame[30] = step;
}

static void finish(bool passed, const char *reason)
{
  phase = TEST_IDLE;
  printf("Promiscuous_disable run %lu: %s - %s\r\n",
         (unsigned long)run_number, passed ? "PASS" : "FAIL", reason);
  eqos_dump_regs(&eth);
}

static void send_step(enum test_phase next)
{
  phase = next;
  make_frame(expected, next == WAIT_FOREIGN ? foreign_mac : eth.cfg.mac_addr,
             (uint8_t)next);
  if (next == WAIT_FOREIGN)
    memcpy(foreign_frame, expected, sizeof(expected));
  int ret = eqos_send(&eth, expected, sizeof(expected));
  if (ret != EQOS_OK) {
    printf("TX failed: %d\r\n", ret);
    finish(false, "transmit failed; reset the board after a TX timeout");
    return;
  }
  phase_start = HAL_GetTick();
}

int promiscuous_disable_test_init(void)
{
  const struct eqos_cfg cfg = {
    .base = EQOS_BASE_ADDR,
    .csr_clk_hz = HAL_RCC_GetHCLKFreq(),
    .phy_addr = EQOS_PHY_ADDR_AUTO,
    .phy_iface = EQOS_IFACE_RMII,
    .mac_addr = {0x00, 0x80, 0xE1, 0x00, 0x00, 0x00},
    .max_speed = 100,
    .promiscuous = false,
    .loopback = false,
    .aneg_timeout_ms = 5000,
  };
  printf("Promiscuous_disable: connect RJ45 TX-to-RX loopback before reset.\r\n");
  int ret = eqos_init(&eth, &cfg);
  if (ret != EQOS_OK) return ret;
  if (!eth.duplex_full) {
    printf("External simultaneous TX/RX test requires full duplex.\r\n");
    return EQOS_ENOLINK;
  }
  eqos_dump_regs(&eth);
  printf("B1 repeats the test. Own DA must pass; foreign DA must be filtered.\r\n");
  run_requested = 1U;
  return EQOS_OK;
}

void promiscuous_disable_test_request(void)
{
  run_requested = 1U;
}

void promiscuous_disable_test_poll(void)
{
  if (phase == TEST_IDLE) {
    /* Consume atomically with respect to the B1 interrupt. Button presses
     * during a run coalesce into one subsequent run. */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint8_t requested = run_requested;
    run_requested = 0;
    __set_PRIMASK(primask);
    if (requested) {
      ++run_number;
      memset(foreign_frame, 0, sizeof(foreign_frame));
      if (eqos_set_promiscuous(&eth, false) != EQOS_OK) {
        finish(false, "cannot disable promiscuous mode");
        return;
      }
      printf("Run %lu: checking own DA, foreign DA, then own DA again\r\n",
             (unsigned long)run_number);
      send_step(WAIT_OWN_BEFORE);
    }
  }

  uint32_t length;
  int ret = eqos_receive(&eth, received, sizeof(received), &length);
  if (ret != EQOS_OK && ret != EQOS_EAGAIN) {
    if (phase != TEST_IDLE) finish(false, "receive descriptor error");
    else printf("RX error: %d\r\n", ret);
    return;
  }
  if (ret == EQOS_OK) {
    if (length == sizeof(foreign_frame) &&
        memcmp(received, foreign_frame, sizeof(foreign_frame)) == 0) {
      finish(false, "foreign destination was delivered with PR=0");
      return;
    }
    if (phase != TEST_IDLE && length == sizeof(expected) &&
        memcmp(received, expected, sizeof(expected)) == 0) {
      if (phase == WAIT_OWN_BEFORE) {
        send_step(WAIT_FOREIGN);
      } else if (phase == WAIT_OWN_AFTER) {
        finish(true, "own DA received before/after; foreign DA absent in receive window");
      }
      return;
    }
    /* Keep PC-injected traffic observable without treating unrelated packets
     * as proof of loopback. Full dumps are limited to idle time. */
    if (phase == TEST_IDLE) {
      printf("RX: %lu bytes\r\n", (unsigned long)length);
      for (uint32_t i = 0; i < length; ++i) {
        printf("%02X ", (unsigned int)received[i]);
        if ((i + 1U) % 16U == 0U) printf("\r\n");
      }
      printf("\r\n");
    }
  }

  if (phase != TEST_IDLE &&
      (uint32_t)(HAL_GetTick() - phase_start) >= ETH_TEST_RX_TIMEOUT_MS) {
    if (phase == WAIT_FOREIGN) {
      /* No negative-only PASS: prove RX still works after the rejection. */
      send_step(WAIT_OWN_AFTER);
    } else {
      finish(false, "own destination not received; check link, loopback and DMA");
    }
  }
}
#endif
