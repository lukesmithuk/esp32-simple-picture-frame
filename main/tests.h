#pragma once

#include "pmic.h"
#include "epd.h"

/*
 * tests_run — execute the full diagnostic suite and log results.
 *
 * Called from app_main() when CONFIG_TEST_MODE=y.  Does not enter deep sleep.
 * Both the PMIC and EPD handles must be initialised by the caller before
 * invoking this function.  See tests.c for details.
 */
void tests_run(pmic_handle_t pmic, epd_handle_t epd);
