/*
 * Persistent configuration in NVS.
 *
 * Flash writes stall both cores' caches, which also stalls the audio task and
 * therefore the watchdog kicks. The protection task only calls settings_save()
 * while the output relay is commanded open (see protection.c).
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    BUILD_LOW_LEVEL = 0,    /* VINL1/VINR1, INPUT_SEL picks preamp or Marantz LFE */
    BUILD_HIGH_LEVEL = 1,   /* VINL3/VINR3 via RC4580, INPUT_SEL unused */
} build_t;

typedef enum {
    INPUT_PREAMP = 0,       /* K2/K3 de-energised, INPUT_SEL low */
    INPUT_LFE = 1,          /* K2/K3 energised, INPUT_SEL high */
} input_t;

#define SETTINGS_GAIN_DB_MIN    -60.0f
#define SETTINGS_GAIN_DB_MAX    12.0f
#define SETTINGS_LIMIT_DB_MIN   -40.0f
#define SETTINGS_LIMIT_DB_MAX   -1.0f
#define SETTINGS_PGA_HALF_MIN   -24     /* -12.0 dB */
#define SETTINGS_PGA_HALF_MAX   80      /* +40.0 dB */

typedef struct {
    build_t build;
    input_t input;
    int pga_half_db;        /* PCM1863 PGA in 0.5 dB steps */
    float gain_db;          /* digital gain before the limiter */
    float limit_dbfs;       /* limiter ceiling */
} settings_t;

/* Initialise NVS and load settings; out-of-range values fall back to defaults. */
esp_err_t settings_init(settings_t *out);
esp_err_t settings_save(const settings_t *s);
void settings_defaults(settings_t *s);
const char *settings_build_name(build_t b);
const char *settings_input_name(input_t i);
