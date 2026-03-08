#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AXP2101_SLAVE_ADDRESS 0x34

void axp2101_init(i2c_master_bus_handle_t i2c_bus);
void axp2101_cmd_init(void);
void axp2101_epd_power(bool enable);
void axp2101_basic_sleep_start(void);

int  axp2101_get_battery_percent(void);
int  axp2101_get_battery_voltage(void);
bool axp2101_is_charging(void);
bool axp2101_is_battery_connected(void);
bool axp2101_is_usb_connected(void);
void axp2101_shutdown(void);

#ifdef __cplusplus
}
#endif
