// components/bsp/src/bsp_battery.c
// 移植自主线 components/platform/platform_esp32/src/battery_cw2017.c。
// 使用当前硬件已验证的 4.2V/520mAh BATINFO profile；SOC 始终直接读取芯片结果。
#include "bsp_battery.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const char *TAG = "bsp_batt";

#define CW_REG_VERSION   0x00   // 版本号,上电应答即代表芯片在位
#define CW_REG_VCELL_H   0x02   // 14bit 电压,V(uV) = raw * 312.5
#define CW_REG_SOC_H     0x04   // 高字节 = 整数百分比;低字节(0x05)= 1/256 %
#define CW_REG_CONFIG    0x08   // 0xF0=睡眠 / 0x30=复位态 / 0x00=正常
#define CW_REG_SOC_ALERT 0x0B   // bit7=UPDATE_FLAG(profile 已写标记)
#define CW_REG_BATINFO   0x10   // 电池 profile 区 0x10..0x5F
#define CW_CONFIG_STATE_MASK 0xF0
#define CW_CONFIG_RESTART 0x30
#define CW_CONFIG_NORMAL  0x00
#define CW_PROFILE_UPDATE_FLAG 0x80
#define CW_PROFILE_LEN 80

// 主线固件当前选用的 4.2V/520mAh profile。缺少这组参数时，芯片虽能
// 正常给出 VCELL，SOC 却会长期停在 0xFE 等无效值。
static const uint8_t s_battery_profile[CW_PROFILE_LEN] = {
    0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAD, 0xC7, 0xC8, 0xCA, 0xBD, 0xB1, 0xC1, 0x94,
    0x88, 0xD1, 0xBD, 0x97, 0x88, 0x66, 0x56, 0x4A,
    0x3F, 0x33, 0x26, 0x5C, 0x37, 0xD1, 0x27, 0xD8,
    0xCC, 0xB7, 0xCF, 0xB3, 0xB2, 0xAE, 0xA6, 0x9E,
    0x99, 0x97, 0x9B, 0x86, 0x47, 0x1E, 0x17, 0x26,
    0x49, 0x96, 0xD9, 0xE1, 0xDD, 0xDC, 0xD4, 0x59,
    0x00, 0x00, 0x90, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5C,
};

static i2c_master_dev_handle_t s_dev;

static esp_err_t cw_read(uint8_t reg, uint8_t *buf, size_t n) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t cw_write(uint8_t reg, uint8_t val) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100);
}

static esp_err_t cw_init_failed(esp_err_t error) {
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    return error;
}

static bool cw_profile_matches(void) {
    uint8_t alert = 0;
    if (cw_read(CW_REG_SOC_ALERT, &alert, 1) != ESP_OK ||
        (alert & CW_PROFILE_UPDATE_FLAG) == 0U) {
        return false;
    }
    for (size_t i = 0; i < CW_PROFILE_LEN; ++i) {
        uint8_t current = 0;
        if (cw_read((uint8_t)(CW_REG_BATINFO + i), &current, 1) != ESP_OK ||
            current != s_battery_profile[i]) {
            return false;
        }
    }
    return true;
}

static esp_err_t cw_write_profile(void) {
    esp_err_t error = cw_write(CW_REG_CONFIG, CW_CONFIG_RESTART);
    if (error != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(30));

    for (size_t i = 0; i < CW_PROFILE_LEN; ++i) {
        error = cw_write((uint8_t)(CW_REG_BATINFO + i), s_battery_profile[i]);
        if (error != ESP_OK) goto restore_normal;
    }

    error = cw_write(CW_REG_CONFIG, CW_CONFIG_NORMAL);
    if (error != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(30));

    // 写完逐字节校验后再置 UPDATE_FLAG；下次 MCU 重启即可保留芯片学习状态。
    for (size_t i = 0; i < CW_PROFILE_LEN; ++i) {
        uint8_t current = 0;
        error = cw_read((uint8_t)(CW_REG_BATINFO + i), &current, 1);
        if (error != ESP_OK) return error;
        if (current != s_battery_profile[i]) return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t alert = 0;
    error = cw_read(CW_REG_SOC_ALERT, &alert, 1);
    if (error != ESP_OK) return error;
    return cw_write(CW_REG_SOC_ALERT, alert | CW_PROFILE_UPDATE_FLAG);

restore_normal:
    (void)cw_write(CW_REG_CONFIG, CW_CONFIG_NORMAL);
    return error;
}

esp_err_t bsp_battery_init(void) {
    if (s_dev) return ESP_OK;

    esp_err_t e = bsp_i2c_init();
    if (e != ESP_OK) return e;

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BSP_I2C_CW2017_ADDR,
        .scl_speed_hz    = 100000,
    };
    e = i2c_master_bus_add_device(bsp_i2c_bus(), &dc, &s_dev);
    if (e != ESP_OK) { ESP_LOGE(TAG, "添加 I2C 设备失败: %s", esp_err_to_name(e)); return e; }

    uint8_t ver = 0;
    if (cw_read(CW_REG_VERSION, &ver, 1) != ESP_OK) {
        ESP_LOGW(TAG, "CW2017 未应答 —— 用 bsp_i2c_scan() 确认 0x%02X 是否在线;"
                      "无电量计的板子可忽略本项", BSP_I2C_CW2017_ADDR);
        return cw_init_failed(ESP_ERR_NOT_FOUND);
    }
    ESP_LOGI(TAG, "CW2017 VERSION=0x%02X", ver);

    uint8_t config = 0;
    e = cw_read(CW_REG_CONFIG, &config, 1);
    if (e != ESP_OK) return cw_init_failed(e);
    if (!cw_profile_matches()) {
        ESP_LOGI(TAG, "loading 4.2V/520mAh battery profile");
        e = cw_write_profile();
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "CW2017 profile write failed: %s", esp_err_to_name(e));
            return cw_init_failed(e);
        }
        config = CW_CONFIG_NORMAL;
    }

    // profile 已存在且芯片处于正常态时绝不重启算法；仅处理真正的睡眠/复位态。
    if ((config & CW_CONFIG_STATE_MASK) != 0U) {
        e = cw_write(CW_REG_CONFIG, CW_CONFIG_RESTART);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "CW2017 退出休眠失败: %s", esp_err_to_name(e));
            return cw_init_failed(e);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
        e = cw_write(CW_REG_CONFIG, CW_CONFIG_NORMAL);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "CW2017 重启计算失败: %s", esp_err_to_name(e));
            return cw_init_failed(e);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    // 首次装载 profile 后通常数百毫秒即可得到有效 SOC；最多等 2 秒，
    // 超时也不伪造数值，后台 30 秒采样会继续直接读取。
    for (int i = 0; i < 40; ++i) {
        uint8_t soc = 0xFF;
        if (cw_read(CW_REG_SOC_H, &soc, 1) == ESP_OK && soc <= 100U) {
            ESP_LOGI(TAG, "SOC ready: %u%%", soc);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return ESP_OK;
}

esp_err_t bsp_battery_read(int *soc, int *millivolts) {
    if (!soc || !millivolts) return ESP_ERR_INVALID_ARG;

    // 两个寄存器均按数据手册定义独立读取；SOC 高字节就是整数百分比。
    uint8_t voltage[2] = { 0 };
    uint8_t charge[2] = { 0 };
    esp_err_t e = cw_read(CW_REG_VCELL_H, voltage, sizeof(voltage));
    if (e != ESP_OK) return e;
    e = cw_read(CW_REG_SOC_H, charge, sizeof(charge));
    if (e != ESP_OK) return e;

    uint32_t raw = (((uint32_t)voltage[0] << 8) | voltage[1]) & 0x3FFF;
    *millivolts = (int)((raw * 3125U) / 10000U);
    *soc = charge[0] <= 100U ? charge[0] : -1;
    return ESP_OK;
}

int bsp_battery_soc(void) {
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_SOC_H, b, 2) != ESP_OK) return -1;
    int soc = b[0];                       // 高字节即整数百分比
    if (soc > 100) return -1;             // 芯片未就绪时可能读到 0xFF
    return soc;
}

int bsp_battery_mv(void) {
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_VCELL_H, b, 2) != ESP_OK) return -1;
    uint32_t raw = ((uint32_t)b[0] << 8 | b[1]) & 0x3FFF;   // 14bit
    return (int)((raw * 3125) / 10000);                     // raw * 312.5uV → mV
}
