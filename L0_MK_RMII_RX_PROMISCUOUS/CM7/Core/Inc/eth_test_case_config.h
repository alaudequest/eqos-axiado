#ifndef ETH_TEST_CASE_CONFIG_H
#define ETH_TEST_CASE_CONFIG_H

/* Select a test here. Add another #elif branch in main.c for future tests.
 * Set to 0 to build the board-only idle application. */
#ifndef TEST_RMII_RX_PROMISCUOUS_DISABLE
#define TEST_RMII_RX_PROMISCUOUS_DISABLE 1
#endif

/* External RJ45 TX-to-RX loopback; this is not internal MAC loopback. */
#define ETH_TEST_RX_TIMEOUT_MS 500U

#endif /* ETH_TEST_CASE_CONFIG_H */




