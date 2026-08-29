// components/bsp/include/bsp_battery.h
// CellWise CW2017 电量计:I2C 0x63,与 ES8311 共用总线。
// 驱动使用主线固件的 4.2V/520mAh profile，直接读取芯片 SOC%，不按电压查表。
#pragma once

#include "esp_err.h"

// 初始化。内部会调 bsp_i2c_init()(幂等)。
// 芯片不应答时返回 ESP_ERR_NOT_FOUND —— 上层可据此在 UI 上标记该项不可用。
esp_err_t bsp_battery_init(void);

// 在同一次上层采样中读取芯片计算的 SOC 和电压；SOC 直接来自
// 0x04 高字节，不根据电压推算。
// SOC 尚未就绪时 *soc 返回 -1，但只要寄存器读取成功仍返回 ESP_OK。
esp_err_t bsp_battery_read(int *soc, int *millivolts);

// 剩余电量百分比 0..100;读失败返回 -1。
int bsp_battery_soc(void);

// 电池电压 mV;读失败返回 -1。
int bsp_battery_mv(void);
