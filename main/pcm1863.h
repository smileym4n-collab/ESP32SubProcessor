/*
 * PCM1863 (IC2) control over I2C0.
 *
 * Register values are from TI SLAS831D (March 2018), §9.5.1.4 and §13.3.
 * All registers used here are on page 0.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define PCM1863_I2C_ADDR        0x4A    /* 1001 01 + AD(=GND), SLAS831D Table 23 */

#define PCM1863_REG_PAGE        0x00    /* write 0x00 = page 0, 0xFE = register reset */
#define PCM1863_REG_PGA_CH1_L   0x01    /* signed 7.1 dB format, -12.0 .. +40.0 dB */
#define PCM1863_REG_PGA_CH1_R   0x02
#define PCM1863_REG_ADC1_SEL_L  0x06    /* POL[7], RSV[6] (always 1), SEL[5:0] */
#define PCM1863_REG_ADC1_SEL_R  0x07
#define PCM1863_REG_PCM_CFG     0x0B    /* RX_WLEN[7:6], TX_WLEN[3:2], FMT[1:0] */
#define PCM1863_REG_CLK_CTRL    0x20    /* SCK_XI_SEL[7:6], MST_MODE[4], CLKDET_EN[0] */
#define PCM1863_REG_PLL_CTRL    0x28    /* LOCK[4] */
#define PCM1863_REG_POWER_CTRL  0x70
#define PCM1863_REG_DSP_CTRL    0x71    /* FLT[5] short-latency filter, HPF_EN[4] */
#define PCM1863_REG_STATE       0x72    /* STATE[3:0], 0xF = run */
#define PCM1863_REG_FSAMPLE     0x73    /* INFO[2:0], 3 = 32-48 kHz */
#define PCM1863_REG_DIV_STATUS  0x74    /* BCK_RATIO[6:4] (3 = 64), SCK_RATIO[2:0] */
#define PCM1863_REG_CLK_STATUS  0x75    /* LRCKHLT[6] BCKHLT[5] SCKHLT[4] LRCKERR[2] BCKERR[1] SCKERR[0] */
#define PCM1863_REG_SUPPLY      0x78    /* DVDD[2] AVDD[1] LDO[0], 1 = good */

typedef enum {
    PCM1863_INPUT_VIN1 = 0, /* LOW-LEVEL build: VINL1 / VINR1 single-ended */
    PCM1863_INPUT_VIN3,     /* HIGH-LEVEL build: VINL3 / VINR3 single-ended */
} pcm1863_input_t;

typedef struct {
    pcm1863_input_t input;
    int pga_half_db;        /* -24 .. +80 (-12.0 .. +40.0 dB) */
} pcm1863_config_t;

typedef struct {
    uint8_t state;      /* 0x72 */
    uint8_t fsample;    /* 0x73 */
    uint8_t div;        /* 0x74 */
    uint8_t clk;        /* 0x75 */
    uint8_t supply;     /* 0x78 */
    uint8_t pll;        /* 0x28 */
} pcm1863_status_t;

/* Creates I2C0 at 100 kHz (external pull-ups on the board). */
esp_err_t pcm1863_bus_init(void);
i2c_master_bus_handle_t pcm1863_bus(void);

/*
 * Probe, register reset, write the full configuration, then read every
 * written register back. On a readback mismatch returns ESP_ERR_INVALID_RESPONSE
 * and, if bad_reg is non-NULL, the first mismatching register address.
 */
esp_err_t pcm1863_configure(const pcm1863_config_t *cfg, uint8_t *bad_reg);

/* Input routing and PGA alone (mode change); also verified by readback. */
esp_err_t pcm1863_set_input(pcm1863_input_t input, uint8_t *bad_reg);
esp_err_t pcm1863_set_pga(int pga_half_db, uint8_t *bad_reg);

esp_err_t pcm1863_read_status(pcm1863_status_t *st);
/* True when the ADC is running from BCK at 48 kHz / 64 fs with good supplies. */
bool pcm1863_status_ok(const pcm1863_status_t *st);
const char *pcm1863_state_name(uint8_t state);

/* Raw page-0 access for the console. */
esp_err_t pcm1863_read(uint8_t reg, uint8_t *val);
esp_err_t pcm1863_write(uint8_t reg, uint8_t val);
