#pragma once

#include "driver/i2c_master.h"
#include "pmic.h"
#include "epd.h"

/*
 * tests_main — top-level test-mode entry point.
 *
 * Called from app_main() when CONFIG_TEST_MODE=y.  Powers on the EPD,
 * initialises it, runs the full diagnostic suite, tears everything down,
 * then returns.  app_main() halts after this returns (no deep sleep).
 */
void tests_main(pmic_handle_t pmic, i2c_master_bus_handle_t bus);

/*
 * tests_run — execute the full diagnostic suite and log results.
 *
 * Called by tests_main() after EPD init.  Both the PMIC and EPD handles
 * must be initialised before invoking this function.  See tests.c for details.
 */
void tests_run(pmic_handle_t pmic, epd_handle_t epd);
