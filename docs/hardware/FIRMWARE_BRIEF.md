# Firmware brief — P68 Subwoofer DSP Controller (ESP32-S3)

Paste the prompt in section 1 into a new Claude Code session opened in the firmware repo
`~/Documents/GitHub/ESP32SubProcessor` (github.com/smileym4n-collab/ESP32SubProcessor).
The hardware lives in `~/Documents/GitHub/Kicadprojects/SubProcessorDigital`. Sections 2–7 are the hardware facts it needs.
They were taken from the schematic netlist on 2026-10-23, after the PCB layout was finished.

---

## 1. Prompt

> I need ESP32-S3 firmware for my P68 Subwoofer DSP Controller board. All firmware work happens in this repo (`ESP32SubProcessor`).
> The hardware is in `~/Documents/GitHub/Kicadprojects/SubProcessorDigital`. Read its `FIRMWARE_BRIEF.md` (hardware/firmware contract) and `DESIGN_REVIEW.md` (sections 3 and 8–10 especially) before writing any code. Copy both into `docs/hardware/` in this repo as a snapshot, and never edit the KiCad repo.
> My earlier ESP-IDF I²S project `~/Documents/GitHub/ESP32S3I2S` can serve as a reference for code style and IDF setup, but this board's pins and converters are different.
>
> Create the firmware at the root of this repo as an **ESP-IDF v5.x** project in C (target `esp32s3`), using the **ESP-DSP** component for biquads.
> Do not use Arduino.
>
> Work in phases and stop for my review at the end of each phase:
> 1. **Bring-up firmware (start here):**
>    - Safe GPIO init and the protection/arming state machine with the external TPS3850 watchdog.
>    - PCM1863 configuration over I²C with register readback.
>    - I²S full-duplex passthrough (ADC → mono sum → DAC) with a fixed high-pass and limiter.
>    - A USB serial console with commands to exercise every test in DESIGN_REVIEW §8: status, arm/disarm, input select, levels, fault readout and watchdog-starve test.
> 2. **DSP chain:** crossover, PEQ, all-pass, delay, volume and limiter, with coefficients computed on the device and parameters stored in NVS.
> 3. **UI:** encoders, I²C display and menus, presets, and 12 V trigger behaviour.
>
> Rules:
> - The board is **not bench-tested yet**, so keep everything measurable and fail-safe.
> - Never assume hardware behaviour beyond what the brief states; where it is uncertain, write a TODO and tell me.
> - Keep the audio path real-time: pinned high-priority task, internal RAM buffers, no blocking calls or logging inside it.
> - Put DSP maths in a host-testable module and add host unit tests (filter response, limiter, sum).
> - Build with `idf.py build` and fix all warnings. I will flash and test on the bench myself.
>
> Before writing code, give me a short plan of the project structure and the state machine for my approval.

---

## 2. Target and tools

- **Module:** ESP32-S3-WROOM-1U-**N8**: 8 MB flash, **no PSRAM**, external antenna on U.FL, which is unused. **Wi-Fi and Bluetooth must stay disabled.**
- **Programming and console:** native USB (USB-Serial-JTAG) on IO19/IO20 through J3 (USB-B). UART0 (TXD0/RXD0) is also on J11 (1 = TX, 2 = RX, 3 = GND, 4 = +3V3_ESP); don't power the board from J11.
  - **USB ground loop:** USB is for configuration only, with the Mac on battery. Audio performance while USB is connected doesn't matter.
- **Buttons:** EN (IC4) and BOOT (IO0, IC5).

## 3. GPIO map (from the netlist)

| GPIO | Net | Dir | Notes |
|---|---|---|---|
| IO1 | DAC_XSMT | out | PCM5102A soft mute: low = muted. 100 k pull-down. |
| IO2 | WATCHDOG_HEARTBEAT | out | TPS3850 WDI (U7). 100 k pull-down. See §5. |
| IO3 | OUT_RLY_SENSE | in | K1 second pole: **low when the output relay K1 is closed** (NO contact to GND, R41 pull-up). Strap pin (JTAG source), so use it as input only. |
| IO4 / IO5 / IO6 | ENC1 A / B / SW | in | Active low, RC-filtered (10 k pull-up, 10 k series, 10 nF). |
| IO7 | I2S_DIN | in | Data from PCM1863. |
| IO8 | ENC2_SW | in | Active low. |
| IO9 | ADC_SCKI | **keep as input (high-Z)** | Wired straight to PCM1863 SCKI, with R33 100 k to GND. SCKI must stay static so the ADC uses its BCK-PLL. **Do not drive IO9** unless outputting a correct 256fs MCLK, which isn't planned. |
| IO10 | I2S_BCK | out | 3.072 MHz (64 fs), 22 R series. Feeds the ADC and DAC. |
| IO11 | I2S_LRCK | out | 48 kHz, 22 R series. |
| IO12 / IO13 | I2C0 SDA / SCL | od | PCM1863 control only (on-board). |
| IO14 / IO21 | I2C1 SDA / SCL (UI) | od | Front panel / display via J6. Separate bus so a ribbon fault can't lock the ADC bus. |
| IO15 / IO18 | ENC2 A / B | in | Active low. |
| IO16 | AUDIO_READY | out | Arms output relay K1 on a **rising edge** (see §5). R53 pull-down. |
| IO17 | USB_VBUS_SENSE | in | High when USB is plugged in (divider from VBUS). |
| IO38 | I2S_DOUT | out | Data to PCM5102A, 22 R series. |
| IO39 / IO40 / IO41 | ENC3 A / B / SW | in | Active low. |
| IO42 | FAULT_N | in | Open-drain wired-OR of all supervisors and the watchdog. Low = fault. |
| IO45 | INPUT_SEL | out | **Strap: must be low at reset** (R31 pull-down); drive it only after boot. Low = relays K2/K3 de-energised = PREAMP stereo. High = MARANTZ LFE on both ADC channels. LOW-LEVEL build only. |
| IO46 | STATUS_LED | out | Local LED plus front-panel LED (J6 pin 19). Strap pin: don't drive it high during download mode. |
| IO47 | MODE_TRIG_N | in | Isolated 12 V trigger (VO617A opto): **low = trigger present**. |
| IO48 | ADC_INT | in | PCM1863 GPIO3/INTC, optional. |
| IO35–37 | — | — | Unused (PSRAM pins on R8 modules). |

## 4. Audio converters

- **Clocking:** the ESP32 is the **I²S0 full-duplex master**: 48 kHz, BCK = 64 fs = 3.072 MHz, standard I²S (Philips) format, 32-bit slots carrying 24-bit data. The ADC and DAC are synchronous by construction.
- **PCM1863 (IC2), TI SLAS831D:**
  - **Control:** I²C on IO12/IO13. MS/AD and MD0 are tied to GND, so the expected address is **0x4A**; confirm it with an I²C scan and the datasheet.
  - **Clock:** I²S slave in **BCK-PLL mode**. XI is tied to GND, so set SCK_XI_SEL to "SCK only" and keep SCKI static.
  - **Filter:** use the low-latency IIR decimation filter.
  - **Input routing depends on a build configuration stored in NVS:**
    - **LOW-LEVEL build:** ADC inputs VINL1/VINR1, single-ended. INPUT_SEL picks preamp stereo or Marantz LFE (the same signal on both channels).
    - **HIGH-LEVEL build** (speaker-level input through an RC4580 difference amp): ADC inputs **VINL3/VINR3**, single-ended. INPUT_SEL is unused.
  - **PGA:** gain available for low-level sources; default 0 dB.
- **PCM5102A (U2):** hardware-configured with FMT = I²S, FLT = low-latency, DEMP off, SCK = GND (internal PLL). No control bus; mute is XSMT (IO1) only.
- **Output level:** the DAC's 2.1 Vrms full scale is trimmed by R38/R39 to suit the P68. Firmware still needs a limiter.

## 5. Protection and arming: must be implemented exactly

**Hardware:**
- **TPS3850H01 (U7):** a supervisor with a **window** watchdog on WDI (IO2).
  - The window is **10.35 ms (max lower) to 165.8 ms (min upper)**. Kick (toggle) IO2 every **~50 ms**, never faster than ~15 ms and never slower than ~120 ms.
  - Kick **only from the audio processing task, and only while audio is healthy**: DMA blocks arriving on time and sane ADC data.
  - Too fast or too slow → WDO pulses for 200 ms → FAULT_N low → fault latch cleared → K1 opens.
- **TPS3808 ×4** monitor +15 V (<11.3 V), +3V3_DAC, +3V3_A_ADC and +3V3_D_ADC (3.07 V). Their outputs are wired onto FAULT_N.
- **Fault latch U13 (SN74AUP1G74):** CLR = FAULT_N, D = 1, CLK = AUDIO_READY.
  - Q goes high only on an **AUDIO_READY rising edge while FAULT_N is high**.
  - K1 closes only when **latch Q AND AUDIO_READY are both high** (two transistors in series).
  - **After any fault the relay stays open until firmware drops AUDIO_READY low and raises it again.**
- **Supervisor reset:** U7 RESET drives ESP32 EN (3.3 V rail undervoltage).

**Start-up sequence:**
1. Boot with XSMT low, AUDIO_READY low and INPUT_SEL low.
2. Configure the PCM1863 and read the registers back.
3. Start I²S full duplex.
4. Verify that DMA timing is stable and the ADC data is sane, then start kicking WDI.
5. Check that FAULT_N (IO42) is high.
6. Raise AUDIO_READY (a rising edge), then confirm IO3 goes **low within 10 ms**. If it doesn't, disarm and report the fault.
7. After 20 ms, set XSMT high and ramp the volume up.

**Mode or input change:**
ramp down → XSMT low → AUDIO_READY low (K1 opens) → switch INPUT_SEL (K2/K3) or reconfigure the ADC → wait ≥200 ms (coupling caps settle, ADC fade-in) → load the preset → re-arm (rising edge on AUDIO_READY, check IO3) → XSMT high → ramp up.

**Runtime monitoring:**
- **FAULT_N low:** go immediately to the muted/disarmed state and log the cause where it can be told apart. Re-arm automatically only after FAULT_N has been high for a settle period and the audio is healthy again. Show the fault on the UI.
- **IO3 disagreeing with the commanded state:** disarm and report.

**Mandatory DSP safety:**
- DC/subsonic high-pass on every path.
- Output limiter.
- Hardware cannot detect corrupted samples, so these must always be present.

## 6. DSP chain (Phase 2): confirm details with Tom

- **Order:** sum to mono (L+R for the HIGH-LEVEL build and preamp stereo; LFE mode is already mono on both channels) → subsonic high-pass (2nd–4th order) → low-pass crossover (LR4/LR8) → about 6 PEQ bands → up to 2 all-pass (phase) sections → delay → volume → limiter.
- **Processing budget:** about 16 biquads using ESP-DSP f32, at 25–35 M cycles/s. That's comfortable on one core at 240 MHz.
- **Latency:** use small DMA blocks (16–32 frames).
- **TODO (Tom to specify):** default crossover and high-pass frequencies, the PEQ defaults, the preset count, and volume range and step.

## 7. UI (Phase 3): confirm details with Tom

- **Encoders:** three quadrature encoders with push switches, on J6 via the front-panel board. Use PCNT units with glitch filtering, or a debounced state machine.
- **Display:** on I2C1 (IO14/IO21). The panel provides +3V3_UI and +5V_UI (5 V displays supported). **Display model: TODO (Tom).**
- **12 V trigger (IO47):** trigger present → run and arm; trigger absent → mute and disarm (standby). Add a setting to ignore the trigger.
- **Status LED:** IO46.
- **USB_VBUS_SENSE:** could enable a configuration/console mode.
