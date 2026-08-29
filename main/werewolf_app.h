#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp_button.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the fixed-capacity controller task and creates the initial LVGL
 * screen. The function is idempotent only after a full firmware restart. */
esp_err_t werewolf_app_start(void);

/* Safe to call from the BSP button task. It briefly owns the LVGL lock,
 * translates the physical event and then queues any action without blocking. */
void werewolf_app_handle_button(bsp_btn_t button, bsp_btn_ev_t event);

/* Non-blocking hardware health publication. Both functions are safe from the
 * dedicated audio/power tasks: the controller remains the only UI writer. */
void werewolf_app_report_battery(bool available, bool stale, uint8_t percent);
void werewolf_app_report_audio(bool checked, bool available);

/* Replaces the initial page with a non-recoverable input error. This does not
 * initialize networking or erase persistent storage. */
void werewolf_app_report_input_failure(void);

#ifdef __cplusplus
}
#endif
