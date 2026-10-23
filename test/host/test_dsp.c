/*
 * Host unit tests for components/dsp_core (no ESP-IDF needed).
 *
 *   make -C test/host
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp_core.h"

#define FS 48000.0f

static int s_failures;
static int s_checks;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        s_checks++;                                                         \
        if (!(cond)) {                                                      \
            s_failures++;                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

#define CHECK_NEAR(val, expect, tol, what)                                  \
    CHECK(fabs((double)(val) - (double)(expect)) <= (tol),                  \
          "%s = %.6f, expected %.6f +/- %.6f", what, (double)(val),         \
          (double)(expect), (double)(tol))

/* Steady-state RMS gain of the biquad at freq, measured in the time domain. */
static float measured_gain_db(dsp_biquad_t *bq, float freq)
{
    enum { N = 256 };
    float in[N], out[N];
    double sum_in = 0.0, sum_out = 0.0;
    float phase = 0.0f;
    const int settle_blocks = (int)(4.0f * FS / N);  /* 4 s */
    const int measure_blocks = (int)(2.0f * FS / N); /* 2 s */

    dsp_biquad_reset(bq);
    for (int b = 0; b < settle_blocks + measure_blocks; b++) {
        for (int i = 0; i < N; i++) {
            in[i] = 0.5f * sinf(2.0f * 3.14159265f * phase);
            phase += freq / FS;
            if (phase >= 1.0f) {
                phase -= 1.0f;
            }
        }
        dsp_biquad_process(bq, in, out, N);
        if (b >= settle_blocks) {
            for (int i = 0; i < N; i++) {
                sum_in += (double)in[i] * in[i];
                sum_out += (double)out[i] * out[i];
            }
        }
    }
    return (float)(10.0 * log10(sum_out / sum_in));
}

static void test_highpass_response(void)
{
    const float fc = 15.0f;
    dsp_biquad_t bq;
    dsp_biquad_highpass(&bq, fc, 0.70710678f, FS);

    /* Butterworth 2nd order: |H|^2 = r^4 / (1 + r^4), r = f / fc. */
    CHECK_NEAR(dsp_biquad_response_db(&bq, fc, FS), -3.0103, 0.05, "HPF at fc");
    CHECK_NEAR(dsp_biquad_response_db(&bq, 2.0f * fc, FS), 10.0 * log10(16.0 / 17.0), 0.05, "HPF at 2fc");
    CHECK_NEAR(dsp_biquad_response_db(&bq, fc / 2.0f, FS), 10.0 * log10(1.0 / 17.0), 0.1, "HPF at fc/2");
    CHECK_NEAR(dsp_biquad_response_db(&bq, 1000.0f, FS), 0.0, 0.01, "HPF at 1 kHz");
    CHECK(dsp_biquad_response_db(&bq, 0.01f, FS) < -100.0f, "HPF near DC not < -100 dB");

    /* The processing loop must realise the designed response. */
    CHECK_NEAR(measured_gain_db(&bq, 15.0f), -3.0103, 0.1, "measured HPF at fc");
    CHECK_NEAR(measured_gain_db(&bq, 100.0f), dsp_biquad_response_db(&bq, 100.0f, FS), 0.05,
               "measured HPF at 100 Hz");
}

static void test_svf_matches_biquad(void)
{
    dsp_svf_t svf;
    dsp_biquad_t bq;
    dsp_svf_highpass(&svf, 15.0f, 0.70710678f, FS);
    dsp_biquad_highpass(&bq, 15.0f, 0.70710678f, FS);

    const float freqs[] = {2.0f, 7.5f, 15.0f, 20.0f, 30.0f, 60.0f, 200.0f, 1000.0f, 10000.0f};
    for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        CHECK_NEAR(dsp_svf_highpass_response_db(&svf, freqs[i], FS),
                   dsp_biquad_response_db(&bq, freqs[i], FS), 0.01, "SVF vs RBJ response");
    }
    CHECK_NEAR(dsp_svf_highpass_response_db(&svf, 15.0f, FS), -3.0103, 0.05, "SVF at fc");
}

/* Time-domain error of each float32 structure against a double-precision
 * reference of the same filter: 20 Hz sine at -6 dBFS through the 15 Hz HPF. */
static void test_hpf_precision(void)
{
    const double fc = 15.0, q = 0.70710678, f = 20.0;
    const double w0 = 2.0 * 3.14159265358979 * fc / FS;
    const double cw = cos(w0), al = sin(w0) / (2.0 * q), a0 = 1.0 + al;
    const double b0 = (1.0 + cw) / 2.0 / a0, b1 = -(1.0 + cw) / a0, b2 = b0;
    const double a1 = -2.0 * cw / a0, a2 = (1.0 - al) / a0;

    dsp_svf_t svf;
    dsp_biquad_t bq;
    dsp_svf_highpass(&svf, (float)fc, (float)q, FS);
    dsp_biquad_highpass(&bq, (float)fc, (float)q, FS);

    double rx1 = 0, rx2 = 0, ry1 = 0, ry2 = 0;
    double err_svf = 0, err_bq = 0, sig = 0;
    for (int n = 0; n < (int)(6 * FS); n++) {
        const float x = 0.5f * sinf((float)(2.0 * 3.14159265358979 * f * n / FS));
        const double ry = b0 * x + b1 * rx1 + b2 * rx2 - a1 * ry1 - a2 * ry2;
        rx2 = rx1; rx1 = x; ry2 = ry1; ry1 = ry;

        float ys, yb;
        dsp_svf_highpass_process(&svf, &x, &ys, 1);
        dsp_biquad_process(&bq, &x, &yb, 1);
        if (n > (int)(2 * FS)) {
            err_svf += (ys - ry) * (ys - ry);
            err_bq += (yb - ry) * (yb - ry);
            sig += ry * ry;
        }
    }
    const double svf_db = 10.0 * log10(err_svf / sig);
    const double bq_db = 10.0 * log10(err_bq / sig);
    printf("info: 20 Hz through 15 Hz HPF, error vs double: SVF %.1f dB, DF2 biquad %.1f dB\n",
           svf_db, bq_db);
    CHECK(svf_db < -90.0, "SVF error %.1f dB, expected < -90 dB", svf_db);
}

static void test_highpass_removes_dc(void)
{
    enum { N = 64 };
    float in[N], out[N];
    dsp_svf_t svf;
    dsp_svf_highpass(&svf, 15.0f, 0.70710678f, FS);

    for (int i = 0; i < N; i++) {
        in[i] = 0.25f; /* -12 dBFS DC offset */
    }
    float tail = 0.0f;
    for (int b = 0; b < (int)(2.0f * FS / N); b++) {
        dsp_svf_highpass_process(&svf, in, out, N);
        tail = dsp_peak_abs(out, N);
    }
    CHECK(tail < 1e-6f, "DC residual after 2 s = %g", (double)tail);
    CHECK(dsp_svf_state_ok(&svf), "state not ok after DC");

    /* In-place processing gives the same result. */
    dsp_svf_reset(&svf);
    float ref[N];
    dsp_svf_highpass_process(&svf, in, ref, N);
    dsp_svf_reset(&svf);
    memcpy(out, in, sizeof(out));
    dsp_svf_highpass_process(&svf, out, out, N);
    CHECK(memcmp(out, ref, sizeof(out)) == 0, "in-place SVF differs");

    svf.ic1 = NAN;
    CHECK(!dsp_svf_state_ok(&svf), "NaN SVF state reported ok");
    dsp_biquad_t bq;
    dsp_biquad_highpass(&bq, 15.0f, 0.70710678f, FS);
    bq.w[0] = NAN;
    CHECK(!dsp_biquad_state_ok(&bq), "NaN biquad state reported ok");
}

static void test_limiter_ceiling(void)
{
    enum { N = 48 };
    float buf[N];
    dsp_limiter_t lim;
    const float ceiling = dsp_db_to_lin(-6.0f);
    dsp_limiter_init(&lim, ceiling, 100.0f, FS);

    /* +12 dBFS 50 Hz sine for 1 s: output must never exceed the ceiling. */
    float worst = 0.0f;
    float phase = 0.0f;
    for (int b = 0; b < (int)(FS / N); b++) {
        for (int i = 0; i < N; i++) {
            buf[i] = 4.0f * sinf(2.0f * 3.14159265f * phase);
            phase += 50.0f / FS;
        }
        dsp_limiter_process(&lim, buf, N);
        const float p = dsp_peak_abs(buf, N);
        if (p > worst) {
            worst = p;
        }
    }
    CHECK(worst <= ceiling, "limiter peak %.6f above ceiling %.6f", (double)worst, (double)ceiling);
    CHECK(worst > 0.99f * ceiling, "limiter peak %.6f far below ceiling", (double)worst);
    CHECK(dsp_limiter_take_min_gain(&lim) < 0.2f, "min gain not reported");

    /* Single full-scale spike: still clamped. */
    memset(buf, 0, sizeof(buf));
    buf[10] = 1000.0f;
    buf[11] = -1000.0f;
    dsp_limiter_process(&lim, buf, N);
    CHECK(dsp_peak_abs(buf, N) <= ceiling, "spike not clamped");
}

static void test_limiter_release_and_transparency(void)
{
    enum { N = 48 };
    float buf[N];
    dsp_limiter_t lim;
    dsp_limiter_init(&lim, 0.5f, 100.0f, FS);

    /* Small signals pass untouched. */
    for (int i = 0; i < N; i++) {
        buf[i] = 0.1f * (float)(i % 7 - 3) / 3.0f;
    }
    float ref[N];
    memcpy(ref, buf, sizeof(buf));
    dsp_limiter_process(&lim, buf, N);
    for (int i = 0; i < N; i++) {
        CHECK_NEAR(buf[i], ref[i], 1e-7, "transparent sample");
    }

    /* Hit it hard once (gain -> 0.05), then measure recovery. */
    buf[0] = 10.0f;
    dsp_limiter_process(&lim, buf, 1);
    CHECK_NEAR(lim.gain, 0.05f, 1e-6, "gain after 10.0 into 0.5 ceiling");

    const int release_samples = (int)(0.1f * FS);
    for (int i = 0; i < release_samples; i++) {
        buf[0] = 0.0f;
        dsp_limiter_process(&lim, buf, 1);
    }
    /* One time constant: 63.2 % of the way back to unity. */
    CHECK_NEAR(lim.gain, 0.05 + 0.95 * (1.0 - exp(-1.0)), 0.01, "gain after one release time constant");

    for (int i = 0; i < release_samples * 10; i++) {
        buf[0] = 0.0f;
        dsp_limiter_process(&lim, buf, 1);
    }
    CHECK(lim.gain > 0.9999f, "gain did not recover: %f", (double)lim.gain);
}

static void test_limiter_nonfinite(void)
{
    float buf[4] = {NAN, INFINITY, -INFINITY, 0.25f};
    dsp_limiter_t lim;
    dsp_limiter_init(&lim, 0.5f, 100.0f, FS);
    dsp_limiter_process(&lim, buf, 4);
    CHECK(buf[0] == 0.0f && buf[1] == 0.0f && buf[2] == 0.0f, "non-finite not silenced");
    CHECK_NEAR(buf[3], 0.25f, 1e-7, "finite sample after NaN");
    CHECK(lim.nonfinite == 3, "nonfinite count %u", (unsigned)lim.nonfinite);
}

static void test_mono_sum(void)
{
    int32_t rx[8];
    float mono[4];

    /* L = R: unchanged (LFE mode). */
    rx[0] = 0x12345600; rx[1] = 0x12345600;
    /* L = -R: cancels. */
    rx[2] = 0x40000000; rx[3] = -0x40000000;
    /* Full scale both. */
    rx[4] = INT32_MAX; rx[5] = INT32_MAX;
    rx[6] = INT32_MIN; rx[7] = INT32_MIN;
    dsp_i2s_to_mono(rx, mono, 4);

    CHECK_NEAR(mono[0], (double)0x12345600 / 2147483648.0, 1e-9, "mono L=R");
    CHECK_NEAR(mono[1], 0.0, 1e-9, "mono L=-R");
    CHECK_NEAR(mono[2], 1.0, 1e-6, "mono +FS");
    CHECK_NEAR(mono[3], -1.0, 1e-9, "mono -FS");

    /* One channel only: half amplitude. */
    rx[0] = 0x40000000; rx[1] = 0;
    dsp_i2s_to_mono(rx, mono, 1);
    CHECK_NEAR(mono[0], 0.25, 1e-9, "mono L only");

    /* Round trip of a 24-bit word is exact. */
    rx[0] = 0x12345600; rx[1] = 0x12345600;
    dsp_i2s_to_mono(rx, mono, 1);
    int32_t tx[2];
    dsp_mono_to_i2s(mono, tx, 1);
    CHECK(tx[0] == 0x12345600 && tx[1] == 0x12345600, "round trip 0x%08x", (unsigned)tx[0]);
}

static void test_output_saturation(void)
{
    float mono[5] = {1.5f, -1.5f, NAN, 1.0f, -1.0f};
    int32_t tx[10];
    dsp_mono_to_i2s(mono, tx, 5);
    CHECK(tx[0] == INT32_MAX && tx[1] == INT32_MAX, "+1.5 not saturated");
    CHECK(tx[2] == INT32_MIN && tx[3] == INT32_MIN, "-1.5 not saturated");
    CHECK(tx[4] == 0 && tx[5] == 0, "NaN not zeroed");
    CHECK(tx[6] == INT32_MAX, "+1.0 not saturated");
    CHECK(tx[8] == INT32_MIN, "-1.0 not INT32_MIN");
}

static void test_ramp(void)
{
    enum { N = 32 };
    float buf[N];
    dsp_ramp_t r;
    dsp_ramp_init(&r, 0.0f);
    dsp_ramp_set(&r, 1.0f, 200.0f, FS);

    const int total = (int)(0.2f * FS);
    float last = -1.0f;
    int done_at = -1;
    for (int s = 0; s < total + 2 * N; s += N) {
        for (int i = 0; i < N; i++) {
            buf[i] = 1.0f;
        }
        dsp_ramp_process(&r, buf, N);
        for (int i = 0; i < N; i++) {
            CHECK(buf[i] >= last, "ramp not monotonic");
            last = buf[i];
        }
        if (s + N == total / 2) {
            CHECK_NEAR(r.current, 0.5, 0.01, "ramp halfway");
        }
        if (done_at < 0 && dsp_ramp_done(&r)) {
            done_at = s + N;
        }
    }
    CHECK(done_at > 0 && abs(done_at - total) <= N, "ramp finished at %d, expected ~%d", done_at, total);
    CHECK(r.current == 1.0f, "ramp did not land on target");

    dsp_ramp_set(&r, 0.0f, 50.0f, FS);
    for (int s = 0; s < (int)(0.05f * FS) + N; s += N) {
        for (int i = 0; i < N; i++) {
            buf[i] = 1.0f;
        }
        dsp_ramp_process(&r, buf, N);
    }
    CHECK(r.current == 0.0f && buf[N - 1] == 0.0f, "ramp down did not reach exact 0");
}

static void test_db_helpers(void)
{
    CHECK_NEAR(dsp_db_to_lin(-6.0206f), 0.5, 1e-4, "db_to_lin(-6.02)");
    CHECK_NEAR(dsp_lin_to_db(0.5f), -6.0206, 1e-3, "lin_to_db(0.5)");
    CHECK(dsp_lin_to_db(0.0f) == -200.0f, "lin_to_db(0)");
}

static void test_generator(void)
{
    enum { N = 480 };
    float buf[N];
    dsp_gen_t g;
    dsp_gen_sine(&g, 1000.0f, 0.5f, FS);
    dsp_gen_process(&g, buf, N);
    CHECK_NEAR(dsp_peak_abs(buf, N), 0.5, 1e-3, "sine peak");

    dsp_gen_pulse(&g, 0.25f, 2.0f, 500.0f, FS);
    dsp_gen_process(&g, buf, N);
    int high = 0;
    for (int i = 0; i < N; i++) {
        high += buf[i] != 0.0f;
    }
    CHECK(high == 96, "pulse width %d samples, expected 96", high);
}

int main(void)
{
    test_highpass_response();
    test_svf_matches_biquad();
    test_hpf_precision();
    test_highpass_removes_dc();
    test_limiter_ceiling();
    test_limiter_release_and_transparency();
    test_limiter_nonfinite();
    test_mono_sum();
    test_output_saturation();
    test_ramp();
    test_db_helpers();
    test_generator();

    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
