#include "dsp_core.h"

#include <math.h>

#ifdef ESP_PLATFORM
#include "dsps_biquad.h"
#endif

#define DSP_PI          3.14159265358979323846
#define I2S_FULL_SCALE  2147483648.0f
/* Anything this large is a numerical fault, not audio. */
#define DSP_SANE_LIMIT  1.0e6f

void dsp_biquad_highpass(dsp_biquad_t *bq, float fc_hz, float q, float fs_hz)
{
    const double w0 = 2.0 * DSP_PI * (double)fc_hz / (double)fs_hz;
    const double cw = cos(w0);
    const double alpha = sin(w0) / (2.0 * (double)q);
    const double a0 = 1.0 + alpha;

    bq->coef[0] = (float)(((1.0 + cw) / 2.0) / a0);
    bq->coef[1] = (float)((-(1.0 + cw)) / a0);
    bq->coef[2] = (float)(((1.0 + cw) / 2.0) / a0);
    bq->coef[3] = (float)((-2.0 * cw) / a0);
    bq->coef[4] = (float)((1.0 - alpha) / a0);
    dsp_biquad_reset(bq);
}

void dsp_biquad_reset(dsp_biquad_t *bq)
{
    bq->w[0] = 0.0f;
    bq->w[1] = 0.0f;
}

void dsp_biquad_process(dsp_biquad_t *bq, const float *in, float *out, int n)
{
#ifdef ESP_PLATFORM
    dsps_biquad_f32(in, out, n, bq->coef, bq->w);
#else
    /* Same direct form II recurrence as dsps_biquad_f32_ansi(). */
    float *c = bq->coef;
    float *w = bq->w;
    for (int i = 0; i < n; i++) {
        const float d0 = in[i] - c[3] * w[0] - c[4] * w[1];
        out[i] = c[0] * d0 + c[1] * w[0] + c[2] * w[1];
        w[1] = w[0];
        w[0] = d0;
    }
#endif
}

bool dsp_biquad_state_ok(const dsp_biquad_t *bq)
{
    /* Written so that NaN fails both comparisons. */
    return fabsf(bq->w[0]) < DSP_SANE_LIMIT && fabsf(bq->w[1]) < DSP_SANE_LIMIT;
}

float dsp_biquad_response_db(const dsp_biquad_t *bq, float f_hz, float fs_hz)
{
    const double w = 2.0 * DSP_PI * (double)f_hz / (double)fs_hz;
    const double c1 = cos(w), s1 = sin(w);
    const double c2 = cos(2.0 * w), s2 = sin(2.0 * w);
    const float *k = bq->coef;

    const double nr = k[0] + k[1] * c1 + k[2] * c2;
    const double ni = -(k[1] * s1 + k[2] * s2);
    const double dr = 1.0 + k[3] * c1 + k[4] * c2;
    const double di = -(k[3] * s1 + k[4] * s2);
    const double mag2 = (nr * nr + ni * ni) / (dr * dr + di * di);

    return (float)(10.0 * log10(mag2 > 1e-30 ? mag2 : 1e-30));
}

void dsp_svf_highpass(dsp_svf_t *f, float fc_hz, float q, float fs_hz)
{
    const double g = tan(DSP_PI * (double)fc_hz / (double)fs_hz);
    const double k = 1.0 / (double)q;
    const double a1 = 1.0 / (1.0 + g * (g + k));

    f->a1 = (float)a1;
    f->a2 = (float)(g * a1);
    f->a3 = (float)(g * g * a1);
    f->k = (float)k;
    dsp_svf_reset(f);
}

void dsp_svf_reset(dsp_svf_t *f)
{
    f->ic1 = 0.0f;
    f->ic2 = 0.0f;
}

void dsp_svf_highpass_process(dsp_svf_t *f, const float *in, float *out, int n)
{
    const float a1 = f->a1, a2 = f->a2, a3 = f->a3, k = f->k;
    float ic1 = f->ic1, ic2 = f->ic2;

    for (int i = 0; i < n; i++) {
        const float v0 = in[i];
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        out[i] = v0 - k * v1 - v2;
    }
    f->ic1 = ic1;
    f->ic2 = ic2;
}

bool dsp_svf_state_ok(const dsp_svf_t *f)
{
    return fabsf(f->ic1) < DSP_SANE_LIMIT && fabsf(f->ic2) < DSP_SANE_LIMIT;
}

float dsp_svf_highpass_response_db(const dsp_svf_t *f, float f_hz, float fs_hz)
{
    /* Equivalent direct-form coefficients of the same bilinear high-pass. */
    const double g = (double)f->a2 / (double)f->a1;
    const double k = f->k;
    const double d = 1.0 + g * k + g * g;
    dsp_biquad_t bq = {
        .coef = {
            (float)(1.0 / d),
            (float)(-2.0 / d),
            (float)(1.0 / d),
            (float)(2.0 * (g * g - 1.0) / d),
            (float)((1.0 - g * k + g * g) / d),
        },
    };
    return dsp_biquad_response_db(&bq, f_hz, fs_hz);
}

void dsp_i2s_to_mono(const int32_t *rx, float *mono, int frames)
{
    const float scale = 0.5f / I2S_FULL_SCALE;
    for (int i = 0; i < frames; i++) {
        mono[i] = ((float)rx[2 * i] + (float)rx[2 * i + 1]) * scale;
    }
}

void dsp_mono_to_i2s(const float *mono, int32_t *tx, int frames)
{
    for (int i = 0; i < frames; i++) {
        const float x = mono[i];
        int32_t v;
        if (x >= 1.0f) {
            v = INT32_MAX;
        } else if (x > -1.0f) {
            v = (int32_t)(x * I2S_FULL_SCALE);
        } else if (x <= -1.0f) {
            v = INT32_MIN;
        } else {
            v = 0; /* NaN */
        }
        tx[2 * i] = v;
        tx[2 * i + 1] = v;
    }
}

void dsp_limiter_init(dsp_limiter_t *lim, float ceiling, float release_ms, float fs_hz)
{
    lim->ceiling = ceiling;
    lim->gain = 1.0f;
    lim->reduction = 0.0f;
    lim->release_coef = 1.0f - expf(-1000.0f / (release_ms * fs_hz));
    lim->min_gain = 1.0f;
    lim->nonfinite = 0;
}

void dsp_limiter_set_ceiling(dsp_limiter_t *lim, float ceiling)
{
    lim->ceiling = ceiling;
}

void dsp_limiter_process(dsp_limiter_t *lim, float *buf, int n)
{
    const float ceiling = lim->ceiling;
    const float decay = 1.0f - lim->release_coef;
    /* State is the gain reduction (1 - gain): a small number keeps full float
     * precision, so the release does not stall just below unity. */
    float reduction = lim->reduction;
    float min_gain = lim->min_gain;

    for (int i = 0; i < n; i++) {
        float x = buf[i];
        float a = fabsf(x);
        if (!(a < DSP_SANE_LIMIT)) {
            x = 0.0f;
            a = 0.0f;
            lim->nonfinite++;
        }

        reduction *= decay;
        float gain = 1.0f - reduction;
        if (a > ceiling && a * gain > ceiling) {
            gain = ceiling / a;
            reduction = 1.0f - gain;
        }
        if (gain < min_gain) {
            min_gain = gain;
        }

        float y = x * gain;
        /* Guard against float rounding putting y a hair above the ceiling. */
        if (y > ceiling) {
            y = ceiling;
        } else if (y < -ceiling) {
            y = -ceiling;
        }
        buf[i] = y;
    }

    lim->reduction = reduction;
    lim->gain = 1.0f - reduction;
    lim->min_gain = min_gain;
}

float dsp_limiter_take_min_gain(dsp_limiter_t *lim)
{
    const float g = lim->min_gain;
    lim->min_gain = lim->gain;
    return g;
}

void dsp_ramp_init(dsp_ramp_t *r, float initial)
{
    r->current = initial;
    r->target = initial;
    r->step = 0.0f;
}

void dsp_ramp_set(dsp_ramp_t *r, float target, float ramp_ms, float fs_hz)
{
    const float samples = ramp_ms * fs_hz / 1000.0f;
    r->target = target;
    r->step = (samples >= 1.0f) ? 1.0f / samples : 1.0f;
}

void dsp_ramp_jump(dsp_ramp_t *r, float value)
{
    r->current = value;
    r->target = value;
}

void dsp_ramp_process(dsp_ramp_t *r, float *buf, int n)
{
    float g = r->current;
    const float t = r->target;
    const float s = r->step;

    if (g == t) {
        for (int i = 0; i < n; i++) {
            buf[i] *= g;
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        if (g < t) {
            g = (g + s < t) ? g + s : t;
        } else if (g > t) {
            g = (g - s > t) ? g - s : t;
        }
        buf[i] *= g;
    }
    r->current = g;
}

bool dsp_ramp_done(const dsp_ramp_t *r)
{
    return r->current == r->target;
}

void dsp_gen_sine(dsp_gen_t *g, float freq_hz, float amp, float fs_hz)
{
    g->mode = DSP_GEN_SINE;
    g->amp = amp;
    g->phase = 0.0f;
    g->phase_inc = freq_hz / fs_hz;
}

void dsp_gen_pulse(dsp_gen_t *g, float amp, float width_ms, float period_ms, float fs_hz)
{
    g->mode = DSP_GEN_PULSE;
    g->amp = amp;
    g->counter = 0;
    g->width = (uint32_t)(width_ms * fs_hz / 1000.0f);
    g->period = (uint32_t)(period_ms * fs_hz / 1000.0f);
    if (g->width < 1) {
        g->width = 1;
    }
    if (g->period <= g->width) {
        g->period = g->width + 1;
    }
}

void dsp_gen_off(dsp_gen_t *g)
{
    g->mode = DSP_GEN_OFF;
}

void dsp_gen_process(dsp_gen_t *g, float *buf, int n)
{
    switch (g->mode) {
    case DSP_GEN_SINE: {
        float ph = g->phase;
        for (int i = 0; i < n; i++) {
            buf[i] = g->amp * sinf(2.0f * (float)DSP_PI * ph);
            ph += g->phase_inc;
            if (ph >= 1.0f) {
                ph -= 1.0f;
            }
        }
        g->phase = ph;
        break;
    }
    case DSP_GEN_PULSE:
        for (int i = 0; i < n; i++) {
            buf[i] = (g->counter < g->width) ? g->amp : 0.0f;
            if (++g->counter >= g->period) {
                g->counter = 0;
            }
        }
        break;
    case DSP_GEN_OFF:
    default:
        break;
    }
}

float dsp_peak_abs(const float *buf, int n)
{
    float p = 0.0f;
    for (int i = 0; i < n; i++) {
        const float a = fabsf(buf[i]);
        if (a > p) {
            p = a;
        }
    }
    return p;
}

float dsp_db_to_lin(float db)
{
    return powf(10.0f, db / 20.0f);
}

float dsp_lin_to_db(float lin)
{
    return (lin > 1e-10f) ? 20.0f * log10f(lin) : -200.0f;
}
