/*
 * P68 Subwoofer DSP Controller: Phase 1 bring-up firmware.
 *
 * PCM1863 ADC -> ESP32-S3 (I2S0 full-duplex master) -> PCM5102A DAC ->
 * output relay K1 -> ESP Project 68 power amplifier.
 *
 * Hardware contract: docs/hardware/FIRMWARE_BRIEF.md (§5 arming and fault latch
 * must be followed exactly). Bench procedure: docs/BRINGUP.md.
 *
 * Wi-Fi and Bluetooth are never initialised.
 */
#include "board_io.h"
#include "console.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_system.h"
#include "pcm1863.h"
#include "protection.h"
#include "settings.h"

static const char *TAG = "main";

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on / EN (TPS3850 RESET or EN button)";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_BROWNOUT: return "brown-out";
    case ESP_RST_USB: return "USB";
    default: return "other";
    }
}

static i2c_master_bus_handle_t ui_bus_init(void)
{
    /* Front-panel bus (J6). Phase 1 only scans it; 4k7 pull-ups on the board. */
    const i2c_master_bus_config_t cfg = {
        .i2c_port = 1,
        .sda_io_num = PIN_I2C1_SDA,
        .scl_io_num = PIN_I2C1_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    i2c_master_bus_handle_t h = NULL;
    if (i2c_new_master_bus(&cfg, &h) != ESP_OK) {
        ESP_LOGW(TAG, "I2C1 (front panel) not started");
        return NULL;
    }
    return h;
}

void app_main(void)
{
    /* First: XSMT, AUDIO_READY, INPUT_SEL, WDI low; IO9 high-Z (brief §5 step 1). */
    board_io_init_safe();

    ESP_LOGI(TAG, "P68 Sub DSP Phase 1, reset reason: %s", reset_reason_name(esp_reset_reason()));

    settings_t settings;
    if (settings_init(&settings) != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable, running on defaults");
        settings_defaults(&settings);
    }

    ESP_ERROR_CHECK(pcm1863_bus_init());
    const i2c_master_bus_handle_t ui_bus = ui_bus_init();

    ESP_ERROR_CHECK(protection_start(&settings));
    ESP_ERROR_CHECK(console_start(ui_bus));
}
