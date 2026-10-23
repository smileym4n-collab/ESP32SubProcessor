/*
 * P68 Subwoofer DSP Controller: pin map.
 *
 * Single source of truth for GPIO assignments, taken from the schematic netlist
 * (docs/hardware/FIRMWARE_BRIEF.md §3, 2026-10-23). Nothing else in the
 * firmware may name a GPIO number directly.
 */
#pragma once

#include "driver/gpio.h"

/* Outputs. All are held low from the first line of app_main. */
#define PIN_DAC_XSMT            GPIO_NUM_1   /* PCM5102A soft mute, low = muted, 100 k pull-down */
#define PIN_WDI                 GPIO_NUM_2   /* TPS3850 WDI (U7), 100 k pull-down */
#define PIN_I2S_BCK             GPIO_NUM_10  /* 3.072 MHz (64 fs) to ADC and DAC */
#define PIN_I2S_LRCK            GPIO_NUM_11  /* 48 kHz */
#define PIN_AUDIO_READY         GPIO_NUM_16  /* Clocks fault latch U13 on a rising edge, ANDs K1 drive */
#define PIN_I2S_DOUT            GPIO_NUM_38  /* To PCM5102A */
#define PIN_INPUT_SEL           GPIO_NUM_45  /* Strap, must be low at reset. High = Marantz LFE (LOW-LEVEL build) */
#define PIN_STATUS_LED          GPIO_NUM_46  /* Strap, local LED + J6 pin 19 */

/* Inputs. Every input has an external pull, so internal pulls stay off. */
#define PIN_OUT_RLY_SENSE       GPIO_NUM_3   /* K1 pole B, LOW when K1 is closed. Strap pin: input only */
#define PIN_I2S_DIN             GPIO_NUM_7   /* From PCM1863 DOUT */
#define PIN_ADC_SCKI            GPIO_NUM_9   /* PCM1863 SCKI, R33 100 k to GND. NEVER DRIVE: keeps the ADC in BCK-PLL mode */
#define PIN_USB_VBUS_SENSE      GPIO_NUM_17  /* High when USB is plugged in */
#define PIN_FAULT_N             GPIO_NUM_42  /* Wired-OR of all supervisors + watchdog WDO, low = fault */
#define PIN_MODE_TRIG_N         GPIO_NUM_47  /* Isolated 12 V trigger, low = trigger present */
#define PIN_ADC_INT             GPIO_NUM_48  /* PCM1863 GPIO3/INTC (unused in Phase 1) */

/* Front-panel encoders (Phase 3), active low, RC filtered on the main board. */
#define PIN_ENC1_A              GPIO_NUM_4
#define PIN_ENC1_B              GPIO_NUM_5
#define PIN_ENC1_SW             GPIO_NUM_6
#define PIN_ENC2_A              GPIO_NUM_15
#define PIN_ENC2_B              GPIO_NUM_18
#define PIN_ENC2_SW             GPIO_NUM_8
#define PIN_ENC3_A              GPIO_NUM_39
#define PIN_ENC3_B              GPIO_NUM_40
#define PIN_ENC3_SW             GPIO_NUM_41

/* I2C0: PCM1863 control only (on-board). I2C1: front panel / display via J6. */
#define PIN_I2C0_SDA            GPIO_NUM_12
#define PIN_I2C0_SCL            GPIO_NUM_13
#define PIN_I2C1_SDA            GPIO_NUM_14
#define PIN_I2C1_SCL            GPIO_NUM_21

/* IO19/IO20 are USB D-/D+ (console). IO43/IO44 are UART0 on J11. IO35-37 unused. */
