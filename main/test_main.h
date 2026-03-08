#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the hardware integration test suite.
 *
 * Called from app_main() when CONFIG_TEST_MODE=y.
 * Tests are run sequentially; each logs PASS/FAIL via ESP_LOG.
 */
void tests_run(void);

#ifdef __cplusplus
}
#endif
