# Phase 1 bench bring-up

Firmware for the first power-up of the P68 Subwoofer DSP Controller. It is meant for
measurement, not for listening. Each step below maps a DESIGN_REVIEW §8 test to console
commands and states what you should see.

The hardware contract is in [hardware/FIRMWARE_BRIEF.md](hardware/FIRMWARE_BRIEF.md) and
[hardware/DESIGN_REVIEW.md](hardware/DESIGN_REVIEW.md). Both are snapshots taken
2026-10-23.

## Build, flash, console

```bash
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

- **Connection:** the console is on the native USB port (J3, USB-Serial-JTAG) at prompt
  `p68>`. Type `help` for the command list.
- **Ground loop:** run the Mac on battery while USB is connected (brief §2).
- **Host DSP tests:** run `make -C test/host`. These need no hardware.

## What a good boot looks like

```
I (..) board: safe GPIO state: XSMT=0 AUDIO_READY=0 INPUT_SEL=0 WDI=0, IO9 high-Z
I (..) protect: BOOT -> ADC_CONFIG
I (..) pcm1863: configured: BCK-PLL slave, I2S 32-bit, short-latency filter, input VIN1, PGA +0.0 dB
I (..) audio: I2S0 full duplex 48 kHz/64 fs, block 32 frames x 3 DMA buffers, HPF 15.0 Hz (SVF)
I (..) protect: ADC_CONFIG -> AUDIO_VERIFY
I (..) protect: audio verified, WDI kicking enabled
I (..) protect: AUDIO_VERIFY -> WAIT_CLEAR
I (..) protect: armed: K1 closed 3xxx us after AUDIO_READY edge
I (..) protect: WAIT_CLEAR -> ARMED_WAIT -> RAMP_UP -> RUNNING
```

- **Status LED:** steady when RUNNING, 1 Hz blink while waiting or disarmed, 4 Hz blink
  when FAULTED or in LOCKOUT.
- **If it stops in a state:** run `status`. Its `waiting for:` line names the
  condition that is blocking.

## Arming sequence (brief §5)

1. **Safe outputs:** at boot, XSMT, AUDIO_READY, INPUT_SEL and WDI are driven low. IO9 is
   left as a high-Z input.
2. **ADC config:** the PCM1863 is configured and every written register is read back.
   A failure retries with backoff (1, 2, 4, 5, 5 … s) and is logged as
   `PCM1863 configuration failed`.
3. **I²S start:** I²S0 starts in full duplex with the output gain at 0.
4. **Verify:** WDI kicking starts only after audio has been healthy for 500 ms and the
   PCM1863 status is good (state run, 32–48 kHz, BCK = 64 fs, supplies good).
5. **FAULT_N stable:** FAULT_N must stay high for 500 ms. The other arming conditions
   must also hold: K1 sense reads open, the output pads match their commands, and
   200 ms has passed since any input change.
6. **Arm:** AUDIO_READY makes a rising edge, and IO3 must read low within 10 ms
   (2 consecutive reads 100 µs apart). The measured close time appears in `status`.
7. **Unmute:** 20 ms later XSMT goes high and the gain ramps up over 200 ms.

## What "healthy audio" means

Each 50 ms of audio is judged as one period. A period counts as healthy only if all of
these hold:

- **Timing:** 75 blocks (N = 32) arrived, and the period took 40–60 ms.
- **Gaps:** no block arrived more than 5 ms after the previous one.
- **DMA queues:** no RX overflow and no TX underrun.
- **I/O:** no I²S read or write error.
- **ADC data:** the words are not bit-identical on both channels for 100 ms or more.
- **DSP state:** the filter state is finite.

Each healthy period toggles WDI once, so WDI changes level every 50 ms. A bad period
skips its toggle.

- **While armed:** any bad period disarms immediately, with the fault
  `audio unhealthy while armed`.
- **Two bad periods in a row:** the TPS3850 window (165.8 ms) runs out and the
  hardware opens K1 by itself.

## §8 commissioning tests

### 1. Bare-board power

This test needs no firmware.

### 2. Supervisors

- **Monitor:** run `levels 400` to print 100 s of lines. The `FN=` column is FAULT_N.
  Sweep each rail down.
- **Read the log:** afterwards, `faults` lists each FAULT_N event with how long it stayed
  low.
  - **Rail supervisor trips:** these read as "sustained".
  - **Watchdog pulses:** these read as "~200 ms: TPS3850 WDO pulse".
- **+3V3_ESP:** this rail resets the ESP32 through U7 RESET → EN. After the reset, the
  boot log shows `reset reason: power-on / EN`.
- **Boot pulses:** the TPS3850 pulses FAULT_N until the first WDI kick. These pulses are
  counted as `boot FAULT_N pulses` in `status` and are not logged as faults.

### 3. Latch logic

| Step | Do | Expect |
|---|---|---|
| a | Boot normally | `status`: RUNNING, `IO3=0 (K1 closed)`, `last K1 close` a few ms. TP20 is high. |
| b | `wdt starve` | WDI stops. FAULT_N falls within 165.8 ms of the last edge, then TP20 falls and K1 opens and stays open. To check the ≤224 ms spec, scope from the last WDI edge to the TP20 fall. `faults` shows `FAULT_N low ~200 ms (TPS3850 WDO pulse), last WDI kick NNN ms before the fall (watchdog starved)`. |
| b' | `wdt normal` | Kicking resumes. After FAULT_N has been stable for 1 s, the board re-arms automatically. |
| c | `wdt fast`, then `wdt normal` | WDI toggles every 1.3 ms, so its falling edges are 2.7 ms apart, inside the lower window. Expect a window fault: FAULT_N low and K1 open. |
| d | Arm, short TP21 (AUDIO_READY) to 3V3, then `wdt starve` | K1 opens and stays open, because there is no new rising edge. Firmware sees AUDIO_READY commanded low but the pad reads high, and logs `output pad differs from command`. It then refuses to arm (`waiting for: output pad mismatch`). |

### 4. Power-down thump

Arm the board, play a source (or `gen sine 40 -20`), then pull the +15 V plug. This is a
hardware-only check.

### 5. Input switching (LOW-LEVEL build)

- **Switch:** run `input lfe` and `input preamp`. The log should show
  `RUNNING -> RAMP_DOWN -> RELEASE`, then `mode: ...`, then the re-arm about 200 ms
  later.
- **Pops:** scope the P68 input while switching. There should be no pop.
- **Holding a mode:** to measure ground isolation, `disarm` keeps the input relays in
  their current mode while K1 stays open. `arm` resumes.

### 6. High-level input

- **Select the build:** run `build high`. The setting is stored in NVS, and the input
  changes to VINL3/VINR3.
- **Expected level:** drive 20 Vrms differential at 50 Hz and run `levels 20`. Input
  peaks should read about −1.3 dBFS (1.8 Vrms against 2.1 Vrms full scale).
- **Limiter:** the output is limited to the −6 dBFS default ceiling. Raise it with
  `limit -1` if needed.

### 7. ADC and DAC performance

- **BCK-PLL lock:** run `adc status`. It decodes registers 0x72–0x75, 0x78 and the PLL
  lock bit in 0x28.
- **DAC alone:** run `gen sine 1000 -1`, `limit -1` and `gain 0`.
- **ADC to DAC:** run `gen off` for passthrough.
- **HPF:** the 15 Hz high-pass is always in the path, so 50 Hz is about 0.1 dB down.
- **Clipping:** the input clips at about 3.1 Vrms on the LOW build, where input peaks
  approach 0 dBFS.

### 8. Latency

- **Measure:** put a pulse into the preamp input and scope the P68 input.
- **Estimate:** with the defaults (N = 32, 3 DMA buffers), expect about
  (10 + 3.5) + 32 + 3 × 32 ≈ 142 samples ≈ **3.0 ms**.
- **Other settings:** change them with `idf.py menuconfig` → *P68 Subwoofer DSP*. The
  block size can be 16 frames, and the DMA buffer count can be 2 (the DESIGN_REVIEW
  estimate).
- **DAC side only:** `gen pulse -6` sends a 2 ms pulse every 500 ms, which isolates the
  DAC-side latency.

### 9. Thermal

- **Load everything:** run `input lfe` (K2/K3 on) with the board armed (K1 on).
- **CPU:** the `DSP` line in `status` shows the core 1 load.
- **Clock:** the CPU is already fixed at 240 MHz.

### 10. Trigger input

Watch the `TRIG=` column in `levels 100` while applying 0, 5, 12 and 15 V and reverse
polarity. Phase 1 only reports the trigger; it doesn't act on it.

## Console reference

| Command | Purpose |
|---|---|
| `status` | Everything in one screen: state, pins (commanded vs pad), arming, watchdog, audio health, DSP load, levels, ADC status, settings |
| `arm` / `disarm` | Leave HOLD, LOCKOUT or FAULTED and arm when the conditions are met / ramp down, open K1 and hold |
| `input preamp\|lfe` | Input relays (LOW-LEVEL build), using the brief §5 mode-change sequence |
| `build low\|high` | Board build (ADC input pins); stored |
| `pga <dB>` | PCM1863 PGA, −12 to +40 dB in 0.5 dB steps |
| `gain <dB>` | Digital gain before the limiter (−60 to +12 dB) |
| `limit <dBFS>` | Limiter ceiling (−40 to −1 dBFS). The DAC full scale is about 1.42 Vrms; the P68 reaches full power at about −0.8 dBFS |
| `levels [n]` | n lines at 4 Hz: state, FAULT_N, K1, AUDIO_READY, XSMT, trigger, VBUS, input and output peaks, limiter gain reduction |
| `faults [clear]` | Fault counters and the last 16 events, with FAULT_N duration and time since the last kick |
| `wdt starve\|fast\|normal` | Watchdog fault injection; automatic re-arm is blocked until `normal` |
| `adc dump\|status\|read\|write\|reconfig` | PCM1863 registers (raw writes are refused while armed) |
| `i2c scan [0\|1]` | Bus 0 = PCM1863 (expect 0x4A), bus 1 = front panel |
| `gen sine <Hz> <dBFS>\|pulse <dBFS>\|off` | Test signal in place of the ADC (still through the HPF, gain and limiter) |
| `save`, `reboot` | Settings are written to NVS automatically, but only while K1 is open |

## Assumptions to confirm on the bench (TODO)

1. **Pins during reset and boot.** XSMT, AUDIO_READY, WDI and INPUT_SEL should stay low
   from power-on through the ROM bootloader until `board_io_init_safe()` runs. They have
   external pull-downs, but check on the scope that no ESP32-S3 default internal pull-up
   fights them.
2. **PCM1863 status values.** `pcm1863_status_ok()` expects:
   - STATE = 0xF (run)
   - fs code 3 (32–48 kHz)
   - BCK ratio code 3 (64 fs)
   - no LRCK/BCK halt or error bits (SCKHLT = 1 is expected, because SCKI is static)
   - supplies = 0x07

   If the real board reports something different but works, `adc status` shows the raw
   values. Disable `CONFIG_P68_REQUIRE_ADC_STATUS` only after you have checked them.
3. **ADC stuck detector.** With the inputs shorted, the ADC noise must still change the
   24-bit words at least once every 100 ms. Check that `status` never reports
   `adc-stuck` with a quiet input.
4. **TPS3850 WDI edge.** The firmware toggles WDI every 50 ms. That satisfies the window
   whether the part counts every edge (50 ms) or only falling edges (100 ms).
5. **FAULT_N duration labels.** The thresholds are <150 ms "glitch", 150–260 ms "WDO
   pulse", and >260 ms "sustained". They come from the 200 ms CRST-open pulse in the
   design review, so adjust them if the scope disagrees.
6. **K1 release time.** The firmware allows 50 ms from AUDIO_READY low to IO3 high
   (DESIGN_REVIEW §4.8: not yet measured).
7. **First kick.** The first WDI toggle can land inside the TPS3850 lower window, which
   gives one ~200 ms WDO pulse after kicking starts. If it happens, it is logged as
   `FAULT_N low while disarmed`. It is harmless because the firmware waits for FAULT_N to
   be stable before arming.

## DSP note: the subsonic high-pass doesn't use the ESP-DSP biquad

The brief asks for ESP-DSP biquads. In float32, ESP-DSP's `dsps_biquad_f32` is direct
form II. With poles this close to z = 1 (fc/fs = 15/48000), its internal states grow by
about (fs / 2πfc)², and the output loses most of its precision.

The host test (`make -C test/host`) measures each structure against a double-precision
reference, using a 20 Hz tone at −6 dBFS through the 15 Hz high-pass:

| Structure | Error relative to signal |
|---|---|
| ESP-DSP DF2 biquad | **−39 dB** |
| TPT state-variable filter, same transfer function | **−96 dB** |

The audio path therefore uses the SVF.

- **Bench comparison:** `CONFIG_P68_HPF_USE_ESP_DSP_BIQUAD` switches back to ESP-DSP so
  you can compare the two on the analyser.
- **Phase 2:** the same problem affects every low-frequency biquad in Phase 2 (LR4/LR8
  around 80 Hz measured about −68 dB with DF2). The crossover, PEQ and all-pass sections
  should use the SVF (or another low-noise structure) as well. This needs your decision
  before Phase 2.
