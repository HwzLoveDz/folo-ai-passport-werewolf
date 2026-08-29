#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the static, low-priority CW2017 monitor. Sampling happens outside the
 * controller and LVGL tasks; results are reported as bounded values through
 * werewolf_app_report_battery(). */
esp_err_t werewolf_power_start(void);

#ifdef __cplusplus
}
#endif
