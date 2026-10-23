#include "board_io.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board";

board_outputs_t g_board_out;

static const gpio_num_t s_outputs[] = {
    PIN_DAC_XSMT,
    PIN_WDI,
    PIN_AUDIO_READY,
    PIN_INPUT_SEL,
    PIN_STATUS_LED,
};

/*
 * Inputs with external pull-ups or pull-downs. Internal pulls stay disabled:
 * an internal ~45 k pull-up on IO9 would fight R33 (100 k to GND) and leave
 * PCM1863 SCKI at a mid-rail level instead of static low.
 */
static const gpio_num_t s_inputs[] = {
    PIN_OUT_RLY_SENSE,
    PIN_ADC_SCKI,
    PIN_USB_VBUS_SENSE,
    PIN_FAULT_N,
    PIN_MODE_TRIG_N,
    PIN_ADC_INT,
    PIN_ENC1_A, PIN_ENC1_B, PIN_ENC1_SW,
    PIN_ENC2_A, PIN_ENC2_B, PIN_ENC2_SW,
    PIN_ENC3_A, PIN_ENC3_B, PIN_ENC3_SW,
};

void board_io_init_safe(void)
{
    uint64_t out_mask = 0;
    uint64_t in_mask = 0;

    /* Latch the output register low before enabling any output driver, so no
     * pin glitches high on the way from reset (input) to output. */
    for (size_t i = 0; i < sizeof(s_outputs) / sizeof(s_outputs[0]); i++) {
        gpio_ll_set_level(&GPIO, s_outputs[i], 0);
        out_mask |= 1ULL << s_outputs[i];
    }
    g_board_out.xsmt = 0;
    g_board_out.audio_ready = 0;
    g_board_out.input_sel = 0;
    g_board_out.led = 0;

    /* Input+output so the pad level can be read back against the command. */
    const gpio_config_t out_cfg = {
        .pin_bit_mask = out_mask,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));
    for (size_t i = 0; i < sizeof(s_outputs) / sizeof(s_outputs[0]); i++) {
        gpio_ll_set_level(&GPIO, s_outputs[i], 0);
    }

    for (size_t i = 0; i < sizeof(s_inputs) / sizeof(s_inputs[0]); i++) {
        in_mask |= 1ULL << s_inputs[i];
    }
    const gpio_config_t in_cfg = {
        .pin_bit_mask = in_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));

    ESP_LOGI(TAG, "safe GPIO state: XSMT=0 AUDIO_READY=0 INPUT_SEL=0 WDI=0, IO9 high-Z");
}
