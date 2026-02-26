#pragma once

#include "driver/i2c_master.h"
#include "pmic.h"

/*
 * tests_main — top-level test-mode entry point.
 *
 * Called from app_main() when CONFIG_TEST_MODE=y.  Runs the full diagnostic
 * suite, tears everything down, then returns.  app_main() halts after this
 * returns (no deep sleep).
 */
void tests_main(pmic_handle_t pmic, i2c_master_bus_handle_t bus);

/*
 * tests_run — execute the full diagnostic suite and log results.
 *
 * Called by tests_main().  Owns the EPD lifecycle internally: it initialises
 * the panel after PMIC tests have completed (so any ALDO3 power-cycling
 * during PMIC tests does not leave the panel in an uninitialised state).
 */
void tests_run(pmic_handle_t pmic);
