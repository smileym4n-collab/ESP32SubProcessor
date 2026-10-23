/*
 * Host-testable DSP primitives for the P68 subwoofer controller.
 *
 * Everything in this module is plain C with no ESP-IDF dependency except the
 * biquad inner loop, which uses ESP-DSP's optimised dsps_biquad_f32() when
 * built under ESP-IDF and an identical ANSI C loop on the host. The unit tests
 * in test/host exercise the same source file.
 *
 * Sample convention: float audio is normalised so that +/-1.0 is digital full
 * scale. I2S words are signed 32-bit, left-justified (24-bit data in the MSBs).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_BIQUAD_COEFS 5

/*
 * One second-order section in ESP-DSP layout:
 * coef = { b0, b1, b2, a1, a2 } with a0 normalised to 1, direct form II state.
 */
typedef struct {
    float coef[DSP_BIQUAD_COEFS];
    float w[2];
} dsp_biquad_t;

/* RBJ cookbook high-pass. q = 0.7071 gives a 2nd-order Butterworth. */
void dsp_biquad_highpass(dsp_biquad_t *bq, float fc_hz, float q, float fs_hz);
void dsp_biquad_reset(dsp_biquad_t *bq);
/* in and out must not overlap. */
void dsp_biquad_process(dsp_biquad_t *bq, const float *in, float *out, int n);
/* False if the filter state has become non-finite or absurdly large. */
bool dsp_biquad_state_ok(const dsp_biquad_t *bq);
/* Magnitude response of the coefficients at f_hz, in dB. */
float dsp_biquad_response_db(const dsp_biquad_t *bq, float f_hz, float fs_hz);

/*
 * Second-order high-pass as a topology-preserving-transform state-variable
 * filter (A. Simper, "Linear Trapezoidal Integrated SVF", 2013).
 *
 * Its transfer function is identical to the RBJ biquad above (bilinear
 * transform, prewarped at fc), but its states stay near signal level instead
 * of growing by ~(fs / 2 pi fc)^2. At fc = 15 Hz / fs = 48 kHz in float32 the
 * ESP-DSP direct form II biquad leaves ~-38 dB of error on a 20 Hz signal; this
 * structure leaves ~-95 dB (see test/host). It is the default for the audio
 * path.
 */
typedef struct {
    float a1, a2, a3, k;
    float ic1, ic2;
} dsp_svf_t;

void dsp_svf_highpass(dsp_svf_t *f, float fc_hz, float q, float fs_hz);
void dsp_svf_reset(dsp_svf_t *f);
/* in and out may be the same buffer. */
void dsp_svf_highpass_process(dsp_svf_t *f, const float *in, float *out, int n);
bool dsp_svf_state_ok(const dsp_svf_t *f);
float dsp_svf_highpass_response_db(const dsp_svf_t *f, float f_hz, float fs_hz);

/*
 * Interleaved stereo I2S words (L, R, L, R, ...) to a mono float buffer,
 * (L + R) / 2. With identical channels (Marantz LFE mode) this returns that
 * channel unchanged.
 */
void dsp_i2s_to_mono(const int32_t *rx, float *mono, int frames);
/* Mono float to both I2S slots, saturating; non-finite samples become 0. */
void dsp_mono_to_i2s(const float *mono, int32_t *tx, int frames);

/*
 * Peak limiter with instant attack and exponential release. The gain applied
 * to each sample never exceeds ceiling / |x|, so |out| <= ceiling always.
 * Non-finite input samples are replaced with silence and counted.
 */
typedef struct {
    float ceiling;
    float gain;         /* current gain, 1.0 = no limiting */
    float reduction;    /* 1 - gain, the release state */
    float release_coef;
    float min_gain;     /* lowest gain since dsp_limiter_take_min_gain() */
    uint32_t nonfinite; /* count of NaN/Inf/huge input samples replaced with 0 */
} dsp_limiter_t;

void dsp_limiter_init(dsp_limiter_t *lim, float ceiling, float release_ms, float fs_hz);
void dsp_limiter_set_ceiling(dsp_limiter_t *lim, float ceiling);
void dsp_limiter_process(dsp_limiter_t *lim, float *buf, int n);
float dsp_limiter_take_min_gain(dsp_limiter_t *lim);

/*
 * Linear gain ramp. dsp_ramp_set() chooses a per-sample step so that a full
 * 0 -> 1 change takes ramp_ms; smaller changes finish proportionally sooner.
 */
typedef struct {
    float current;
    float target;
    float step;
} dsp_ramp_t;

void dsp_ramp_init(dsp_ramp_t *r, float initial);
void dsp_ramp_set(dsp_ramp_t *r, float target, float ramp_ms, float fs_hz);
void dsp_ramp_jump(dsp_ramp_t *r, float value);
void dsp_ramp_process(dsp_ramp_t *r, float *buf, int n);
bool dsp_ramp_done(const dsp_ramp_t *r);

/* Test-signal generator for bring-up (sine or periodic rectangular pulse). */
typedef enum {
    DSP_GEN_OFF = 0,
    DSP_GEN_SINE,
    DSP_GEN_PULSE,
} dsp_gen_mode_t;

typedef struct {
    dsp_gen_mode_t mode;
    float amp;
    float phase;      /* sine: cycles, 0..1 */
    float phase_inc;  /* sine: cycles per sample */
    uint32_t counter; /* pulse: sample counter */
    uint32_t period;  /* pulse: samples between pulse starts */
    uint32_t width;   /* pulse: samples high */
} dsp_gen_t;

void dsp_gen_sine(dsp_gen_t *g, float freq_hz, float amp, float fs_hz);
void dsp_gen_pulse(dsp_gen_t *g, float amp, float width_ms, float period_ms, float fs_hz);
void dsp_gen_off(dsp_gen_t *g);
void dsp_gen_process(dsp_gen_t *g, float *buf, int n);

float dsp_peak_abs(const float *buf, int n);
float dsp_db_to_lin(float db);
/* Returns -200 dB for zero or negative input. */
float dsp_lin_to_db(float lin);

#ifdef __cplusplus
}
#endif
