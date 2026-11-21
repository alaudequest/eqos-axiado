/*
 * test_loopback.h - phan TEST cua bai MAC loopback: [9] dung frame thu nghiem,
 * [10] gui, [11] nhan + so voi frame da gui. Than ham: test_loopback.c (cung thu muc).
 */
#ifndef TEST_LOOPBACK_H
#define TEST_LOOPBACK_H

#include <stdint.h>

// Kich thuoc bo dem frame thu nghiem: 14 byte header + payload (test dung 2300)
#define TEST_FRAME_MAX 2304U

// PART 4 - TEST: frame thu nghiem
struct test_cfg {
  uint8_t dest[6];         // dia chi dich
  uint16_t ethertype;      // EtherType, ghi vao frame theo thu tu mang (byte cao truoc)
  const uint8_t *payload;  // du lieu sau EtherType
  uint32_t payload_len;    // so byte payload
};

void test_build_frame(const struct test_cfg *c, const uint8_t *src_mac);  // [9] dung frame thu nghiem
void test_send_once(void);                                               // [10] gui frame, in ket qua + bo dem MMC
void test_poll_rx(void);                                                 // [11] in frame nhan, so voi frame da gui

#endif /* TEST_LOOPBACK_H */
