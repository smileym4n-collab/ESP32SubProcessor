#include "pcm1863.h"

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PCM1863_I2C_HZ          100000
#define PCM1863_XFER_TIMEOUT_MS 20

/* Register values written by pcm1863_configure() (SLAS831D §13.3). */
#define ADC_SEL_RSV             0x40    /* bit 6 "always write 1", POL = normal */
#define ADC_SEL_VIN1            0x01    /* VINx1[SE] */
#define ADC_SEL_VIN3            0x04    /* VINx3[SE] */
/* RX_WLEN 24 (unused, default), TX_WLEN 32-bit, FMT I2S. A 32-bit word fills
 * the whole 64 fs slot, so DOUT never leaves undriven bits after the data. */
#define PCM_CFG_VALUE           0x40
/* SCK_XI_SEL = 01 "SCK" only (XI is grounded), slave, auto clock detect on.
 * With SCKI held static the detector selects BCK-PLL mode (Table 15). */
#define CLK_CTRL_VALUE          0x41
/* FLT = 1 short-latency (IIR) decimation filter, HPF_EN = 1 (ADC DC removal). */
#define DSP_CTRL_VALUE          0x30

static const char *TAG = "pcm1863";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;

esp_err_t pcm1863_bus_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = PIN_I2C0_SDA,
        .scl_io_num = PIN_I2C0_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C0 bus");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCM1863_I2C_ADDR,
        .scl_speed_hz = PCM1863_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "add device");

    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

i2c_master_bus_handle_t pcm1863_bus(void)
{
    return s_bus;
}

static esp_err_t write_locked(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), PCM1863_XFER_TIMEOUT_MS);
}

static esp_err_t read_locked(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, PCM1863_XFER_TIMEOUT_MS);
}

esp_err_t pcm1863_write(uint8_t reg, uint8_t val)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = write_locked(reg, val);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t pcm1863_read(uint8_t reg, uint8_t *val)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_locked(reg, val);
    xSemaphoreGive(s_lock);
    return err;
}

typedef struct {
    uint8_t reg;
    uint8_t val;
} reg_val_t;

/* Write each entry, then read each back and compare. Caller holds s_lock. */
static esp_err_t write_verify_locked(const reg_val_t *rv, size_t n, uint8_t *bad_reg)
{
    for (size_t i = 0; i < n; i++) {
        ESP_RETURN_ON_ERROR(write_locked(rv[i].reg, rv[i].val), TAG, "write 0x%02x", rv[i].reg);
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t got = 0;
        ESP_RETURN_ON_ERROR(read_locked(rv[i].reg, &got), TAG, "read 0x%02x", rv[i].reg);
        if (got != rv[i].val) {
            ESP_LOGE(TAG, "readback 0x%02x = 0x%02x, wrote 0x%02x", rv[i].reg, got, rv[i].val);
            if (bad_reg) {
                *bad_reg = rv[i].reg;
            }
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_OK;
}

static uint8_t input_sel_value(pcm1863_input_t input)
{
    return ADC_SEL_RSV | (input == PCM1863_INPUT_VIN3 ? ADC_SEL_VIN3 : ADC_SEL_VIN1);
}

static uint8_t pga_value(int pga_half_db)
{
    if (pga_half_db < -24) {
        pga_half_db = -24;
    } else if (pga_half_db > 80) {
        pga_half_db = 80;
    }
    return (uint8_t)(int8_t)pga_half_db;
}

esp_err_t pcm1863_configure(const pcm1863_config_t *cfg, uint8_t *bad_reg)
{
    esp_err_t err = i2c_master_probe(s_bus, PCM1863_I2C_ADDR, PCM1863_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no ACK at 0x%02x (%s)", PCM1863_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = write_locked(PCM1863_REG_PAGE, 0x00);
    if (err == ESP_OK) {
        err = write_locked(PCM1863_REG_PAGE, 0xFE); /* register reset */
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        err = write_locked(PCM1863_REG_PAGE, 0x00);
    }
    if (err == ESP_OK) {
        const uint8_t sel = input_sel_value(cfg->input);
        const uint8_t pga = pga_value(cfg->pga_half_db);
        const reg_val_t table[] = {
            {PCM1863_REG_CLK_CTRL, CLK_CTRL_VALUE},
            {PCM1863_REG_PCM_CFG, PCM_CFG_VALUE},
            {PCM1863_REG_DSP_CTRL, DSP_CTRL_VALUE},
            {PCM1863_REG_ADC1_SEL_L, sel},
            {PCM1863_REG_ADC1_SEL_R, sel},
            {PCM1863_REG_PGA_CH1_L, pga},
            {PCM1863_REG_PGA_CH1_R, pga},
        };
        err = write_verify_locked(table, sizeof(table) / sizeof(table[0]), bad_reg);
    }
    xSemaphoreGive(s_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "configured: BCK-PLL slave, I2S 32-bit, short-latency filter, input VIN%d, PGA %+.1f dB",
                 cfg->input == PCM1863_INPUT_VIN3 ? 3 : 1, cfg->pga_half_db / 2.0);
    }
    return err;
}

esp_err_t pcm1863_set_input(pcm1863_input_t input, uint8_t *bad_reg)
{
    const uint8_t sel = input_sel_value(input);
    const reg_val_t table[] = {
        {PCM1863_REG_ADC1_SEL_L, sel},
        {PCM1863_REG_ADC1_SEL_R, sel},
    };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = write_verify_locked(table, 2, bad_reg);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t pcm1863_set_pga(int pga_half_db, uint8_t *bad_reg)
{
    const uint8_t pga = pga_value(pga_half_db);
    const reg_val_t table[] = {
        {PCM1863_REG_PGA_CH1_L, pga},
        {PCM1863_REG_PGA_CH1_R, pga},
    };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = write_verify_locked(table, 2, bad_reg);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t pcm1863_read_status(pcm1863_status_t *st)
{
    esp_err_t err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = read_locked(PCM1863_REG_STATE, &st->state);
    if (err == ESP_OK) {
        err = read_locked(PCM1863_REG_FSAMPLE, &st->fsample);
    }
    if (err == ESP_OK) {
        err = read_locked(PCM1863_REG_DIV_STATUS, &st->div);
    }
    if (err == ESP_OK) {
        err = read_locked(PCM1863_REG_CLK_STATUS, &st->clk);
    }
    if (err == ESP_OK) {
        err = read_locked(PCM1863_REG_SUPPLY, &st->supply);
    }
    if (err == ESP_OK) {
        err = read_locked(PCM1863_REG_PLL_CTRL, &st->pll);
    }
    xSemaphoreGive(s_lock);
    return err;
}

bool pcm1863_status_ok(const pcm1863_status_t *st)
{
    const uint8_t clk_bad = 0x66; /* LRCKHLT | BCKHLT | LRCKERR | BCKERR (SCK halt is expected) */
    return (st->state & 0x0F) == 0x0F &&
           (st->fsample & 0x07) == 0x03 &&
           ((st->div >> 4) & 0x07) == 0x03 &&
           (st->clk & clk_bad) == 0 &&
           (st->supply & 0x07) == 0x07;
}

const char *pcm1863_state_name(uint8_t state)
{
    switch (state & 0x0F) {
    case 0x0: return "power-down";
    case 0x1: return "wait-clock";
    case 0x2: return "release-reset";
    case 0x3: return "standby";
    case 0x4: return "fade-in";
    case 0x5: return "fade-out";
    case 0x9: return "sleep";
    case 0xF: return "run";
    default:  return "reserved";
    }
}
