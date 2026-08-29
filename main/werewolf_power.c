#include "werewolf_power.h"

#include <stdbool.h>
#include <stdint.h>

#include "bsp_battery.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "werewolf_app.h"

#define POWER_REFRESH_MS       30000U
#define POWER_TASK_STACK_BYTES  3072U
#define POWER_TASK_PRIORITY        2U

static const char *TAG = "werewolf_power";

static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[POWER_TASK_STACK_BYTES / sizeof(StackType_t)];
static TaskHandle_t s_task;
static bool s_battery_initialized;
static bool s_have_sample;
static bool s_failure_logged;
static uint8_t s_last_percent;

static void report_sample_failure(esp_err_t error, int soc, int millivolts)
{
    if (!s_failure_logged) {
        const char *reason = error == ESP_OK ? "invalid SOC"
                                             : esp_err_to_name(error);
        ESP_LOGW(TAG, "battery sample unavailable: %s, soc=%d, mv=%d",
                 reason, soc, millivolts);
        s_failure_logged = true;
    }

    /* Never derive a percentage from voltage. A transient failure keeps the
     * last chip-provided SOC but marks it stale; before the first valid sample
     * the UI gets an explicit unavailable state. */
    werewolf_app_report_battery(s_have_sample, s_have_sample, s_last_percent);
}

static void sample_once(void)
{
    int soc = -1;
    int millivolts = -1;
    esp_err_t error;

    if (!s_battery_initialized) {
        error = bsp_battery_init();
        if (error != ESP_OK) {
            report_sample_failure(error, soc, millivolts);
            return;
        }
        s_battery_initialized = true;
    }

    error = bsp_battery_read(&soc, &millivolts);
    if (error != ESP_OK || soc < 0 || soc > 100) {
        report_sample_failure(error, soc, millivolts);
        return;
    }

    s_have_sample = true;
    s_last_percent = (uint8_t)soc;
    s_failure_logged = false;
    werewolf_app_report_battery(true, false, s_last_percent);
}

static void power_task(void *argument)
{
    bool stack_reported = false;
    (void)argument;

    for (;;) {
        sample_once();
        if (!stack_reported) {
            ESP_LOGI(TAG, "task stack minimum free=%u bytes",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            stack_reported = true;
        }
        vTaskDelay(pdMS_TO_TICKS(POWER_REFRESH_MS));
    }
}

esp_err_t werewolf_power_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_task = xTaskCreateStatic(power_task, "werewolf_power",
                               sizeof(s_task_stack),
                               NULL, POWER_TASK_PRIORITY, s_task_stack,
                               &s_task_buffer);
    if (s_task == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
