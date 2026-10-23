/*
 * Real-time audio path and TPS3850 watchdog kicking.
 *
 * One task, pinned to core 1 at high priority, owns I2S0 (full-duplex master,
 * 48 kHz, 64 fs, 32-bit Philips slots) and is the only code that ever toggles
 * WDI. It processes ADC -> mono sum -> [test generator] -> subsonic HPF ->
 * gain ramp -> limiter -> DAC, with buffers in internal RAM and no logging,
 * locks or allocation in the loop.
 *
 * Every 50 ms of audio (counted in DMA blocks) the task judges the period:
 * blocks on time, no DMA queue overflow, no read/write error, ADC data not
 * stuck, DSP state finite. Only a healthy period produces a WDI toggle, and
 * only after audio_enable_kicks(). Two bad periods in a row exceed the
 * TPS3850 window (165.8 ms min) and the hardware opens the output relay.
 *
 * The control side talks to the task through single-writer parameters with
 * sequence counters; nothing here blocks the audio task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dsp_core.h"
#include "esp_err.h"
#include "sdkconfig.h"

#define AUDIO_SAMPLE_RATE       48000
#define AUDIO_BLOCK_FRAMES      CONFIG_P68_AUDIO_BLOCK_FRAMES
#define AUDIO_KICK_PERIOD_MS    50
#define AUDIO_KICK_BLOCKS       ((AUDIO_SAMPLE_RATE / 1000 * AUDIO_KICK_PERIOD_MS) / AUDIO_BLOCK_FRAMES)

#if CONFIG_P68_HPF_USE_ESP_DSP_BIQUAD
#define AUDIO_HPF_NAME          "ESP-DSP biquad"
#else
#define AUDIO_HPF_NAME          "SVF"
#endif

/* Why the last judged period was unhealthy. */
#define AUDIO_HF_READ_ERROR     (1u << 0)   /* i2s read timeout or short read */
#define AUDIO_HF_RX_OVF         (1u << 1)   /* RX DMA queue overflowed (task too late) */
#define AUDIO_HF_TX_OVF         (1u << 2)   /* TX DMA queue ran dry */
#define AUDIO_HF_GAP            (1u << 3)   /* a block arrived > 5 ms after the previous one */
#define AUDIO_HF_TIMING         (1u << 4)   /* 50 ms of blocks took < 40 ms or > 60 ms */
#define AUDIO_HF_STUCK          (1u << 5)   /* ADC words bit-identical on both channels >= 100 ms */
#define AUDIO_HF_DSP            (1u << 6)   /* non-finite filter state, filter reset */
#define AUDIO_HF_WRITE_ERROR    (1u << 7)   /* i2s write timeout or short write */

typedef enum {
    AUDIO_WDT_NORMAL = 0,   /* kick every 50 ms while healthy */
    AUDIO_WDT_STARVE,       /* stop kicking: TPS3850 must fault within 165.8 ms */
    AUDIO_WDT_FAST,         /* toggle every 2 blocks (~1.3 ms): lower-window fault */
} audio_wdt_test_t;

typedef struct {
    bool running;
    bool healthy;
    bool kicks_enabled;
    audio_wdt_test_t wdt_test;
    uint32_t last_bad_flags;    /* AUDIO_HF_* of the most recent unhealthy period */
    uint32_t healthy_for_ms;    /* continuous healthy time, 0 if unhealthy */

    uint32_t blocks;
    uint32_t periods;
    uint32_t bad_periods;
    uint32_t read_errors;
    uint32_t write_errors;
    uint32_t rx_ovf;
    uint32_t tx_ovf;
    uint32_t dsp_resets;
    uint32_t nonfinite;

    uint32_t gap_max_us;        /* largest block-to-block gap, last period */
    uint32_t gap_max_us_ever;
    uint32_t period_us;         /* duration of the last 50 ms period */
    uint32_t cycles_max;        /* DSP cycles per block, max over last period */
    uint32_t cycles_avg;        /* DSP cycles per block, mean over last period */

    uint32_t kicks;
    uint32_t last_kick_us;
    uint32_t kick_interval_min_us;  /* since boot, normal mode only */
    uint32_t kick_interval_max_us;

    float in_peak_l;            /* linear, last period */
    float in_peak_r;
    float out_peak;
    float limiter_min_gain;
    float gain_now;             /* current ramp gain */
    float gain_target;
} audio_stats_t;

/* Time of the latest WDI toggle (esp_timer, low 32 bits of us). ISR-readable. */
extern volatile uint32_t g_audio_last_kick_us;

/*
 * Start I2S and the audio task with the output gain at 0 (silent).
 * Returns once I2S is running, or with the I2S setup error.
 */
esp_err_t audio_start(float hpf_hz, float ceiling_lin);

void audio_get_stats(audio_stats_t *st);
bool audio_is_healthy(void);
uint32_t audio_healthy_for_ms(void);

/* One-way: once enabled, kicking is gated only by audio health. */
void audio_enable_kicks(void);
void audio_set_wdt_test(audio_wdt_test_t mode);
audio_wdt_test_t audio_get_wdt_test(void);

/* Ramp the output gain to lin over ramp_ms (full-scale 0 -> 1 time). */
void audio_set_gain(float lin, float ramp_ms);
/* Output gain to 0 at the next block, no ramp. */
void audio_mute_now(void);
bool audio_ramp_done(void);
float audio_gain_now(void);

void audio_set_ceiling(float lin);
void audio_set_generator(dsp_gen_mode_t mode, float freq_hz, float amp);
