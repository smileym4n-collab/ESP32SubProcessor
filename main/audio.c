#include "audio.h"

#include <math.h>
#include <string.h>

#include "board_io.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_TASK_CORE         1
#define AUDIO_TASK_PRIORITY     20
#define AUDIO_TASK_STACK        4096

#define BLOCK_SAMPLES           (AUDIO_BLOCK_FRAMES * 2)
#define BLOCK_BYTES             (BLOCK_SAMPLES * sizeof(int32_t))
#define IO_TIMEOUT_MS           20
#define GAP_LIMIT_US            5000
#define PERIOD_MIN_US           40000
#define PERIOD_MAX_US           60000
#define MIN_KICK_INTERVAL_US    20000   /* TPS3850 lower window is 10.35 ms max */
#define FAST_TEST_BLOCKS        2
#define STUCK_LIMIT_FRAMES      (AUDIO_SAMPLE_RATE / 10)
#define HPF_Q                   0.70710678f
#define LIMITER_RELEASE_MS      100.0f

#if (AUDIO_SAMPLE_RATE / 1000 * AUDIO_KICK_PERIOD_MS) % AUDIO_BLOCK_FRAMES != 0
#error "AUDIO_BLOCK_FRAMES must divide 50 ms of samples"
#endif

static const char *TAG = "audio";

/* Audio buffers: static, so in internal DRAM (no PSRAM on this module). */
static int32_t s_rx[BLOCK_SAMPLES];
static int32_t s_tx[BLOCK_SAMPLES];
static float s_buf[AUDIO_BLOCK_FRAMES];
#if CONFIG_P68_HPF_USE_ESP_DSP_BIQUAD
static float s_buf2[AUDIO_BLOCK_FRAMES];
#endif

static i2s_chan_handle_t s_tx_chan;
static i2s_chan_handle_t s_rx_chan;
static TaskHandle_t s_task;
static TaskHandle_t s_starter;
static esp_err_t s_start_err;

/* DSP state, owned by the audio task. */
#if CONFIG_P68_HPF_USE_ESP_DSP_BIQUAD
static dsp_biquad_t s_hpf;
#else
static dsp_svf_t s_hpf;
#endif
static dsp_ramp_t s_ramp;
static dsp_limiter_t s_lim;
static dsp_gen_t s_gen;
static float s_hpf_hz;

/* Counters written from the I2S ISR. */
static volatile uint32_t s_rx_ovf;
static volatile uint32_t s_tx_ovf;

volatile uint32_t g_audio_last_kick_us;

/* Control -> audio parameters. Single writer each, sequence-counted. */
static volatile uint32_t s_gain_seq;
static volatile float s_gain_target;
static volatile float s_gain_ramp_ms;
static volatile bool s_gain_jump;
static volatile float s_ceiling;
static volatile uint32_t s_gen_seq;
static volatile int s_gen_mode;
static volatile float s_gen_freq;
static volatile float s_gen_amp;
static volatile bool s_kicks_enabled;
static volatile int s_wdt_test;

/* Audio -> control state. */
static volatile bool s_healthy;
static volatile uint32_t s_healthy_since_us;
static volatile bool s_ramp_done_flag = true;
static volatile uint32_t s_gain_seq_applied;
static volatile float s_gain_now;

static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static audio_stats_t s_stats;

static inline uint32_t now_us(void)
{
    return (uint32_t)esp_timer_get_time();
}

static bool IRAM_ATTR on_recv_q_ovf(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    (void)handle;
    (void)event;
    (void)ctx;
    s_rx_ovf++;
    return false;
}

static bool IRAM_ATTR on_send_q_ovf(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    (void)handle;
    (void)event;
    (void)ctx;
    s_tx_ovf++;
    return false;
}

static esp_err_t i2s_setup(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = CONFIG_P68_AUDIO_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_BLOCK_FRAMES;
    chan_cfg.auto_clear = true; /* TX sends zeros if the task ever falls behind */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan), TAG, "new channel");

    /* 32-bit data in 32-bit slots: BCK = 64 fs = 3.072 MHz. No MCLK output:
     * IO9 (PCM1863 SCKI) must stay undriven so the ADC uses its BCK-PLL. */
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCK,
            .ws = PIN_I2S_LRCK,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg), TAG, "init tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg), TAG, "init rx");

    const i2s_event_callbacks_t rx_cbs = {.on_recv_q_ovf = on_recv_q_ovf};
    const i2s_event_callbacks_t tx_cbs = {.on_send_q_ovf = on_send_q_ovf};
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_rx_chan, &rx_cbs, NULL), TAG, "rx cb");
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_tx_chan, &tx_cbs, NULL), TAG, "tx cb");

    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "enable tx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "enable rx");
    return ESP_OK;
}

static void hpf_init(void)
{
#if CONFIG_P68_HPF_USE_ESP_DSP_BIQUAD
    dsp_biquad_highpass(&s_hpf, s_hpf_hz, HPF_Q, AUDIO_SAMPLE_RATE);
#else
    dsp_svf_highpass(&s_hpf, s_hpf_hz, HPF_Q, AUDIO_SAMPLE_RATE);
#endif
}

/* Returns false if the filter state went non-finite (it is then reset). */
static bool hpf_process(float *buf)
{
#if CONFIG_P68_HPF_USE_ESP_DSP_BIQUAD
    dsp_biquad_process(&s_hpf, buf, s_buf2, AUDIO_BLOCK_FRAMES);
    memcpy(buf, s_buf2, sizeof(s_buf2));
    if (!dsp_biquad_state_ok(&s_hpf)) {
        dsp_biquad_reset(&s_hpf);
        return false;
    }
#else
    dsp_svf_highpass_process(&s_hpf, buf, buf, AUDIO_BLOCK_FRAMES);
    if (!dsp_svf_state_ok(&s_hpf)) {
        dsp_svf_reset(&s_hpf);
        return false;
    }
#endif
    return true;
}

static void apply_params(uint32_t *gain_seq_seen, uint32_t *gen_seq_seen)
{
    uint32_t s1 = s_gain_seq;
    if (s1 != *gain_seq_seen) {
        __sync_synchronize();
        const float target = s_gain_target;
        const float ramp_ms = s_gain_ramp_ms;
        const bool jump = s_gain_jump;
        __sync_synchronize();
        if (s_gain_seq == s1) {
            if (jump) {
                dsp_ramp_jump(&s_ramp, target);
            } else {
                dsp_ramp_set(&s_ramp, target, ramp_ms, AUDIO_SAMPLE_RATE);
            }
            *gain_seq_seen = s1;
        }
    }

    const float ceiling = s_ceiling;
    if (ceiling != s_lim.ceiling) {
        dsp_limiter_set_ceiling(&s_lim, ceiling);
    }

    s1 = s_gen_seq;
    if (s1 != *gen_seq_seen) {
        __sync_synchronize();
        const int mode = s_gen_mode;
        const float freq = s_gen_freq;
        const float amp = s_gen_amp;
        __sync_synchronize();
        if (s_gen_seq == s1) {
            if (mode == DSP_GEN_SINE) {
                dsp_gen_sine(&s_gen, freq, amp, AUDIO_SAMPLE_RATE);
            } else if (mode == DSP_GEN_PULSE) {
                dsp_gen_pulse(&s_gen, amp, 2.0f, 500.0f, AUDIO_SAMPLE_RATE);
            } else {
                dsp_gen_off(&s_gen);
            }
            *gen_seq_seen = s1;
        }
    }
}

static inline float peak_i32(const int32_t *rx, int ch)
{
    int32_t pmax = 0;
    for (int i = ch; i < BLOCK_SAMPLES; i += 2) {
        int32_t v = rx[i];
        v = (v == INT32_MIN) ? INT32_MAX : (v < 0 ? -v : v);
        if (v > pmax) {
            pmax = v;
        }
    }
    return (float)pmax / 2147483648.0f;
}

static inline void toggle_wdi(bool *level, uint32_t t)
{
    *level = !*level;
    board_set_wdi(*level);
    g_audio_last_kick_us = t;
}

static void audio_task(void *arg)
{
    (void)arg;

    s_start_err = i2s_setup();
    xTaskNotifyGive(s_starter);
    if (s_start_err != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }

    uint32_t gain_seq_seen = 0, gen_seq_seen = 0;
    bool wdi_level = false;
    uint32_t t_last_block = now_us();
    uint32_t t_period_start = t_last_block;

    /* Per-period accumulators. */
    uint32_t p_blocks = 0, p_flags = 0, p_gap_max = 0, p_cycles_max = 0;
    uint64_t p_cycles_sum = 0;
    float p_in_l = 0, p_in_r = 0, p_out = 0;
    uint32_t p_rx_ovf0 = s_rx_ovf, p_tx_ovf0 = s_tx_ovf;
    uint32_t fast_count = 0;
    int32_t prev_l = 0, prev_r = 0;
    uint32_t stuck_frames = 0;

    /* Totals published to audio_get_stats(). */
    uint32_t t_blocks = 0, t_periods = 0, t_bad = 0, t_read_err = 0, t_write_err = 0;
    uint32_t t_dsp_resets = 0, t_kicks = 0, t_gap_ever = 0, last_bad_flags = 0;
    uint32_t kick_min = UINT32_MAX, kick_max = 0;
    uint32_t t_last_kick = t_last_block;
    bool kicked_once = false;

    for (;;) {
        size_t got = 0;
        const esp_err_t rerr = i2s_channel_read(s_rx_chan, s_rx, BLOCK_BYTES, &got, IO_TIMEOUT_MS);
        const uint32_t t = now_us();

        if (rerr != ESP_OK || got != BLOCK_BYTES) {
            /* Clock stalled or DMA broken: this period is bad, start a new one.
             * No write: TX auto-clear keeps sending zeros. */
            t_read_err++;
            s_healthy = false;
            last_bad_flags = AUDIO_HF_READ_ERROR;
            t_bad++;
            p_blocks = 0;
            p_flags = 0;
            p_gap_max = 0;
            p_rx_ovf0 = s_rx_ovf;
            p_tx_ovf0 = s_tx_ovf;
            t_period_start = t;
            t_last_block = t;
            continue;
        }

        const uint32_t c0 = esp_cpu_get_cycle_count();
        const uint32_t gap = t - t_last_block;
        t_last_block = t;
        if (gap > p_gap_max) {
            p_gap_max = gap;
        }

        apply_params(&gain_seq_seen, &gen_seq_seen);

        /* ADC sanity: words bit-identical on both channels for >= 100 ms. */
        bool changed = false;
        for (int i = 0; i < BLOCK_SAMPLES; i += 2) {
            if (s_rx[i] != prev_l || s_rx[i + 1] != prev_r) {
                changed = true;
                break;
            }
        }
        prev_l = s_rx[BLOCK_SAMPLES - 2];
        prev_r = s_rx[BLOCK_SAMPLES - 1];
        stuck_frames = changed ? 0 : stuck_frames + AUDIO_BLOCK_FRAMES;

        const float in_l = peak_i32(s_rx, 0);
        const float in_r = peak_i32(s_rx, 1);
        p_in_l = in_l > p_in_l ? in_l : p_in_l;
        p_in_r = in_r > p_in_r ? in_r : p_in_r;

        dsp_i2s_to_mono(s_rx, s_buf, AUDIO_BLOCK_FRAMES);
        if (s_gen.mode != DSP_GEN_OFF) {
            dsp_gen_process(&s_gen, s_buf, AUDIO_BLOCK_FRAMES);
        }
        if (!hpf_process(s_buf)) {
            memset(s_buf, 0, sizeof(s_buf));
            p_flags |= AUDIO_HF_DSP;
            t_dsp_resets++;
        }
        dsp_ramp_process(&s_ramp, s_buf, AUDIO_BLOCK_FRAMES);
        dsp_limiter_process(&s_lim, s_buf, AUDIO_BLOCK_FRAMES);
        const float out = dsp_peak_abs(s_buf, AUDIO_BLOCK_FRAMES);
        p_out = out > p_out ? out : p_out;
        dsp_mono_to_i2s(s_buf, s_tx, AUDIO_BLOCK_FRAMES);

        s_gain_now = s_ramp.current;
        s_ramp_done_flag = dsp_ramp_done(&s_ramp);
        __sync_synchronize();
        s_gain_seq_applied = gain_seq_seen;

        const uint32_t cycles = esp_cpu_get_cycle_count() - c0;
        p_cycles_sum += cycles;
        if (cycles > p_cycles_max) {
            p_cycles_max = cycles;
        }

        size_t written = 0;
        const esp_err_t werr = i2s_channel_write(s_tx_chan, s_tx, BLOCK_BYTES, &written, IO_TIMEOUT_MS);
        if (werr != ESP_OK || written != BLOCK_BYTES) {
            p_flags |= AUDIO_HF_WRITE_ERROR;
            t_write_err++;
        }

        t_blocks++;
        p_blocks++;

        const int wdt_test = s_wdt_test;
        if (wdt_test == AUDIO_WDT_FAST && ++fast_count >= FAST_TEST_BLOCKS) {
            fast_count = 0;
            toggle_wdi(&wdi_level, t);
        }

        if (p_blocks < AUDIO_KICK_BLOCKS) {
            continue;
        }

        /* ---- 50 ms of audio: judge the period. ---- */
        const uint32_t period_us = t - t_period_start;
        const uint32_t rx_ovf = s_rx_ovf, tx_ovf = s_tx_ovf;
        uint32_t flags = p_flags;
        if (rx_ovf != p_rx_ovf0) {
            flags |= AUDIO_HF_RX_OVF;
        }
        if (tx_ovf != p_tx_ovf0) {
            flags |= AUDIO_HF_TX_OVF;
        }
        if (p_gap_max > GAP_LIMIT_US) {
            flags |= AUDIO_HF_GAP;
        }
        if (period_us < PERIOD_MIN_US || period_us > PERIOD_MAX_US) {
            flags |= AUDIO_HF_TIMING;
        }
        if (stuck_frames >= STUCK_LIMIT_FRAMES) {
            flags |= AUDIO_HF_STUCK;
        }

        const bool ok = (flags == 0);
        t_periods++;
        if (ok) {
            if (!s_healthy) {
                s_healthy_since_us = t;
            }
            s_healthy = true;
        } else {
            s_healthy = false;
            last_bad_flags = flags;
            t_bad++;
        }

        if (ok && s_kicks_enabled && wdt_test == AUDIO_WDT_NORMAL &&
            (t - t_last_kick) >= MIN_KICK_INTERVAL_US) {
            if (kicked_once) {
                const uint32_t interval = t - t_last_kick;
                kick_min = interval < kick_min ? interval : kick_min;
                kick_max = interval > kick_max ? interval : kick_max;
            }
            toggle_wdi(&wdi_level, t);
            t_last_kick = t;
            kicked_once = true;
            t_kicks++;
        }
        if (p_gap_max > t_gap_ever) {
            t_gap_ever = p_gap_max;
        }

        portENTER_CRITICAL(&s_stats_mux);
        s_stats.running = true;
        s_stats.healthy = ok;
        s_stats.last_bad_flags = last_bad_flags;
        s_stats.blocks = t_blocks;
        s_stats.periods = t_periods;
        s_stats.bad_periods = t_bad;
        s_stats.read_errors = t_read_err;
        s_stats.write_errors = t_write_err;
        s_stats.rx_ovf = rx_ovf;
        s_stats.tx_ovf = tx_ovf;
        s_stats.dsp_resets = t_dsp_resets;
        s_stats.nonfinite = s_lim.nonfinite;
        s_stats.gap_max_us = p_gap_max;
        s_stats.gap_max_us_ever = t_gap_ever;
        s_stats.period_us = period_us;
        s_stats.cycles_max = p_cycles_max;
        s_stats.cycles_avg = (uint32_t)(p_cycles_sum / p_blocks);
        s_stats.kicks = t_kicks;
        s_stats.last_kick_us = g_audio_last_kick_us;
        s_stats.kick_interval_min_us = kick_min == UINT32_MAX ? 0 : kick_min;
        s_stats.kick_interval_max_us = kick_max;
        s_stats.in_peak_l = p_in_l;
        s_stats.in_peak_r = p_in_r;
        s_stats.out_peak = p_out;
        s_stats.limiter_min_gain = dsp_limiter_take_min_gain(&s_lim);
        s_stats.gain_now = s_ramp.current;
        s_stats.gain_target = s_ramp.target;
        portEXIT_CRITICAL(&s_stats_mux);

        p_blocks = 0;
        p_flags = 0;
        p_gap_max = 0;
        p_cycles_max = 0;
        p_cycles_sum = 0;
        p_in_l = p_in_r = p_out = 0;
        p_rx_ovf0 = rx_ovf;
        p_tx_ovf0 = tx_ovf;
        t_period_start = t;
    }
}

esp_err_t audio_start(float hpf_hz, float ceiling_lin)
{
    if (s_task) {
        return ESP_OK;
    }
    s_hpf_hz = hpf_hz;
    hpf_init();
    dsp_ramp_init(&s_ramp, 0.0f);
    dsp_limiter_init(&s_lim, ceiling_lin, LIMITER_RELEASE_MS, AUDIO_SAMPLE_RATE);
    dsp_gen_off(&s_gen);
    s_ceiling = ceiling_lin;
    s_gain_target = 0.0f;

    s_starter = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(audio_task, "audio", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIORITY,
                                &s_task, AUDIO_TASK_CORE) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (s_start_err != ESP_OK) {
        s_task = NULL;
        ESP_LOGE(TAG, "I2S start failed: %s", esp_err_to_name(s_start_err));
        return s_start_err;
    }
    ESP_LOGI(TAG, "I2S0 full duplex 48 kHz/64 fs, block %d frames x %d DMA buffers, HPF %.1f Hz (%s)",
             AUDIO_BLOCK_FRAMES, CONFIG_P68_AUDIO_DMA_DESC_NUM, (double)hpf_hz,
             AUDIO_HPF_NAME);
    return ESP_OK;
}

void audio_get_stats(audio_stats_t *st)
{
    portENTER_CRITICAL(&s_stats_mux);
    *st = s_stats;
    portEXIT_CRITICAL(&s_stats_mux);
    st->running = (s_task != NULL);
    st->healthy = s_healthy;
    st->healthy_for_ms = audio_healthy_for_ms();
    st->kicks_enabled = s_kicks_enabled;
    st->wdt_test = (audio_wdt_test_t)s_wdt_test;
}

bool audio_is_healthy(void)
{
    return s_task != NULL && s_healthy;
}

uint32_t audio_healthy_for_ms(void)
{
    if (!audio_is_healthy()) {
        return 0;
    }
    return (now_us() - s_healthy_since_us) / 1000;
}

void audio_enable_kicks(void)
{
    s_kicks_enabled = true;
}

void audio_set_wdt_test(audio_wdt_test_t mode)
{
    s_wdt_test = mode;
}

audio_wdt_test_t audio_get_wdt_test(void)
{
    return (audio_wdt_test_t)s_wdt_test;
}

static void post_gain(float lin, float ramp_ms, bool jump)
{
    s_gain_target = lin;
    s_gain_ramp_ms = ramp_ms;
    s_gain_jump = jump;
    __sync_synchronize();
    s_gain_seq = s_gain_seq + 1;
}

void audio_set_gain(float lin, float ramp_ms)
{
    post_gain(lin, ramp_ms, false);
}

void audio_mute_now(void)
{
    post_gain(0.0f, 0.0f, true);
}

bool audio_ramp_done(void)
{
    /* Only trust the flag once the audio task has applied the latest request. */
    return s_gain_seq_applied == s_gain_seq && s_ramp_done_flag;
}

float audio_gain_now(void)
{
    return s_gain_now;
}

void audio_set_ceiling(float lin)
{
    s_ceiling = lin;
}

void audio_set_generator(dsp_gen_mode_t mode, float freq_hz, float amp)
{
    s_gen_mode = mode;
    s_gen_freq = freq_hz;
    s_gen_amp = amp;
    __sync_synchronize();
    s_gen_seq = s_gen_seq + 1;
}
