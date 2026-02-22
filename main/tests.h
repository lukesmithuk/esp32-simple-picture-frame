#pragma once

#include "pmic.h"

/*
 * tests_run — execute the full diagnostic suite and log results.
 *
 * Called from app_main() when CONFIG_TEST_MODE=y.  Does not enter deep sleep.
 * See tests.c for details.
 */
void tests_run(pmic_handle_t pmic);
