#ifndef L0_MK_RMII_RX_PROMISCUOUS_H
#define L0_MK_RMII_RX_PROMISCUOUS_H

int promiscuous_disable_test_init(void);
void promiscuous_disable_test_poll(void);
/* ISR-safe: only latches a request; the test runs from poll(). */
void promiscuous_disable_test_request(void);

#endif
