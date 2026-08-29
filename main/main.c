// AI-PASSPORT Werewolf product entry point.
//
// The firmware boots straight into the seven-device game. Button callbacks
// stay intentionally thin: werewolf_app_handle_button() owns the LVGL lock,
// translates the edge and only enqueues a value action for its controller
// task. Game and network work never runs in the button component task.
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "werewolf_app.h"
#include "werewolf_power.h"
#include "werewolf_sound.h"
#include "werewolf_ui.h"

static const char *TAG = "werewolf_main";
static StaticSemaphore_t s_first_frame_signal_buffer;
static SemaphoreHandle_t s_first_frame_signal;

#define FIRST_FRAME_TIMEOUT_MS 2000U

static void first_refresh_ready(lv_event_t *event)
{
    SemaphoreHandle_t signal = lv_event_get_user_data(event);
    if (signal) {
        (void)xSemaphoreGive(signal);
    }
}

static void on_key(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    (void)user;
    werewolf_app_handle_button(button, event);
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI-PASSPORT Werewolf starting");

    lv_display_t *display = NULL;
    if (bsp_display_init() != ESP_OK ||
        (display = bsp_lvgl_init()) == NULL) {
        ESP_LOGE(TAG,
                 "display/LVGL failed; check SPI pins MOSI=%d SCLK=%d "
                 "CS=%d DC=%d BL=%d",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC,
                 BSP_LCD_BL);
        return;
    }

    s_first_frame_signal =
        xSemaphoreCreateBinaryStatic(&s_first_frame_signal_buffer);
    if (!s_first_frame_signal || !bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "unable to arm first-frame synchronization");
        return;
    }
    /* Keep this outer (recursive) lock from callback registration through UI
     * creation. Otherwise the LVGL task could report a refresh of its default
     * empty screen before the Werewolf screen has been loaded. */
    lv_display_add_event_cb(display, first_refresh_ready,
                            LV_EVENT_REFR_READY, s_first_frame_signal);
    esp_err_t app_error = werewolf_app_start();
    if (app_error == ESP_OK) {
        lv_obj_t *screen = lv_display_get_screen_active(display);
        if (screen) {
            lv_obj_invalidate(screen);
        }
    } else {
        (void)lv_display_remove_event_cb_with_user_data(
            display, first_refresh_ready, s_first_frame_signal);
    }
    bsp_lvgl_unlock();
    if (app_error != ESP_OK) {
        ESP_LOGE(TAG, "Werewolf application failed to start");
        return;
    }

    if (xSemaphoreTake(s_first_frame_signal,
                       pdMS_TO_TICKS(FIRST_FRAME_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "product UI first frame timed out; backlight stays off");
        return;
    }

    lv_mem_monitor_t lv_mem = { 0 };
    esp_err_t panel_idle = ESP_ERR_TIMEOUT;
    if (bsp_lvgl_lock(1000)) {
        panel_idle = bsp_display_wait_idle();
        (void)lv_display_remove_event_cb_with_user_data(
            display, first_refresh_ready, s_first_frame_signal);
        lv_mem_monitor(&lv_mem);
        bsp_lvgl_unlock();
    }
    if (panel_idle != ESP_OK) {
        ESP_LOGE(TAG, "first-frame DMA did not finish: %s; backlight stays off",
                 esp_err_to_name(panel_idle));
        return;
    }
    ESP_LOGI(TAG, "initial UI complete; main stack minimum free: %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG,
             "LVGL memory after first frame: peak=%u, free=%u/%u, "
             "largest=%u, fragmentation=%u%%",
             (unsigned)lv_mem.max_used, (unsigned)lv_mem.free_size,
             (unsigned)lv_mem.total_size,
             (unsigned)lv_mem.free_biggest_size,
             (unsigned)lv_mem.frag_pct);
    /* The product screen has rendered and the final SPI DMA transaction has
     * completed, so enabling the backlight cannot expose stale panel GRAM. */
    bsp_display_backlight(100);
    bool sound_ready = werewolf_sound_start();
    werewolf_app_report_audio(true, sound_ready);
    if (!sound_ready) {
        ESP_LOGW(TAG, "audio unavailable; continuing without sound cues");
    }
    esp_err_t power_error = werewolf_power_start();
    if (power_error != ESP_OK) {
        ESP_LOGW(TAG, "battery monitor unavailable: %s",
                 esp_err_to_name(power_error));
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "button input failed; game cannot continue");
        werewolf_app_report_input_failure();
        return;
    }

    ESP_LOGI(TAG,
             "ready: 7 players, ESP-NOW channel 6, audio=%s, "
             "main stack minimum free=%u bytes",
             sound_ready ? "ok" : "fault",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
