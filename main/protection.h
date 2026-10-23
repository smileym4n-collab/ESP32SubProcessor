/*
 * Protection and arming state machine (FIRMWARE_BRIEF §5).
 *
 * Runs in its own task on core 0 with a 1 ms tick. It is the only code that
 * drives AUDIO_READY, XSMT and INPUT_SEL (apart from the FAULT_N interrupt,
 * which can only force XSMT and AUDIO_READY low), and the only code that
 * writes settings to flash, which it does only while the relay is commanded
 * open.
 *
 *   BOOT -> ADC_CONFIG -> AUDIO_VERIFY -> WAIT_CLEAR -> (arm) -> ARMED_WAIT
 *        -> RAMP_UP -> RUNNING
 *   any armed state --fault--> FAULTED --settled, healthy--> WAIT_CLEAR
 *   RUNNING --mode change / disarm--> RAMP_DOWN -> RELEASE -> WAIT_CLEAR | HOLD
 *   FAULTED --too many auto re-arms--> LOCKOUT (console `arm` to leave)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pcm1863.h"
#include "settings.h"

typedef enum {
    PROT_BOOT = 0,
    PROT_ADC_CONFIG,    /* configuring PCM1863, retrying with backoff on failure */
    PROT_AUDIO_VERIFY,  /* I2S running; waiting for healthy audio + ADC status before kicking WDI */
    PROT_WAIT_CLEAR,    /* kicking; waiting for FAULT_N stable, settle, relay open, then arm */
    PROT_ARMED_WAIT,    /* AUDIO_READY high, K1 confirmed closed, 20 ms before XSMT */
    PROT_RAMP_UP,       /* XSMT high, gain ramping up */
    PROT_RUNNING,
    PROT_RAMP_DOWN,     /* gain ramping down before a mode change or disarm */
    PROT_RELEASE,       /* XSMT and AUDIO_READY low, waiting for IO3 to confirm K1 open */
    PROT_FAULTED,       /* muted and disarmed after a fault; auto re-arm when settled */
    PROT_LOCKOUT,       /* auto re-arm limit reached; needs console `arm` */
    PROT_HOLD,          /* disarmed by the console; needs `arm` */
    PROT_STATE_COUNT,
} prot_state_t;

typedef enum {
    FAULT_NONE = 0,
    FAULT_N_ASSERTED,       /* FAULT_N low while armed */
    FAULT_N_DISARMED,       /* FAULT_N low while disarmed (after WDI kicking started) */
    FAULT_RELAY_NO_CLOSE,   /* IO3 not low within 10 ms of the AUDIO_READY edge */
    FAULT_RELAY_OPENED,     /* IO3 went high while armed */
    FAULT_RELAY_STUCK,      /* IO3 low while AUDIO_READY is low */
    FAULT_AUDIO_UNHEALTHY,  /* audio health check failed while armed (detail = AUDIO_HF_*) */
    FAULT_ADC_CONFIG,       /* PCM1863 probe / write / readback failed */
    FAULT_ADC_STATUS,       /* PCM1863 not running at 48 kHz from BCK while armed */
    FAULT_ADC_I2C,          /* PCM1863 status reads failing */
    FAULT_AUDIO_START,      /* I2S driver failed to start */
    FAULT_VERIFY_TIMEOUT,   /* audio or ADC not healthy 5 s after I2S start */
    FAULT_PIN_MISMATCH,     /* AUDIO_READY or XSMT pad level differs from the command */
    FAULT_REARM_LIMIT,      /* auto re-arm limit reached, entering LOCKOUT */
    FAULT_COUNT,
} fault_cause_t;

typedef struct {
    uint32_t seq;
    uint32_t t_ms;              /* ms since boot of the first occurrence */
    uint32_t count;             /* identical consecutive occurrences folded in */
    fault_cause_t cause;
    uint32_t detail;
    int32_t fault_n_low_ms;     /* FAULT_N low duration, -1 if n/a or still low */
    int32_t since_kick_ms;      /* last WDI kick -> FAULT_N fall, -1 if n/a */
    prot_state_t state;         /* state when it happened */
} fault_entry_t;

typedef struct {
    prot_state_t state;
    uint32_t state_ms;
    bool hold;
    bool fault_n_ok;
    bool relay_closed;
    const char *waiting_for;    /* why WAIT_CLEAR / FAULTED is not arming, or "" */
    uint32_t arm_count;
    uint32_t last_relay_close_us;   /* AUDIO_READY edge -> IO3 low, last arm */
    uint32_t auto_rearms;
    uint32_t boot_fault_n_pulses;   /* FAULT_N pulses before WDI kicking started */
    bool adc_configured;
    bool adc_status_valid;
    bool adc_ok;
    pcm1863_status_t adc;
    settings_t settings;
    bool settings_dirty;
} prot_status_t;

/* Installs the FAULT_N interrupt and starts the control task. */
esp_err_t protection_start(const settings_t *initial);

void protection_get_status(prot_status_t *st);
/* Copies up to max entries, newest first. Returns the number copied. */
int protection_get_faults(fault_entry_t *out, int max);
void protection_get_fault_counts(uint32_t counts[FAULT_COUNT]);
void protection_clear_faults(void);

const char *protection_state_name(prot_state_t s);
const char *protection_fault_name(fault_cause_t c);
bool protection_is_armed(void);

/* Console requests, executed by the control task. ESP_ERR_TIMEOUT if the queue is full. */
esp_err_t protection_request_arm(void);
esp_err_t protection_request_disarm(void);
esp_err_t protection_request_input(input_t input);
esp_err_t protection_request_build(build_t build);
esp_err_t protection_request_pga(int pga_half_db);
esp_err_t protection_request_gain(float gain_db);
esp_err_t protection_request_limit(float limit_dbfs);
esp_err_t protection_request_save(void);
esp_err_t protection_request_adc_reconfig(void);
