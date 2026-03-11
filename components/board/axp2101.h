#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t axp2101_init(void);
esp_err_t axp2101_cmd_init(void);
esp_err_t axp2101_epd_power(bool enable);
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
