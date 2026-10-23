/*
 * USB-Serial-JTAG command console for bench bring-up (DESIGN_REVIEW §8).
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/* ui_bus may be NULL if I2C1 failed to start; `i2c scan 1` then reports it. */
esp_err_t console_start(i2c_master_bus_handle_t ui_bus);
