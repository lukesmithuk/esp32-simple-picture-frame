#pragma once

#include <stdbool.h>
#include <time.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pcf85063_init(i2c_master_bus_handle_t i2c_bus);
esp_err_t pcf85063_read_time(time_t *time_out);
esp_err_t pcf85063_write_time(time_t time_in);
bool      pcf85063_is_available(void);

#ifdef __cplusplus
}
#endif
