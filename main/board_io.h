/*
 * Board-level GPIO: safe initialisation and fast, ISR-safe accessors.
 *
 * The output setters write the GPIO registers directly (static inline, no
 * locks), so they are safe from the FAULT_N interrupt, from IRAM, and from the
 * audio task. Every output is configured input+output so the pad level can be
 * read back and compared with the commanded level (catches a pin shorted to a
 * rail, e.g. DESIGN_REVIEW §8.3 "short TP21 to 3V3").
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "esp_attr.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

/* Commanded output levels, written only through the setters below. */
typedef struct {
    volatile uint8_t xsmt;
    volatile uint8_t audio_ready;
    volatile uint8_t input_sel;
    volatile uint8_t led;
} board_outputs_t;

extern board_outputs_t g_board_out;

/*
 * Drive every output to its safe (low) level and configure every input with
 * internal pulls disabled. Must be the first thing app_main does.
 */
void board_io_init_safe(void);

static inline void board_set_xsmt(bool unmute)
{
    g_board_out.xsmt = unmute;
    gpio_ll_set_level(&GPIO, PIN_DAC_XSMT, unmute);
}

static inline void board_set_audio_ready(bool high)
{
    g_board_out.audio_ready = high;
    gpio_ll_set_level(&GPIO, PIN_AUDIO_READY, high);
}

static inline void board_set_input_sel(bool lfe)
{
    g_board_out.input_sel = lfe;
    gpio_ll_set_level(&GPIO, PIN_INPUT_SEL, lfe);
}

static inline void board_set_led(bool on)
{
    g_board_out.led = on;
    gpio_ll_set_level(&GPIO, PIN_STATUS_LED, on);
}

/* Audio task only: TPS3850 WDI. */
static inline void board_set_wdi(bool high)
{
    gpio_ll_set_level(&GPIO, PIN_WDI, high);
}

/* Muted and disarmed in two register writes. Safe from IRAM ISRs. */
static inline void board_mute_disarm(void)
{
    board_set_xsmt(false);
    board_set_audio_ready(false);
}

static inline bool board_pad(gpio_num_t pin)
{
    return gpio_ll_get_level(&GPIO, pin) != 0;
}

/* FAULT_N high: no supervisor or watchdog fault asserted. */
static inline bool board_fault_n_ok(void)
{
    return board_pad(PIN_FAULT_N);
}

/* IO3 low: K1 pole B says the output relay is closed. */
static inline bool board_relay_closed(void)
{
    return !board_pad(PIN_OUT_RLY_SENSE);
}

static inline bool board_vbus_present(void)
{
    return board_pad(PIN_USB_VBUS_SENSE);
}

static inline bool board_trigger_present(void)
{
    return !board_pad(PIN_MODE_TRIG_N);
}
