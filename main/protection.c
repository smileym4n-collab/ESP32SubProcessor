#include "protection.h"

#include <inttypes.h>
#include <string.h>

#include "audio.h"
#include "board_io.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define CONTROL_TASK_CORE       0
#define CONTROL_TASK_PRIORITY   10
#define CONTROL_TASK_STACK      4096

#define RELAY_CLOSE_TIMEOUT_US  10000   /* brief §5 step 6 */
#define RELAY_POLL_US           100
#define UNMUTE_DELAY_MS         20      /* brief §5 step 7 */
#define RELAY_RELEASE_MS        50      /* AUDIO_READY low -> IO3 high (not yet measured) */
#define RELAY_OPEN_DEBOUNCE_MS  3       /* IO3 high this long while armed = relay opened */
#define RAMP_DOWN_TIMEOUT_MS    500
#define VERIFY_HEALTHY_MS       500
#define VERIFY_TIMEOUT_MS       5000
#define ADC_POLL_VERIFY_MS      100
#define ADC_POLL_MS             500
#define ADC_I2C_FAIL_LIMIT      3
#define REARM_WINDOW_MS         60000
#define FAULT_LOG_LEN           16
#define FAULT_FOLD_MS           10000   /* same cause within this window folds into one entry */

static const char *TAG = "protect";

typedef enum {
    REQ_ARM,
    REQ_DISARM,
    REQ_INPUT,
    REQ_BUILD,
    REQ_PGA,
    REQ_GAIN,
    REQ_LIMIT,
    REQ_SAVE,
    REQ_ADC_RECONFIG,
} req_type_t;

typedef struct {
    req_type_t type;
    int i;
    float f;
} request_t;

static TaskHandle_t s_task;
static QueueHandle_t s_queue;

/* FAULT_N interrupt -> control task. */
static volatile bool s_isr_flag;
static volatile uint32_t s_isr_fall_us;
static volatile uint32_t s_isr_kick_us;

/* Control-task state (only the control task writes these). */
static prot_state_t s_state;
static int64_t s_state_enter_us;
static settings_t s_settings;
static bool s_dirty;
static bool s_hold;
static int64_t s_settle_until_us;
static int64_t s_next_adc_try_us;
static uint32_t s_adc_backoff_ms;
static bool s_adc_configured;
static bool s_audio_started;
static bool s_kicks_started;
static bool s_verify_timeout_logged;
static int64_t s_next_adc_poll_us;
static int s_adc_i2c_fails;
static bool s_adc_status_valid;
static bool s_adc_ok;
static pcm1863_status_t s_adc_status;

static bool s_fn_low;
static int64_t s_fn_fall_us;
static int64_t s_fn_high_since_us;
static int s_fn_pending_entry = -1;  /* fault log slot awaiting a FAULT_N duration */
static uint32_t s_boot_fn_pulses;

static int64_t s_relay_open_since_us;  /* armed, IO3 high since */
static int64_t s_relay_stuck_since_us; /* disarmed, IO3 low since */
static bool s_relay_stuck_logged;
static bool s_pin_mismatch_logged;

static uint32_t s_arm_count;
static uint32_t s_last_close_us;
static uint32_t s_auto_rearms;
static int64_t s_rearm_times_ms[CONFIG_P68_AUTO_REARM_MAX];
static int s_rearm_head;
static const char *s_waiting_for = "";

static struct {
    bool input;
    input_t input_v;
    bool build;
    build_t build_v;
    bool pga;
    int pga_v;
    bool reconfig;
    bool disarm;
} s_pending;

/* Fault log, shared with the console. */
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static fault_entry_t s_log[FAULT_LOG_LEN];
static int s_log_next;
static uint32_t s_log_seq;
static uint32_t s_fault_counts[FAULT_COUNT];

/* Status snapshot for the console. */
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static prot_status_t s_status;

static const char *const s_state_names[PROT_STATE_COUNT] = {
    [PROT_BOOT] = "BOOT",
    [PROT_ADC_CONFIG] = "ADC_CONFIG",
    [PROT_AUDIO_VERIFY] = "AUDIO_VERIFY",
    [PROT_WAIT_CLEAR] = "WAIT_CLEAR",
    [PROT_ARMED_WAIT] = "ARMED_WAIT",
    [PROT_RAMP_UP] = "RAMP_UP",
    [PROT_RUNNING] = "RUNNING",
    [PROT_RAMP_DOWN] = "RAMP_DOWN",
    [PROT_RELEASE] = "RELEASE",
    [PROT_FAULTED] = "FAULTED",
    [PROT_LOCKOUT] = "LOCKOUT",
    [PROT_HOLD] = "HOLD",
};

static const char *const s_fault_names[FAULT_COUNT] = {
    [FAULT_NONE] = "none",
    [FAULT_N_ASSERTED] = "FAULT_N low while armed",
    [FAULT_N_DISARMED] = "FAULT_N low while disarmed",
    [FAULT_RELAY_NO_CLOSE] = "K1 did not close (IO3 not low within 10 ms)",
    [FAULT_RELAY_OPENED] = "K1 opened while armed (IO3 high)",
    [FAULT_RELAY_STUCK] = "K1 sense closed while AUDIO_READY low",
    [FAULT_AUDIO_UNHEALTHY] = "audio unhealthy while armed",
    [FAULT_ADC_CONFIG] = "PCM1863 configuration failed",
    [FAULT_ADC_STATUS] = "PCM1863 status bad while armed",
    [FAULT_ADC_I2C] = "PCM1863 status reads failing",
    [FAULT_AUDIO_START] = "I2S start failed",
    [FAULT_VERIFY_TIMEOUT] = "audio/ADC not healthy 5 s after start",
    [FAULT_PIN_MISMATCH] = "output pad differs from command",
    [FAULT_REARM_LIMIT] = "auto re-arm limit reached",
};

const char *protection_state_name(prot_state_t s)
{
    return (s < PROT_STATE_COUNT) ? s_state_names[s] : "?";
}

const char *protection_fault_name(fault_cause_t c)
{
    return (c < FAULT_COUNT) ? s_fault_names[c] : "?";
}

static inline int64_t now_us(void)
{
    return esp_timer_get_time();
}

static inline uint32_t ms_since(int64_t t_us, int64_t now)
{
    return (uint32_t)((now - t_us) / 1000);
}

static bool state_is_armed(prot_state_t s)
{
    return s == PROT_ARMED_WAIT || s == PROT_RAMP_UP || s == PROT_RUNNING || s == PROT_RAMP_DOWN;
}

bool protection_is_armed(void)
{
    return state_is_armed(s_state);
}

/* ---------------------------------------------------------------- fault log */

/* Returns the slot index used. */
static int log_fault(fault_cause_t cause, uint32_t detail, int32_t since_kick_ms)
{
    const uint32_t t_ms = (uint32_t)(now_us() / 1000);
    int slot;

    portENTER_CRITICAL(&s_log_mux);
    s_fault_counts[cause]++;
    const int last = (s_log_next + FAULT_LOG_LEN - 1) % FAULT_LOG_LEN;
    fault_entry_t *prev = &s_log[last];
    if (s_log_seq > 0 && prev->cause == cause && prev->detail == detail &&
        t_ms - prev->t_ms < FAULT_FOLD_MS && cause != FAULT_N_ASSERTED && cause != FAULT_N_DISARMED) {
        prev->count++;
        slot = last;
    } else {
        slot = s_log_next;
        s_log[slot] = (fault_entry_t){
            .seq = ++s_log_seq,
            .t_ms = t_ms,
            .count = 1,
            .cause = cause,
            .detail = detail,
            .fault_n_low_ms = -1,
            .since_kick_ms = since_kick_ms,
            .state = s_state,
        };
        s_log_next = (s_log_next + 1) % FAULT_LOG_LEN;
    }
    portEXIT_CRITICAL(&s_log_mux);

    ESP_LOGW(TAG, "FAULT: %s (detail 0x%08" PRIx32 ") in %s", protection_fault_name(cause), detail,
             protection_state_name(s_state));
    return slot;
}

int protection_get_faults(fault_entry_t *out, int max)
{
    int n = 0;
    portENTER_CRITICAL(&s_log_mux);
    const int avail = s_log_seq < FAULT_LOG_LEN ? (int)s_log_seq : FAULT_LOG_LEN;
    for (int i = 0; i < avail && n < max; i++) {
        const int slot = (s_log_next + FAULT_LOG_LEN - 1 - i) % FAULT_LOG_LEN;
        out[n++] = s_log[slot];
    }
    portEXIT_CRITICAL(&s_log_mux);
    return n;
}

void protection_get_fault_counts(uint32_t counts[FAULT_COUNT])
{
    portENTER_CRITICAL(&s_log_mux);
    memcpy(counts, s_fault_counts, sizeof(s_fault_counts));
    portEXIT_CRITICAL(&s_log_mux);
}

void protection_clear_faults(void)
{
    portENTER_CRITICAL(&s_log_mux);
    memset(s_log, 0, sizeof(s_log));
    memset(s_fault_counts, 0, sizeof(s_fault_counts));
    s_log_next = 0;
    s_log_seq = 0;
    s_fn_pending_entry = -1;
    portEXIT_CRITICAL(&s_log_mux);
}

/* ------------------------------------------------------------ state changes */

static void enter(prot_state_t st)
{
    if (st != s_state) {
        ESP_LOGI(TAG, "%s -> %s", protection_state_name(s_state), protection_state_name(st));
    }
    s_state = st;
    s_state_enter_us = now_us();
}

/* Immediate mute + disarm, then FAULTED. */
static int fault(fault_cause_t cause, uint32_t detail, int32_t since_kick_ms)
{
    board_mute_disarm();
    audio_mute_now();
    const int slot = log_fault(cause, detail, since_kick_ms);
    enter(PROT_FAULTED);
    return slot;
}

static void IRAM_ATTR fault_n_isr(void *arg)
{
    (void)arg;
    /* Hardware has already cleared the latch and opened K1. Drop the GPIOs
     * at once so firmware state agrees, then let the task do the rest. */
    board_mute_disarm();
    s_isr_fall_us = (uint32_t)esp_timer_get_time();
    s_isr_kick_us = g_audio_last_kick_us;
    s_isr_flag = true;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static uint32_t pack_adc_status(const pcm1863_status_t *st)
{
    return (uint32_t)st->state | ((uint32_t)st->fsample << 8) | ((uint32_t)st->clk << 16) |
           ((uint32_t)st->supply << 24);
}

static bool adc_status_acceptable(void)
{
#if CONFIG_P68_REQUIRE_ADC_STATUS
    return s_adc_status_valid && s_adc_ok;
#else
    return true;
#endif
}

static void update_input_sel(void)
{
    /* INPUT_SEL only exists in the LOW-LEVEL build; keep it low otherwise. */
    board_set_input_sel(s_settings.build == BUILD_LOW_LEVEL && s_settings.input == INPUT_LFE);
}

static pcm1863_config_t adc_config(void)
{
    return (pcm1863_config_t){
        .input = s_settings.build == BUILD_HIGH_LEVEL ? PCM1863_INPUT_VIN3 : PCM1863_INPUT_VIN1,
        .pga_half_db = s_settings.pga_half_db,
    };
}

/* Apply queued mode changes. Only called with the relay commanded open. */
static void apply_pending(int64_t now)
{
    bool changed = false;
    esp_err_t err = ESP_OK;
    uint8_t bad = 0;

    if (s_pending.build) {
        s_settings.build = s_pending.build_v;
        changed = true;
        if (s_adc_configured) {
            err = pcm1863_set_input(adc_config().input, &bad);
        }
    }
    if (s_pending.input) {
        s_settings.input = s_pending.input_v;
        changed = true;
    }
    if (s_pending.pga) {
        s_settings.pga_half_db = s_pending.pga_v;
        changed = true;
        if (s_adc_configured && err == ESP_OK) {
            err = pcm1863_set_pga(s_settings.pga_half_db, &bad);
        }
    }
    if (s_pending.reconfig && s_adc_configured && err == ESP_OK) {
        const pcm1863_config_t cfg = adc_config();
        err = pcm1863_configure(&cfg, &bad);
        changed = true;
    }
    s_pending.build = s_pending.input = s_pending.pga = s_pending.reconfig = false;

    if (!changed) {
        return;
    }
    update_input_sel();
    s_dirty = true;
    /* Coupling caps, relay transfer and ADC fade-in (brief §5 mode change). */
    s_settle_until_us = now + (int64_t)CONFIG_P68_MODE_SETTLE_MS * 1000;
    ESP_LOGI(TAG, "mode: %s build, input %s, PGA %+.1f dB",
             settings_build_name(s_settings.build), settings_input_name(s_settings.input),
             s_settings.pga_half_db / 2.0);

    if (err != ESP_OK) {
        log_fault(FAULT_ADC_CONFIG, ((uint32_t)bad << 16) | ((uint32_t)err & 0xFFFF), -1);
        s_adc_configured = false;
        s_adc_ok = false;
        s_next_adc_try_us = now;
        s_adc_backoff_ms = 1000;
        enter(PROT_ADC_CONFIG);
    }
}

static void save_if_dirty(void)
{
    if (!s_dirty || state_is_armed(s_state) || s_state == PROT_RELEASE) {
        return;
    }
    /* Stalls the caches (and the audio task) briefly; the relay is open. */
    const esp_err_t err = settings_save(&s_settings);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "settings saved");
    } else {
        ESP_LOGE(TAG, "settings save failed: %s", esp_err_to_name(err));
    }
    s_dirty = false;
}

/* Starts RAMP_DOWN from an armed state (mode change or disarm). */
static void begin_ramp_down(void)
{
    if (s_state == PROT_RAMP_DOWN) {
        return;
    }
    audio_set_gain(0.0f, CONFIG_P68_MUTE_RAMP_MS);
    enter(PROT_RAMP_DOWN);
}

static bool pins_match(void)
{
    return board_pad(PIN_AUDIO_READY) == (bool)g_board_out.audio_ready &&
           board_pad(PIN_DAC_XSMT) == (bool)g_board_out.xsmt;
}

/* Why we may not arm yet, or NULL if all conditions hold. */
static const char *arm_blocker(int64_t now)
{
    if (s_hold) {
        return "disarmed (console)";
    }
    if (!s_adc_configured) {
        return "ADC not configured";
    }
    if (audio_get_wdt_test() != AUDIO_WDT_NORMAL) {
        return "watchdog test active";
    }
    if (!board_fault_n_ok() || s_fn_low ||
        ms_since(s_fn_high_since_us, now) < CONFIG_P68_FAULT_N_STABLE_MS) {
        return "FAULT_N low or not yet stable";
    }
    if (audio_healthy_for_ms() < VERIFY_HEALTHY_MS) {
        return "audio not healthy";
    }
    if (!adc_status_acceptable()) {
        return "ADC status";
    }
    if (board_relay_closed()) {
        return "IO3 says K1 closed while disarmed";
    }
    if (!pins_match()) {
        return "output pad mismatch";
    }
    if (now < s_settle_until_us) {
        return "settling";
    }
    return NULL;
}

/* Rising edge on AUDIO_READY, then IO3 must read low within 10 ms. */
static void do_arm(void)
{
    board_set_xsmt(false);
    board_set_audio_ready(false);
    esp_rom_delay_us(100);

    board_set_audio_ready(true);
    const int64_t t0 = now_us();
    int64_t t_first_low = 0;
    int stable = 0;
    bool closed = false;
    int64_t elapsed = 0;

    while (elapsed < RELAY_CLOSE_TIMEOUT_US) {
        if (board_relay_closed()) {
            if (stable == 0) {
                t_first_low = now_us();
            }
            if (++stable >= 2) {
                closed = true;
                break;
            }
        } else {
            stable = 0;
        }
        esp_rom_delay_us(RELAY_POLL_US);
        elapsed = now_us() - t0;
    }

    if (!closed) {
        fault(FAULT_RELAY_NO_CLOSE, (uint32_t)elapsed, -1);
        return;
    }
    if (!board_fault_n_ok()) {
        fault(FAULT_N_ASSERTED, 0, -1);
        return;
    }
    s_last_close_us = (uint32_t)(t_first_low - t0);
    s_arm_count++;
    s_relay_open_since_us = 0;
    ESP_LOGI(TAG, "armed: K1 closed %" PRIu32 " us after AUDIO_READY edge", s_last_close_us);
    enter(PROT_ARMED_WAIT);
}

/* Rate limit for automatic re-arming after faults. */
static bool rearm_allowed(int64_t now)
{
    const int64_t now_ms = now / 1000;
    int recent = 0;
    for (int i = 0; i < CONFIG_P68_AUTO_REARM_MAX; i++) {
        if (s_rearm_times_ms[i] != 0 && now_ms - s_rearm_times_ms[i] < REARM_WINDOW_MS) {
            recent++;
        }
    }
    if (recent >= CONFIG_P68_AUTO_REARM_MAX) {
        return false;
    }
    s_rearm_times_ms[s_rearm_head] = now_ms;
    s_rearm_head = (s_rearm_head + 1) % CONFIG_P68_AUTO_REARM_MAX;
    s_auto_rearms++;
    return true;
}

static void clear_rearm_history(void)
{
    memset(s_rearm_times_ms, 0, sizeof(s_rearm_times_ms));
}

/* --------------------------------------------------------------- monitors */

static void on_fault_n_fall(int32_t since_kick_ms)
{
    if (state_is_armed(s_state)) {
        s_fn_pending_entry = fault(FAULT_N_ASSERTED, 0, since_kick_ms);
    } else if (s_kicks_started) {
        s_fn_pending_entry = log_fault(FAULT_N_DISARMED, 0, since_kick_ms);
    } else {
        /* TPS3850 WDO pulses until the first WDI kick: expected at boot. */
        s_boot_fn_pulses++;
        s_fn_pending_entry = -1;
    }
}

static void track_fault_n(int64_t now)
{
    if (s_isr_flag) {
        s_isr_flag = false;
        if (!s_fn_low) {
            const uint32_t fall32 = s_isr_fall_us;
            s_fn_low = true;
            s_fn_fall_us = now - (int64_t)(uint32_t)((uint32_t)now - fall32);
            on_fault_n_fall((int32_t)((fall32 - s_isr_kick_us) / 1000));
        }
    }

    const bool high = board_fault_n_ok();
    if (!high && !s_fn_low) {
        s_fn_low = true;
        s_fn_fall_us = now;
        on_fault_n_fall((int32_t)(((uint32_t)now - g_audio_last_kick_us) / 1000));
    } else if (high && s_fn_low) {
        s_fn_low = false;
        s_fn_high_since_us = now;
        const int32_t low_ms = (int32_t)((now - s_fn_fall_us) / 1000);
        if (s_fn_pending_entry >= 0) {
            portENTER_CRITICAL(&s_log_mux);
            s_log[s_fn_pending_entry].fault_n_low_ms = low_ms;
            portEXIT_CRITICAL(&s_log_mux);
            ESP_LOGI(TAG, "FAULT_N high again after %" PRId32 " ms", low_ms);
        }
        s_fn_pending_entry = -1;
    }
}

static void monitor(int64_t now)
{
    /* Output pads must follow the command (catches a shorted AUDIO_READY). */
    if (!pins_match()) {
        if (state_is_armed(s_state)) {
            fault(FAULT_PIN_MISMATCH, ((uint32_t)board_pad(PIN_AUDIO_READY) << 1) | board_pad(PIN_DAC_XSMT), -1);
        } else if (!s_pin_mismatch_logged) {
            log_fault(FAULT_PIN_MISMATCH, ((uint32_t)board_pad(PIN_AUDIO_READY) << 1) | board_pad(PIN_DAC_XSMT), -1);
            s_pin_mismatch_logged = true;
        }
    } else {
        s_pin_mismatch_logged = false;
    }

    if (state_is_armed(s_state)) {
        if (!board_fault_n_ok()) {
            /* Normally caught by the ISR / track_fault_n first. */
            fault(FAULT_N_ASSERTED, 0, -1);
            return;
        }
        if (!board_relay_closed()) {
            if (s_relay_open_since_us == 0) {
                s_relay_open_since_us = now;
            } else if (ms_since(s_relay_open_since_us, now) >= RELAY_OPEN_DEBOUNCE_MS) {
                fault(FAULT_RELAY_OPENED, 0, -1);
                return;
            }
        } else {
            s_relay_open_since_us = 0;
        }
        if (!audio_is_healthy()) {
            audio_stats_t st;
            audio_get_stats(&st);
            fault(FAULT_AUDIO_UNHEALTHY, st.last_bad_flags, -1);
            return;
        }
        if (!adc_status_acceptable()) {
            fault(FAULT_ADC_STATUS, pack_adc_status(&s_adc_status), -1);
            return;
        }
        s_relay_stuck_since_us = 0;
        s_relay_stuck_logged = false;
    } else if (s_state != PROT_RELEASE && !g_board_out.audio_ready) {
        /* Disarmed: IO3 must read open (high). */
        if (board_relay_closed()) {
            if (s_relay_stuck_since_us == 0) {
                s_relay_stuck_since_us = now;
            } else if (!s_relay_stuck_logged && ms_since(s_relay_stuck_since_us, now) >= RELAY_RELEASE_MS) {
                log_fault(FAULT_RELAY_STUCK, 0, -1);
                s_relay_stuck_logged = true;
            }
        } else {
            s_relay_stuck_since_us = 0;
            s_relay_stuck_logged = false;
        }
    }
}

static void poll_adc(int64_t now)
{
    if (!s_adc_configured || now < s_next_adc_poll_us) {
        return;
    }
    s_next_adc_poll_us = now + (int64_t)(s_state == PROT_AUDIO_VERIFY ? ADC_POLL_VERIFY_MS : ADC_POLL_MS) * 1000;

    pcm1863_status_t st;
    if (pcm1863_read_status(&st) == ESP_OK) {
        s_adc_status = st;
        s_adc_status_valid = true;
        s_adc_ok = pcm1863_status_ok(&st);
        s_adc_i2c_fails = 0;
    } else if (++s_adc_i2c_fails >= ADC_I2C_FAIL_LIMIT) {
        s_adc_ok = false;
        if (s_adc_i2c_fails == ADC_I2C_FAIL_LIMIT) {
            if (state_is_armed(s_state)) {
                fault(FAULT_ADC_I2C, 0, -1);
            } else {
                log_fault(FAULT_ADC_I2C, 0, -1);
            }
        }
    }
}

/* ----------------------------------------------------------- state machine */

static bool pending_any(void)
{
    return s_pending.build || s_pending.input || s_pending.pga || s_pending.reconfig;
}

static void run_state(int64_t now)
{
    s_waiting_for = "";

    /* A change queued before a fault interrupted RAMP_DOWN: the relay is open
     * now, so apply it here. */
    if (pending_any() && (s_state == PROT_AUDIO_VERIFY || s_state == PROT_WAIT_CLEAR ||
                          s_state == PROT_FAULTED || s_state == PROT_LOCKOUT || s_state == PROT_HOLD)) {
        apply_pending(now);
    }
    const uint32_t in_state_ms = ms_since(s_state_enter_us, now);

    switch (s_state) {
    case PROT_BOOT:
        enter(PROT_ADC_CONFIG);
        break;

    case PROT_ADC_CONFIG: {
        if (now < s_next_adc_try_us) {
            break;
        }
        const pcm1863_config_t cfg = adc_config();
        uint8_t bad = 0;
        const esp_err_t err = pcm1863_configure(&cfg, &bad);
        if (err != ESP_OK) {
            log_fault(FAULT_ADC_CONFIG, ((uint32_t)bad << 16) | ((uint32_t)err & 0xFFFF), -1);
            s_next_adc_try_us = now + (int64_t)s_adc_backoff_ms * 1000;
            s_adc_backoff_ms = s_adc_backoff_ms < 5000 ? s_adc_backoff_ms * 2 : 5000;
            break;
        }
        s_adc_configured = true;
        s_adc_backoff_ms = 1000;
        s_adc_status_valid = false;
        s_next_adc_poll_us = now;
        update_input_sel();
        s_settle_until_us = now + (int64_t)CONFIG_P68_MODE_SETTLE_MS * 1000;

        if (!s_audio_started) {
            const esp_err_t aerr = audio_start((float)CONFIG_P68_HPF_HZ, dsp_db_to_lin(s_settings.limit_dbfs));
            if (aerr != ESP_OK) {
                log_fault(FAULT_AUDIO_START, (uint32_t)aerr, -1);
                s_next_adc_try_us = now + 5000 * 1000LL;
                break;
            }
            s_audio_started = true;
        }
        s_verify_timeout_logged = false;
        enter(PROT_AUDIO_VERIFY);
        break;
    }

    case PROT_AUDIO_VERIFY:
        if (audio_healthy_for_ms() >= VERIFY_HEALTHY_MS && adc_status_acceptable()) {
            audio_enable_kicks();
            s_kicks_started = true;
            ESP_LOGI(TAG, "audio verified, WDI kicking enabled");
            enter(s_hold ? PROT_HOLD : PROT_WAIT_CLEAR);
        } else if (in_state_ms > VERIFY_TIMEOUT_MS && !s_verify_timeout_logged) {
            audio_stats_t st;
            audio_get_stats(&st);
            log_fault(FAULT_VERIFY_TIMEOUT, st.last_bad_flags | (s_adc_ok ? 0 : 0x80000000u), -1);
            s_verify_timeout_logged = true;
        }
        s_waiting_for = !audio_is_healthy() ? "audio not healthy" : "ADC status";
        break;

    case PROT_WAIT_CLEAR: {
        if (s_hold) {
            enter(PROT_HOLD);
            break;
        }
        const char *blocker = arm_blocker(now);
        if (blocker) {
            s_waiting_for = blocker;
            break;
        }
        do_arm();
        break;
    }

    case PROT_ARMED_WAIT:
        if (in_state_ms >= UNMUTE_DELAY_MS) {
            board_set_xsmt(true);
            audio_set_gain(dsp_db_to_lin(s_settings.gain_db), CONFIG_P68_UNMUTE_RAMP_MS);
            enter(PROT_RAMP_UP);
        }
        break;

    case PROT_RAMP_UP:
        if (audio_ramp_done()) {
            enter(PROT_RUNNING);
        }
        break;

    case PROT_RUNNING:
        break;

    case PROT_RAMP_DOWN:
        if (audio_ramp_done() || in_state_ms >= RAMP_DOWN_TIMEOUT_MS) {
            board_set_xsmt(false);
            board_set_audio_ready(false);
            enter(PROT_RELEASE);
        }
        break;

    case PROT_RELEASE:
        if (!board_relay_closed()) {
            apply_pending(now);
            if (s_state != PROT_RELEASE) {
                break; /* apply_pending moved to ADC_CONFIG */
            }
            if (s_pending.disarm) {
                s_pending.disarm = false;
                enter(PROT_HOLD);
            } else {
                enter(s_hold ? PROT_HOLD : PROT_WAIT_CLEAR);
            }
        } else if (in_state_ms >= RELAY_RELEASE_MS) {
            fault(FAULT_RELAY_STUCK, in_state_ms, -1);
            s_relay_stuck_logged = true;
            apply_pending(now);
        }
        break;

    case PROT_FAULTED: {
        if (s_hold) {
            enter(PROT_HOLD);
            break;
        }
        if (s_fn_low || ms_since(s_fn_high_since_us, now) < CONFIG_P68_FAULT_SETTLE_MS ||
            in_state_ms < CONFIG_P68_FAULT_SETTLE_MS) {
            s_waiting_for = "FAULT_N settle";
            break;
        }
        const char *blocker = arm_blocker(now);
        if (blocker) {
            s_waiting_for = blocker;
            break;
        }
        if (!rearm_allowed(now)) {
            log_fault(FAULT_REARM_LIMIT, CONFIG_P68_AUTO_REARM_MAX, -1);
            enter(PROT_LOCKOUT);
            break;
        }
        ESP_LOGI(TAG, "fault cleared, re-arming automatically");
        enter(PROT_WAIT_CLEAR);
        break;
    }

    case PROT_LOCKOUT:
        s_waiting_for = "console `arm` (auto re-arm limit)";
        break;

    case PROT_HOLD:
        s_waiting_for = "console `arm`";
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------- requests */

static void queue_mode_change(int64_t now)
{
    if (state_is_armed(s_state)) {
        begin_ramp_down();
    } else if (s_state != PROT_RELEASE && s_state != PROT_BOOT && s_state != PROT_ADC_CONFIG) {
        /* Relay already open: apply now, re-arm waits for the settle time. */
        apply_pending(now);
    }
    /* BOOT / ADC_CONFIG: settings are picked up by the next configure. */
    if (s_state == PROT_ADC_CONFIG || s_state == PROT_BOOT) {
        if (s_pending.build) {
            s_settings.build = s_pending.build_v;
        }
        if (s_pending.input) {
            s_settings.input = s_pending.input_v;
        }
        if (s_pending.pga) {
            s_settings.pga_half_db = s_pending.pga_v;
        }
        s_pending.build = s_pending.input = s_pending.pga = s_pending.reconfig = false;
        s_dirty = true;
    }
}

static void handle_request(const request_t *r, int64_t now)
{
    switch (r->type) {
    case REQ_ARM:
        s_hold = false;
        s_pending.disarm = false;
        clear_rearm_history();
        if (s_state == PROT_HOLD || s_state == PROT_LOCKOUT || s_state == PROT_FAULTED) {
            enter(PROT_WAIT_CLEAR);
        }
        break;

    case REQ_DISARM:
        s_hold = true;
        if (state_is_armed(s_state)) {
            s_pending.disarm = true;
            begin_ramp_down();
        } else if (s_state == PROT_WAIT_CLEAR || s_state == PROT_FAULTED || s_state == PROT_LOCKOUT) {
            enter(PROT_HOLD);
        }
        break;

    case REQ_INPUT:
        s_pending.input = true;
        s_pending.input_v = (input_t)r->i;
        queue_mode_change(now);
        break;

    case REQ_BUILD:
        s_pending.build = true;
        s_pending.build_v = (build_t)r->i;
        queue_mode_change(now);
        break;

    case REQ_PGA:
        s_pending.pga = true;
        s_pending.pga_v = r->i;
        queue_mode_change(now);
        break;

    case REQ_ADC_RECONFIG:
        s_pending.reconfig = true;
        queue_mode_change(now);
        break;

    case REQ_GAIN:
        s_settings.gain_db = r->f;
        s_dirty = true;
        if (s_state == PROT_RAMP_UP || s_state == PROT_RUNNING) {
            audio_set_gain(dsp_db_to_lin(r->f), CONFIG_P68_UNMUTE_RAMP_MS);
        }
        break;

    case REQ_LIMIT:
        s_settings.limit_dbfs = r->f;
        s_dirty = true;
        audio_set_ceiling(dsp_db_to_lin(r->f));
        break;

    case REQ_SAVE:
        s_dirty = true; /* written by save_if_dirty() as soon as the relay is open */
        break;
    }
}

static void publish_status(int64_t now)
{
    prot_status_t st = {
        .state = s_state,
        .state_ms = ms_since(s_state_enter_us, now),
        .hold = s_hold,
        .fault_n_ok = board_fault_n_ok(),
        .relay_closed = board_relay_closed(),
        .waiting_for = s_waiting_for,
        .arm_count = s_arm_count,
        .last_relay_close_us = s_last_close_us,
        .auto_rearms = s_auto_rearms,
        .boot_fault_n_pulses = s_boot_fn_pulses,
        .adc_configured = s_adc_configured,
        .adc_status_valid = s_adc_status_valid,
        .adc_ok = s_adc_ok,
        .adc = s_adc_status,
        .settings = s_settings,
        .settings_dirty = s_dirty,
    };
    portENTER_CRITICAL(&s_status_mux);
    s_status = st;
    portEXIT_CRITICAL(&s_status_mux);
}

void protection_get_status(prot_status_t *st)
{
    portENTER_CRITICAL(&s_status_mux);
    *st = s_status;
    portEXIT_CRITICAL(&s_status_mux);
}

static void update_led(int64_t now)
{
    const uint32_t ms = (uint32_t)(now / 1000);
    bool on;
    if (s_state == PROT_RUNNING) {
        on = true;
    } else if (s_state == PROT_FAULTED || s_state == PROT_LOCKOUT) {
        on = (ms / 125) & 1;    /* 4 Hz */
    } else {
        on = (ms / 500) & 1;    /* 1 Hz */
    }
    if (on != (bool)g_board_out.led) {
        board_set_led(on);
    }
}

static void control_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    s_fn_high_since_us = now_us();
    s_adc_backoff_ms = 1000;
    enter(PROT_BOOT);

    for (;;) {
        ulTaskNotifyTake(pdTRUE, 1);
        esp_task_wdt_reset();
        const int64_t now = now_us();

        track_fault_n(now);
        monitor(now);

        request_t r;
        while (xQueueReceive(s_queue, &r, 0) == pdTRUE) {
            handle_request(&r, now);
        }

        run_state(now);
        poll_adc(now);
        save_if_dirty();
        update_led(now);
        publish_status(now);
    }
}

esp_err_t protection_start(const settings_t *initial)
{
    s_settings = *initial;
    s_queue = xQueueCreate(8, sizeof(request_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");

    if (xTaskCreatePinnedToCore(control_task, "protect", CONTROL_TASK_STACK, NULL, CONTROL_TASK_PRIORITY,
                                &s_task, CONTROL_TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* FAULT_N falling edge -> immediate mute. IRAM ISR so it also runs while
     * the flash cache is disabled. */
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(ESP_INTR_FLAG_IRAM), TAG, "isr service");
    ESP_RETURN_ON_ERROR(gpio_set_intr_type(PIN_FAULT_N, GPIO_INTR_NEGEDGE), TAG, "intr type");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_FAULT_N, fault_n_isr, NULL), TAG, "isr add");
    ESP_RETURN_ON_ERROR(gpio_intr_enable(PIN_FAULT_N), TAG, "intr enable");
    return ESP_OK;
}

static esp_err_t post(req_type_t type, int i, float f)
{
    const request_t r = {.type = type, .i = i, .f = f};
    return xQueueSend(s_queue, &r, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t protection_request_arm(void)
{
    return post(REQ_ARM, 0, 0);
}

esp_err_t protection_request_disarm(void)
{
    return post(REQ_DISARM, 0, 0);
}

esp_err_t protection_request_input(input_t input)
{
    return post(REQ_INPUT, input, 0);
}

esp_err_t protection_request_build(build_t build)
{
    return post(REQ_BUILD, build, 0);
}

esp_err_t protection_request_pga(int pga_half_db)
{
    return post(REQ_PGA, pga_half_db, 0);
}

esp_err_t protection_request_gain(float gain_db)
{
    return post(REQ_GAIN, 0, gain_db);
}

esp_err_t protection_request_limit(float limit_dbfs)
{
    return post(REQ_LIMIT, 0, limit_dbfs);
}

esp_err_t protection_request_save(void)
{
    return post(REQ_SAVE, 0, 0);
}

esp_err_t protection_request_adc_reconfig(void)
{
    return post(REQ_ADC_RECONFIG, 0, 0);
}
