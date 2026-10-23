# ESP32SubProcessor

This is the firmware for the **P68 Subwoofer DSP Controller**. The signal path is:

PCM1863 ADC → ESP32-S3-WROOM-1U-N8 → PCM5102A DAC → protected output relay → ESP Project 68 power amplifier.

The KiCad project lives in `~/Documents/GitHub/Kicadprojects/SubProcessorDigital`. `docs/hardware/` holds snapshots of its
firmware brief and design review.

**Status:** Phase 1 (bring-up). It builds cleanly with ESP-IDF v5.5.6 and has **not yet been run on hardware**.
See [docs/BRINGUP.md](docs/BRINGUP.md) for the bench procedure.

## Phase 1 features

- **Safe GPIO init:** XSMT, AUDIO_READY, INPUT_SEL and WDI are low before anything else runs, and IO9 stays high-Z.
- **Protection and arming state machine:** follows brief §5.
  - AUDIO_READY rising edge, IO3 checked within 10 ms, then XSMT 20 ms later and a ramp up.
  - Mode changes follow the brief's sequence.
  - The FAULT_N interrupt mutes immediately.
  - Faults are logged and the board re-arms automatically, with a rate limit.
- **TPS3850 window watchdog:** WDI is toggled every 50 ms of audio, only from the audio task, and only while audio is healthy.
- **PCM1863 over I²C:** BCK-PLL slave, SCK-only clocking, short-latency filter, and VIN1 or VIN3 by build. Every write is read back to check it.
- **I²S0 full duplex:** 48 kHz, 64 fs. Audio is summed to mono, passed through a 15 Hz 2nd-order high-pass and a peak limiter, and sent to both DAC slots.
- **USB console:** commands for every DESIGN_REVIEW §8 test, including status, arm/disarm, input select, levels, faults and watchdog fault injection.

## Layout

```
main/
  board.h        pin map (the only place GPIO numbers appear)
  board_io.*     safe init, register-level ISR-safe pin access
  protection.*   arming / fault state machine, fault log (core 0)
  audio.*        I2S, DSP chain, health judgement, WDI kicks (core 1)
  pcm1863.*      ADC driver (SLAS831D register values)
  settings.*     NVS settings
  console.*      USB-Serial-JTAG commands
components/dsp_core/   host-testable DSP: SVF/biquad, mono sum, limiter, ramp, generator
test/host/             host unit tests for dsp_core
docs/                  BRINGUP.md, hardware/ snapshots
```

## Build

```bash
. ~/esp/esp-idf/export.sh
idf.py build
```
```bash
make -C test/host
```

Phases 2 (DSP chain) and 3 (UI) wait on decisions listed in `docs/hardware/FIRMWARE_BRIEF.md` §6–7.
