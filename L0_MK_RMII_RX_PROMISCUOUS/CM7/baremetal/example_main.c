/* SPDX-License-Identifier: GPL-2.0 */
/* API example, excluded from CubeIDE builds.
 * Call after HAL_Init, system clocks and UART setup with D-cache disabled.
 * For the active application/test, see Core/Src/main.c and 0_test_case.
 */
#include "stm32h7xx_hal.h"
#include "eqos.h"
#include "eqos_hw.h"
#include <string.h>

static struct eqos_dev example_eth;
static uint8_t example_rx[EQOS_BUF_SIZE];

int eqos_example(void)
{
  const struct eqos_cfg cfg = {
    .base = EQOS_BASE_ADDR,
    .csr_clk_hz = HAL_RCC_GetHCLKFreq(),
    .phy_addr = EQOS_PHY_ADDR_AUTO,
    .phy_iface = EQOS_IFACE_RMII,
    .mac_addr = {0x00, 0x80, 0xE1, 0x00, 0x00, 0x00},
    .max_speed = 100,
    .promiscuous = false,
  };
  int ret = eqos_init(&example_eth, &cfg);
  if (ret != EQOS_OK) return ret;

  uint8_t frame[60] = {0};
  memcpy(frame, cfg.mac_addr, 6); /* External loopback destination. */
  memcpy(frame + 6, cfg.mac_addr, 6);
  frame[12] = 0x88;
  frame[13] = 0xB5;
  ret = eqos_send(&example_eth, frame, sizeof(frame));
  if (ret != EQOS_OK) return ret;

  uint32_t start = HAL_GetTick();
  do {
    uint32_t length;
    ret = eqos_receive(&example_eth, example_rx, sizeof(example_rx), &length);
    if (ret == EQOS_OK) {
      /* Data belongs to the caller; no descriptor-release API is needed. */
      return length == sizeof(frame) &&
             memcmp(frame, example_rx, sizeof(frame)) == 0 ? EQOS_OK : EQOS_EIO;
    }
    if (ret != EQOS_EAGAIN) return ret;
  } while ((uint32_t)(HAL_GetTick() - start) < 500U);
  return EQOS_ETIMEDOUT;
}
