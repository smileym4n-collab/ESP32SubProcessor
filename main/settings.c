#include "settings.h"

#include <math.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NVS_NS = "p68";

/* Phase 1 defaults (approved 2026-10-23, TODO Tom to confirm for Phase 2). */
void settings_defaults(settings_t *s)
{
    s->build = BUILD_LOW_LEVEL;
    s->input = INPUT_PREAMP;
    s->pga_half_db = 0;
    s->gain_db = 0.0f;
    s->limit_dbfs = -6.0f;
}

const char *settings_build_name(build_t b)
{
    return b == BUILD_HIGH_LEVEL ? "HIGH-LEVEL" : "LOW-LEVEL";
}

const char *settings_input_name(input_t i)
{
    return i == INPUT_LFE ? "lfe" : "preamp";
}

esp_err_t settings_init(settings_t *out)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition reformatted (%s)", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    settings_defaults(out);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
        return ESP_OK;
    }

    uint8_t u8;
    int8_t i8;
    int16_t i16;
    if (nvs_get_u8(h, "build", &u8) == ESP_OK && u8 <= BUILD_HIGH_LEVEL) {
        out->build = (build_t)u8;
    }
    if (nvs_get_u8(h, "input", &u8) == ESP_OK && u8 <= INPUT_LFE) {
        out->input = (input_t)u8;
    }
    if (nvs_get_i8(h, "pga", &i8) == ESP_OK &&
        i8 >= SETTINGS_PGA_HALF_MIN && i8 <= SETTINGS_PGA_HALF_MAX) {
        out->pga_half_db = i8;
    }
    if (nvs_get_i16(h, "gain_cdb", &i16) == ESP_OK &&
        i16 >= SETTINGS_GAIN_DB_MIN * 100 && i16 <= SETTINGS_GAIN_DB_MAX * 100) {
        out->gain_db = i16 / 100.0f;
    }
    if (nvs_get_i16(h, "limit_cdb", &i16) == ESP_OK &&
        i16 >= SETTINGS_LIMIT_DB_MIN * 100 && i16 <= SETTINGS_LIMIT_DB_MAX * 100) {
        out->limit_dbfs = i16 / 100.0f;
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t settings_save(const settings_t *s)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_u8(h, "build", (uint8_t)s->build);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "input", (uint8_t)s->input);
    }
    if (err == ESP_OK) {
        err = nvs_set_i8(h, "pga", (int8_t)s->pga_half_db);
    }
    if (err == ESP_OK) {
        err = nvs_set_i16(h, "gain_cdb", (int16_t)lroundf(s->gain_db * 100.0f));
    }
    if (err == ESP_OK) {
        err = nvs_set_i16(h, "limit_cdb", (int16_t)lroundf(s->limit_dbfs * 100.0f));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
