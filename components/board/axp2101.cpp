#include "axp2101.h"
#include "board.h"

#include <stdio.h>
#include <string.h>

#include "XPowersLib/XPowersLib.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "axp2101";

static XPowersPMU axp2101;

static int AXP2101_SLAVE_Read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    return board_bb_i2c_read(devAddr, regAddr, data, len);
}

static int AXP2101_SLAVE_Write(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    return board_bb_i2c_write(devAddr, regAddr, data, len);
}

esp_err_t axp2101_init(void)
{
    if (axp2101.begin(AXP2101_SLAVE_ADDRESS, AXP2101_SLAVE_Read, AXP2101_SLAVE_Write)) {
        ESP_LOGI(TAG, "Init PMU SUCCESS!");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Init PMU FAILED!");
    return ESP_FAIL;
}

esp_err_t axp2101_cmd_init(void)
{
    axp2101.disableTSPinMeasure();

    int data = axp2101.readRegister(0x26);
    ESP_LOGW(TAG, "reg_26:0x%02x", data);
    if (data & 0x01) {
        axp2101.enableWakeup();
        ESP_LOGW(TAG, "i2c_wakeup");
    }
    if (data & 0x08) {
        axp2101.wakeupControl(XPOWERS_AXP2101_WAKEUP_PWROK_TO_LOW, false);
        ESP_LOGW(TAG, "pwrok pull-down disabled");
    }

    if (axp2101.getPowerKeyPressOffTime() != XPOWERS_POWEROFF_4S) {
        axp2101.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
        ESP_LOGW(TAG, "PWR hold 4s = power off");
    }
    if (axp2101.getPowerKeyPressOnTime() != XPOWERS_POWERON_128MS) {
        axp2101.setPowerKeyPressOnTime(XPOWERS_POWERON_128MS);
        ESP_LOGW(TAG, "PWR click = power on");
    }
    if (axp2101.getChargingLedMode() != XPOWERS_CHG_LED_OFF) {
        axp2101.setChargingLedMode(XPOWERS_CHG_LED_OFF);
        ESP_LOGW(TAG, "CHGLED disabled");
    }
    if (axp2101.getChargeTargetVoltage() != XPOWERS_AXP2101_CHG_VOL_4V2) {
        axp2101.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
        ESP_LOGW(TAG, "Charge target = 4.2V");
    }
    if (axp2101.getVbusCurrentLimit() != XPOWERS_AXP2101_VBUS_CUR_LIM_500MA) {
        axp2101.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_500MA);
        ESP_LOGW(TAG, "VBUS current limit = 500mA");
    }
    if (axp2101.getChargerConstantCurr() != XPOWERS_AXP2101_CHG_CUR_500MA) {
        axp2101.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
        ESP_LOGW(TAG, "Charge current = 500mA");
    }
    if (axp2101.getButtonBatteryVoltage() != 3300) {
        axp2101.setButtonBatteryChargeVoltage(3300);
        ESP_LOGW(TAG, "Button battery charge voltage = 3.3V");
    }
    if (axp2101.isEnableButtonBatteryCharge() == 0) {
        axp2101.enableButtonBatteryCharge();
        ESP_LOGW(TAG, "Button battery charge enabled");
    }
    if (axp2101.getDC1Voltage() != 3300) {
        axp2101.setDC1Voltage(3300);
        ESP_LOGW(TAG, "DCDC1 = 3.3V");
    }
    if (axp2101.getALDO3Voltage() != 3300) {
        axp2101.setALDO3Voltage(3300);
        ESP_LOGW(TAG, "ALDO3 voltage = 3.3V");
    }
    if (axp2101.getALDO4Voltage() != 3300) {
        axp2101.setALDO4Voltage(3300);
        ESP_LOGW(TAG, "ALDO4 voltage = 3.3V");
    }
    if (axp2101.getSysPowerDownVoltage() != 2900) {
        axp2101.setSysPowerDownVoltage(2900);
        ESP_LOGW(TAG, "VOFF = 2.9V (battery UVLO)");
    }

    /* Enable battery voltage ADC and battery detection. */
    axp2101.enableBattVoltageMeasure();
    axp2101.enableBattDetection();
    axp2101.enableSystemVoltageMeasure();
    axp2101.enableVbusVoltageMeasure();

    return ESP_OK;
}

esp_err_t axp2101_epd_power(bool enable)
{
    bool ok;
    if (enable) {
        ok = axp2101.enableALDO3();
        ESP_LOGI(TAG, "EPD power ON (ALDO3)");
    } else {
        ok = axp2101.disableALDO3();
        ESP_LOGI(TAG, "EPD power OFF (ALDO3)");
    }
    return ok ? ESP_OK : ESP_FAIL;
}

void axp2101_basic_sleep_start(void)
{
    axp2101.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    axp2101.clearIrqStatus();

    int power_value = axp2101.readRegister(0x26);
    ESP_LOGW(TAG, "reg_26:0x%02x", power_value);

    if (!(power_value & 0x04)) {
        axp2101.wakeupControl(XPOWERS_AXP2101_WAKEUP_DC_DLO_SELECT, true);
        ESP_LOGW(TAG, "Wake power state = same as pre-sleep");
    }
    if ((power_value & 0x08)) {
        axp2101.wakeupControl(XPOWERS_AXP2101_WAKEUP_PWROK_TO_LOW, false);
        ESP_LOGW(TAG, "pwrok pull-down disabled");
    }
    if (!(power_value & 0x10)) {
        axp2101.wakeupControl(XPOWERS_AXP2101_WAKEUP_IRQ_PIN_TO_LOW, true);
        ESP_LOGW(TAG, "Wake source = AXP IRQ pin");
    }

    axp2101.enableSleep();

    power_value = axp2101.readRegister(0x26);
    ESP_LOGW(TAG, "reg_26 post-sleep-enable: 0x%02x", power_value);

    // Disable rails (DLDO1/DLDO2 cut USB-JTAG serial — see project memory)
    axp2101.disableDC2();
    axp2101.disableDC3();
    axp2101.disableDC4();
    axp2101.disableDC5();
    axp2101.disableALDO1();
    axp2101.disableALDO2();
    axp2101.disableBLDO1();
    axp2101.disableBLDO2();
    axp2101.disableCPUSLDO();
    axp2101.disableDLDO1();
    axp2101.disableDLDO2();
    axp2101.disableALDO4();
    axp2101.disableALDO3();
}

int axp2101_get_battery_percent(void)
{
    if (axp2101.isBatteryConnect()) {
        return axp2101.getBatteryPercent();
    }
    return -1;
}

int axp2101_get_battery_voltage(void)
{
    return axp2101.getBattVoltage();
}

bool axp2101_is_charging(void)
{
    return axp2101.isCharging();
}

bool axp2101_is_battery_connected(void)
{
    return axp2101.isBatteryConnect();
}

bool axp2101_is_usb_connected(void)
{
    return axp2101.isVbusIn();
}

void axp2101_shutdown(void)
{
    ESP_LOGI(TAG, "Triggering hard power-off via AXP2101");
    axp2101.shutdown();
}
