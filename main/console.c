#include "console.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "board_io.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcm1863.h"
#include "protection.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "console";
static i2c_master_bus_handle_t s_ui_bus;

/* ----------------------------------------------------------------- helpers */

static bool parse_float(const char *s, float *out)
{
    char *end = NULL;
    const float v = strtof(s, &end);
    if (end == s || *end != '\0' || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_int(const char *s, long *out)
{
    char *end = NULL;
    const long v = strtol(s, &end, 0);
    if (end == s || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

static int report(esp_err_t err)
{
    if (err == ESP_OK) {
        printf("ok\n");
        return 0;
    }
    printf("error: %s\n", esp_err_to_name(err));
    return 1;
}

static float db(float lin)
{
    return dsp_lin_to_db(lin);
}

static const char *wdt_test_name(audio_wdt_test_t m)
{
    switch (m) {
    case AUDIO_WDT_STARVE: return "STARVE";
    case AUDIO_WDT_FAST: return "FAST";
    default: return "normal";
    }
}

static void print_health_flags(uint32_t f)
{
    if (f == 0) {
        printf("none");
        return;
    }
    static const char *const names[] = {"read-error", "rx-overflow", "tx-underrun", "block-gap",
                                        "period-timing", "adc-stuck", "dsp-nonfinite", "write-error"};
    bool first = true;
    for (int i = 0; i < 8; i++) {
        if (f & (1u << i)) {
            printf("%s%s", first ? "" : ",", names[i]);
            first = false;
        }
    }
}

static const char *classify_fault_n(int32_t low_ms)
{
    if (low_ms < 0) {
        return "";
    }
    if (low_ms < 150) {
        return " (short: supervisor glitch?)";
    }
    if (low_ms <= 260) {
        return " (~200 ms: TPS3850 WDO pulse)";
    }
    return " (sustained: rail supervisor)";
}

/* ---------------------------------------------------------------- commands */

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    prot_status_t p;
    audio_stats_t a;
    protection_get_status(&p);
    audio_get_stats(&a);
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    printf("P68 Sub DSP, Phase 1 bring-up    uptime %.1f s\n", now_ms / 1000.0);
    printf("State     %s (%.1f s)%s%s\n", protection_state_name(p.state), p.state_ms / 1000.0,
           p.waiting_for[0] ? "   waiting for: " : "", p.waiting_for);
    printf("Pins      FAULT_N=%d%s  IO3=%d (K1 %s)  AUDIO_READY cmd %d pad %d  XSMT cmd %d pad %d\n",
           p.fault_n_ok, p.fault_n_ok ? "" : " FAULT", !p.relay_closed, p.relay_closed ? "closed" : "open",
           g_board_out.audio_ready, board_pad(PIN_AUDIO_READY), g_board_out.xsmt, board_pad(PIN_DAC_XSMT));
    printf("          INPUT_SEL=%d  VBUS=%d  12V-TRIG=%s  ADC_INT=%d\n", g_board_out.input_sel,
           board_vbus_present(), board_trigger_present() ? "present" : "absent", board_pad(PIN_ADC_INT));
    printf("Arming    arms=%" PRIu32 "  last K1 close %" PRIu32 " us  auto re-arms=%" PRIu32
           "  boot FAULT_N pulses=%" PRIu32 "%s\n",
           p.arm_count, p.last_relay_close_us, p.auto_rearms, p.boot_fault_n_pulses, p.hold ? "  HOLD" : "");
    printf("Watchdog  %s, test %s, kicks=%" PRIu32 ", last %" PRIu32 " ms ago, interval %.1f..%.1f ms\n",
           a.kicks_enabled ? "kicking" : "not started", wdt_test_name(a.wdt_test), a.kicks,
           (now_ms * 1000u - a.last_kick_us) / 1000u, a.kick_interval_min_us / 1000.0,
           a.kick_interval_max_us / 1000.0);
    printf("Audio     %s", a.running ? (a.healthy ? "healthy" : "UNHEALTHY") : "not running");
    if (a.healthy) {
        printf(" %.1f s", a.healthy_for_ms / 1000.0);
    }
    printf("  last bad: ");
    print_health_flags(a.last_bad_flags);
    printf("\n          blocks=%" PRIu32 " bad periods=%" PRIu32 "/%" PRIu32 " rx_ovf=%" PRIu32
           " tx_ovf=%" PRIu32 " read_err=%" PRIu32 " write_err=%" PRIu32 "\n",
           a.blocks, a.bad_periods, a.periods, a.rx_ovf, a.tx_ovf, a.read_errors, a.write_errors);
    printf("          period %.2f ms, block gap max %.2f ms (ever %.2f)\n", a.period_us / 1000.0,
           a.gap_max_us / 1000.0, a.gap_max_us_ever / 1000.0);
    const uint32_t budget = 5000u * AUDIO_BLOCK_FRAMES; /* 240 MHz cycles per block */
    printf("DSP       %" PRIu32 " cycles/block avg, %" PRIu32 " max (%.1f%% of core 1), HPF %d Hz %s, "
           "nonfinite=%" PRIu32 " resets=%" PRIu32 "\n",
           a.cycles_avg, a.cycles_max, 100.0 * a.cycles_avg / budget, CONFIG_P68_HPF_HZ,
           AUDIO_HPF_NAME, a.nonfinite, a.dsp_resets);
    printf("Levels    in L %.1f  R %.1f dBFS   out %.1f dBFS   limiter GR %.1f dB   gain %.1f -> %.1f dB\n",
           db(a.in_peak_l), db(a.in_peak_r), db(a.out_peak), -db(a.limiter_min_gain), db(a.gain_now),
           db(a.gain_target));
    if (p.adc_status_valid) {
        printf("ADC       %s: state %s, fs code %d, BCK ratio code %d, clk 0x%02x, supply 0x%02x, PLL lock %d\n",
               p.adc_ok ? "ok" : "BAD", pcm1863_state_name(p.adc.state), p.adc.fsample & 7,
               (p.adc.div >> 4) & 7, p.adc.clk, p.adc.supply, (p.adc.pll >> 4) & 1);
    } else {
        printf("ADC       %s\n", p.adc_configured ? "configured, no status yet" : "NOT configured");
    }
    printf("Settings  %s, input %s, PGA %+.1f dB, gain %+.1f dB, limit %.1f dBFS%s\n",
           settings_build_name(p.settings.build), settings_input_name(p.settings.input),
           p.settings.pga_half_db / 2.0, p.settings.gain_db, p.settings.limit_dbfs,
           p.settings_dirty ? " (not yet saved)" : "");
    return 0;
}

static int cmd_arm(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return report(protection_request_arm());
}

static int cmd_disarm(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return report(protection_request_disarm());
}

static int cmd_input(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "preamp") && strcmp(argv[1], "lfe"))) {
        printf("usage: input preamp|lfe\n");
        return 1;
    }
    prot_status_t p;
    protection_get_status(&p);
    if (p.settings.build == BUILD_HIGH_LEVEL) {
        printf("note: HIGH-LEVEL build, INPUT_SEL stays low; setting stored only\n");
    }
    return report(protection_request_input(strcmp(argv[1], "lfe") == 0 ? INPUT_LFE : INPUT_PREAMP));
}

static int cmd_build(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "low") && strcmp(argv[1], "high"))) {
        printf("usage: build low|high   (low = VINL1/VINR1 + INPUT_SEL, high = VINL3/VINR3)\n");
        return 1;
    }
    return report(protection_request_build(strcmp(argv[1], "high") == 0 ? BUILD_HIGH_LEVEL : BUILD_LOW_LEVEL));
}

static int cmd_pga(int argc, char **argv)
{
    float v;
    if (argc != 2 || !parse_float(argv[1], &v)) {
        printf("usage: pga <dB>   (-12.0 .. +40.0 in 0.5 dB steps)\n");
        return 1;
    }
    const long half = lroundf(v * 2.0f);
    if (half < SETTINGS_PGA_HALF_MIN || half > SETTINGS_PGA_HALF_MAX || fabsf(v * 2.0f - half) > 1e-3f) {
        printf("error: PGA must be -12.0 .. +40.0 dB in 0.5 dB steps\n");
        return 1;
    }
    return report(protection_request_pga((int)half));
}

static int cmd_gain(int argc, char **argv)
{
    float v;
    if (argc != 2 || !parse_float(argv[1], &v) || v < SETTINGS_GAIN_DB_MIN || v > SETTINGS_GAIN_DB_MAX) {
        printf("usage: gain <dB>   (%.0f .. %+.0f, digital gain before the limiter)\n",
               SETTINGS_GAIN_DB_MIN, SETTINGS_GAIN_DB_MAX);
        return 1;
    }
    return report(protection_request_gain(v));
}

static int cmd_limit(int argc, char **argv)
{
    float v;
    if (argc != 2 || !parse_float(argv[1], &v) || v < SETTINGS_LIMIT_DB_MIN || v > SETTINGS_LIMIT_DB_MAX) {
        printf("usage: limit <dBFS>   (%.0f .. %.0f, output limiter ceiling)\n",
               SETTINGS_LIMIT_DB_MIN, SETTINGS_LIMIT_DB_MAX);
        return 1;
    }
    return report(protection_request_limit(v));
}

static int cmd_save(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (protection_is_armed()) {
        printf("armed: settings will be written at the next disarm or mode change\n");
    }
    return report(protection_request_save());
}

static void print_mon_line(void)
{
    prot_status_t p;
    audio_stats_t a;
    protection_get_status(&p);
    audio_get_stats(&a);
    printf("%9.2f  %-12s FN=%d K1=%s AR=%d XSMT=%d TRIG=%d VBUS=%d  in %6.1f/%6.1f out %6.1f GR %4.1f  kicks=%" PRIu32 " %s\n",
           esp_timer_get_time() / 1e6, protection_state_name(p.state), p.fault_n_ok,
           p.relay_closed ? "closed" : "open  ", g_board_out.audio_ready, g_board_out.xsmt,
           board_trigger_present(), board_vbus_present(), db(a.in_peak_l), db(a.in_peak_r), db(a.out_peak),
           -db(a.limiter_min_gain), a.kicks, a.healthy ? "healthy" : "UNHEALTHY");
}

static int cmd_levels(int argc, char **argv)
{
    long n = 1;
    if (argc == 2 && (!parse_int(argv[1], &n) || n < 1 || n > 1000)) {
        printf("usage: levels [count]   (one line every 250 ms)\n");
        return 1;
    }
    printf("  time s   state        pins                                  peak dBFS in L/R, out, limiter GR dB\n");
    for (long i = 0; i < n; i++) {
        print_mon_line();
        if (i + 1 < n) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    return 0;
}

static int cmd_faults(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        protection_clear_faults();
        printf("fault log cleared\n");
        return 0;
    }
    uint32_t counts[FAULT_COUNT];
    protection_get_fault_counts(counts);
    printf("Counts:");
    bool any = false;
    for (int c = 1; c < FAULT_COUNT; c++) {
        if (counts[c]) {
            printf("\n  %5" PRIu32 "  %s", counts[c], protection_fault_name((fault_cause_t)c));
            any = true;
        }
    }
    printf("%s\n", any ? "" : " none");

    static fault_entry_t entries[16];
    const int n = protection_get_faults(entries, 16);
    for (int i = 0; i < n; i++) {
        const fault_entry_t *e = &entries[i];
        printf("#%-3" PRIu32 " t=%9.3f s", e->seq, e->t_ms / 1000.0);
        if (e->count > 1) {
            printf(" x%" PRIu32, e->count);
        }
        printf("  %s, detail 0x%08" PRIx32 ", in %s\n", protection_fault_name(e->cause), e->detail,
               protection_state_name(e->state));
        if (e->cause == FAULT_AUDIO_UNHEALTHY || e->cause == FAULT_VERIFY_TIMEOUT) {
            printf("      audio: ");
            print_health_flags(e->detail & 0xFF);
            printf("\n");
        }
        if (e->cause == FAULT_N_ASSERTED || e->cause == FAULT_N_DISARMED) {
            if (e->fault_n_low_ms >= 0) {
                printf("      FAULT_N low %" PRId32 " ms%s", e->fault_n_low_ms, classify_fault_n(e->fault_n_low_ms));
            } else {
                printf("      FAULT_N still low or duration unknown");
            }
            if (e->since_kick_ms >= 0) {
                printf(", last WDI kick %" PRId32 " ms before the fall%s", e->since_kick_ms,
                       e->since_kick_ms > 160 ? " (watchdog starved)" : "");
            }
            printf("\n");
        }
    }
    return 0;
}

static int cmd_wdt(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: wdt starve|fast|normal   (current: %s)\n", wdt_test_name(audio_get_wdt_test()));
        return 1;
    }
    if (strcmp(argv[1], "starve") == 0) {
        audio_set_wdt_test(AUDIO_WDT_STARVE);
        printf("WDI kicks stopped. Expect FAULT_N low within 165.8 ms of the last kick, K1 open and staying open.\n"
               "Automatic re-arm is blocked until `wdt normal`. Check with `faults`.\n");
    } else if (strcmp(argv[1], "fast") == 0) {
        audio_set_wdt_test(AUDIO_WDT_FAST);
        printf("WDI toggling every %d blocks (~%.1f ms), inside the 10.35 ms lower window.\n"
               "Expect a window fault: FAULT_N low, K1 open. Re-arm blocked until `wdt normal`.\n",
               2, 2.0 * AUDIO_BLOCK_FRAMES / 48.0);
    } else if (strcmp(argv[1], "normal") == 0) {
        audio_set_wdt_test(AUDIO_WDT_NORMAL);
        printf("normal kicking (50 ms while audio is healthy)\n");
    } else {
        printf("usage: wdt starve|fast|normal\n");
        return 1;
    }
    return 0;
}

static const uint8_t s_adc_dump_regs[] = {
    0x01, 0x02, 0x05, 0x06, 0x07, 0x0B, 0x20, 0x28, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x78,
};

static int cmd_adc(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
        for (size_t i = 0; i < sizeof(s_adc_dump_regs); i++) {
            uint8_t v = 0;
            const esp_err_t err = pcm1863_read(s_adc_dump_regs[i], &v);
            if (err == ESP_OK) {
                printf("  0x%02x = 0x%02x\n", s_adc_dump_regs[i], v);
            } else {
                printf("  0x%02x : %s\n", s_adc_dump_regs[i], esp_err_to_name(err));
            }
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        pcm1863_status_t st;
        const esp_err_t err = pcm1863_read_status(&st);
        if (err != ESP_OK) {
            return report(err);
        }
        static const char *const fs_names[] = {"halt/low", "8k", "16k", "32-48k", "88.2-96k", "176.4-192k", "high", "invalid"};
        static const char *const bck_names[] = {"halt/low", "32", "48", "64", "256", "-", "high", "invalid"};
        printf("  0x72 state   %s (0x%02x)\n", pcm1863_state_name(st.state), st.state);
        printf("  0x73 fs      %s\n", fs_names[st.fsample & 7]);
        printf("  0x74 BCK/LRCK %s, SCK ratio code %d\n", bck_names[(st.div >> 4) & 7], st.div & 7);
        printf("  0x75 clocks  LRCKHLT=%d BCKHLT=%d SCKHLT=%d LRCKERR=%d BCKERR=%d SCKERR=%d\n",
               (st.clk >> 6) & 1, (st.clk >> 5) & 1, (st.clk >> 4) & 1, (st.clk >> 2) & 1, (st.clk >> 1) & 1,
               st.clk & 1);
        printf("  0x78 supply  DVDD=%d AVDD=%d LDO=%d\n", (st.supply >> 2) & 1, (st.supply >> 1) & 1,
               st.supply & 1);
        printf("  0x28 PLL     LOCK=%d\n", (st.pll >> 4) & 1);
        printf("  => %s\n", pcm1863_status_ok(&st) ? "OK (run, 48 kHz, 64 fs, supplies good)" : "NOT OK");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "read") == 0) {
        long reg;
        if (!parse_int(argv[2], &reg) || reg < 0 || reg > 0x7F) {
            printf("usage: adc read <reg 0x00..0x7f>\n");
            return 1;
        }
        uint8_t v = 0;
        const esp_err_t err = pcm1863_read((uint8_t)reg, &v);
        if (err == ESP_OK) {
            printf("0x%02lx = 0x%02x\n", reg, v);
        }
        return err == ESP_OK ? 0 : report(err);
    }
    if (argc == 4 && strcmp(argv[1], "write") == 0) {
        long reg, val;
        if (!parse_int(argv[2], &reg) || !parse_int(argv[3], &val) || reg < 0 || reg > 0x7F || val < 0 ||
            val > 0xFF) {
            printf("usage: adc write <reg> <val>\n");
            return 1;
        }
        if (protection_is_armed()) {
            printf("refused: disarm first (raw writes bypass the mode-change sequence)\n");
            return 1;
        }
        printf("warning: raw write, not stored; `adc reconfig` restores the configuration\n");
        return report(pcm1863_write((uint8_t)reg, (uint8_t)val));
    }
    if (argc == 2 && strcmp(argv[1], "reconfig") == 0) {
        return report(protection_request_adc_reconfig());
    }
    printf("usage: adc dump | status | read <reg> | write <reg> <val> | reconfig\n");
    return 1;
}

static int cmd_i2c(int argc, char **argv)
{
    long bus = 0;
    if (argc < 2 || strcmp(argv[1], "scan") != 0 || (argc == 3 && (!parse_int(argv[2], &bus) || bus < 0 || bus > 1))) {
        printf("usage: i2c scan [0|1]   (0 = PCM1863 bus, 1 = front panel J6)\n");
        return 1;
    }
    i2c_master_bus_handle_t h = bus == 0 ? pcm1863_bus() : s_ui_bus;
    if (!h) {
        printf("I2C%ld not available\n", bus);
        return 1;
    }
    int found = 0;
    for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(h, addr, 10) == ESP_OK) {
            printf("  0x%02x%s\n", addr, (bus == 0 && addr == PCM1863_I2C_ADDR) ? "  PCM1863" : "");
            found++;
        }
    }
    printf("%d device(s) on I2C%ld\n", found, bus);
    return 0;
}

static int cmd_gen(int argc, char **argv)
{
    float hz = 0, dbfs = 0;
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        audio_set_generator(DSP_GEN_OFF, 0, 0);
        printf("generator off, ADC input restored\n");
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "sine") == 0 && parse_float(argv[2], &hz) && parse_float(argv[3], &dbfs) &&
        hz >= 5.0f && hz <= 20000.0f && dbfs <= 0.0f && dbfs >= -120.0f) {
        audio_set_generator(DSP_GEN_SINE, hz, dsp_db_to_lin(dbfs));
        printf("sine %.1f Hz at %.1f dBFS replaces the ADC input (still through HPF, gain and limiter)\n",
               (double)hz, (double)dbfs);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "pulse") == 0 && parse_float(argv[2], &dbfs) && dbfs <= 0.0f &&
        dbfs >= -120.0f) {
        audio_set_generator(DSP_GEN_PULSE, 0, dsp_db_to_lin(dbfs));
        printf("2 ms pulse every 500 ms at %.1f dBFS replaces the ADC input\n", (double)dbfs);
        return 0;
    }
    printf("usage: gen sine <Hz> <dBFS> | gen pulse <dBFS> | gen off\n");
    return 1;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    board_mute_disarm();
    printf("rebooting (relay opened)\n");
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
    return 0;
}

static void reg(const char *name, const char *help, const char *hint, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t cmd = {
        .command = name,
        .help = help,
        .hint = hint,
        .func = fn,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

esp_err_t console_start(i2c_master_bus_handle_t ui_bus)
{
    s_ui_bus = ui_bus;

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "p68> ";
    repl_cfg.task_core_id = 0;
    repl_cfg.max_cmdline_length = 128;
    const esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl), TAG, "repl");

    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "help");
    reg("status", "State machine, pins, watchdog, audio health, ADC status, settings", NULL, cmd_status);
    reg("arm", "Clear HOLD/LOCKOUT and arm when all conditions are met", NULL, cmd_arm);
    reg("disarm", "Ramp down, open K1 and stay disarmed (HOLD)", NULL, cmd_disarm);
    reg("input", "Select input (LOW-LEVEL build): preamp stereo or Marantz LFE", "preamp|lfe", cmd_input);
    reg("build", "Select board build: low (VIN1 + INPUT_SEL) or high (VIN3)", "low|high", cmd_build);
    reg("pga", "PCM1863 PGA gain in dB (-12 .. +40, 0.5 dB steps)", "<dB>", cmd_pga);
    reg("gain", "Digital gain before the limiter, dB", "<dB>", cmd_gain);
    reg("limit", "Output limiter ceiling, dBFS", "<dBFS>", cmd_limit);
    reg("save", "Write settings to NVS (deferred while armed)", NULL, cmd_save);
    reg("levels", "Peak levels and pin states, every 250 ms", "[count]", cmd_levels);
    reg("faults", "Fault counters and log, newest first", "[clear]", cmd_faults);
    reg("wdt", "Watchdog tests: stop kicking, kick too fast, or normal", "starve|fast|normal", cmd_wdt);
    reg("adc", "PCM1863 registers", "dump|status|read <reg>|write <reg> <val>|reconfig", cmd_adc);
    reg("i2c", "Scan an I2C bus", "scan [0|1]", cmd_i2c);
    reg("gen", "Test signal in place of the ADC input", "sine <Hz> <dBFS>|pulse <dBFS>|off", cmd_gen);
    reg("reboot", "Open the relay and restart", NULL, cmd_reboot);

    return esp_console_start_repl(repl);
}
