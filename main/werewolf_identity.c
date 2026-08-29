#include "werewolf_identity.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "werewolf_identity_record.h"

#define IDENTITY_NAMESPACE "ww_identity"
#define IDENTITY_KEY       "profile"

static const char *TAG = "werewolf_identity";

static void fallback_name(werewolf_nickname_t nickname, const uint8_t mac[6])
{
    unsigned high = mac != NULL ? mac[4] : 0U;
    unsigned low = mac != NULL ? mac[5] : 0U;
    char fallback[WEREWOLF_NICKNAME_CAPACITY];

    (void)snprintf(fallback, sizeof(fallback), "MOTE-%02X%02X", high, low);
    if (!werewolf_nickname_normalize(nickname, fallback)) {
        (void)werewolf_nickname_normalize(nickname, "MOTE");
    }
}

static bool write_record(const char *nickname, uint32_t revision)
{
    uint8_t record[WEREWOLF_IDENTITY_RECORD_SIZE];
    nvs_handle_t handle;
    esp_err_t error;

    if (!werewolf_identity_record_encode(record, nickname, revision)) {
        return false;
    }
    error = nvs_open(IDENTITY_NAMESPACE, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, IDENTITY_KEY, record, sizeof(record));
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    memset(record, 0, sizeof(record));
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "nickname NVS write skipped: %s",
                 esp_err_to_name(error));
        return false;
    }
    return true;
}

bool werewolf_identity_load(werewolf_nickname_t nickname,
                            const uint8_t mac[6])
{
    werewolf_nickname_t stored = { 0 };
    werewolf_nickname_t seed = { 0 };
    uint8_t record[WEREWOLF_IDENTITY_RECORD_SIZE];
    uint32_t stored_revision = 0U;
    uint32_t seed_revision =
        (uint32_t)CONFIG_WEREWOLF_FACTORY_PROFILE_REVISION;
    size_t record_size = sizeof(record);
    nvs_handle_t handle;
    esp_err_t error;
    bool have_stored = false;
    bool have_seed = CONFIG_WEREWOLF_FACTORY_NICKNAME[0] != '\0' &&
                     werewolf_nickname_normalize(
                         seed, CONFIG_WEREWOLF_FACTORY_NICKNAME);

    if (nickname == NULL) {
        return false;
    }
    fallback_name(nickname, mac);

    error = nvs_flash_init();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable; using %s without erasing: %s",
                 nickname, esp_err_to_name(error));
        return false;
    }
    error = nvs_open(IDENTITY_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_OK) {
        error = nvs_get_blob(handle, IDENTITY_KEY, record, &record_size);
        nvs_close(handle);
        have_stored = error == ESP_OK &&
                      werewolf_identity_record_decode(
                          record, record_size, stored, &stored_revision);
        memset(record, 0, sizeof(record));
    }

    if (have_stored && (!have_seed || seed_revision <= stored_revision)) {
        memcpy(nickname, stored, sizeof(stored));
        return true;
    }
    if (have_seed) {
        memcpy(nickname, seed, sizeof(seed));
        if (write_record(seed, seed_revision)) {
            ESP_LOGI(TAG, "nickname provisioned: %s rev=%lu", nickname,
                     (unsigned long)seed_revision);
            return true;
        }
        return false;
    }
    if (error != ESP_ERR_NVS_NOT_FOUND && error != ESP_OK) {
        ESP_LOGW(TAG, "nickname NVS record unavailable: %s",
                 esp_err_to_name(error));
    }
    return false;
}
