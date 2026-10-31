# Smart Hospital IoT System — ESP32 & FreeRTOS

An enterprise-grade, concurrent medical telemetry and facility management firmware implemented on the ESP32 microcontroller using the Arduino framework and FreeRTOS SMP.

Designed and verified for physical hardware and the Wokwi ESP32 simulator (`diagram.json`).

---

## 1. System Architecture & Concurrency Model

All application worker tasks are pinned to **Core 1**, dedicating **Core 0** to the Espressif Wi-Fi MAC, TCP/IP stack (lwIP), and background network services.

```
                                  ESP32 DUAL CORE
       CORE 0: Network Stacks                    CORE 1: Application Tasks
 ┌────────────────────────────────┐       ┌─────────────────────────────────────────┐
 │ • Espressif Wi-Fi MAC / PHY    │       │ P4: watchdogTask (Auditor, 1000 ms)     │
 │ • lwIP TCP/IP Stack            │       │ P3: medicationTask (50 Hz / 20 ms)      │
 │ • FreeRTOS System Timers       │       │ P3: vitalTask (Adaptive 5s - 60s)       │
 └────────────────────────────────┘       │ P3: ecgTask (Fixed 5 Hz / 200 ms)       │
                                          │ P2: temperatureTask (Adapt 5s - 60s)    │
                                          │ P2: motionTask (Fixed 1000 ms)          │
                                          │ P2: environmentTask (Fixed 1000 ms)     │
                                          │ P2: bedTask (50 Hz / 20 ms)             │
                                          │ P2: mqttTask (Round-robin 3000 ms)      │
                                          │ P2: samplingTask (Supervisory 1000 ms)  │
                                          │ P2: alertTask (Event Semaphore)         │
                                          │ P2: offlineTask (Cadence & Health 50 ms)│
                                          │ P1: displayTask (OLED & Aggregator 100ms)│
                                          └─────────────────────────────────────────┘
```

---

## 2. Assignment Task Implementation

### Task 1: Multi-Sensor Expansion Using FreeRTOS
* **Physical Temperature:** Dallas DS18B20 on 1-Wire bus (`GPIO 4`).
* **Physical Motion:** HC-SR501 PIR sensor on `GPIO 27`.
* **Simulated Photoplethysmogram (PPG):** Optical Heart Rate (`50 - 100 BPM`) and $\text{SpO}_2$ (`92 - 100 %`).
* **Simulated Electrocardiogram (ECG):** Illustrative real-time Gaussian superposition model simulating cardiac cycle morphology at 5 Hz (200 ms).
* **Supervision:** Dual-tier watchdog coupling ESP-IDF Task Watchdog Timer (TWDT, 8000 ms) with an application heartbeat auditor (`watchdogTask`, 6000 ms).

### Task 2: Dual-Role Cloud IoT Monitoring (Role-Based Dashboards)
* **Medical Staff Telemetry:** `medical-heart-rate`, `medical-spo2`, `medical-body-temperature`.
* **Facility Management Telemetry:** `facility-room-temperature`, `facility-oxygen`, `facility-aqi`.
* **Consolidated Events & Summaries:** `hospital-events` receives real-time medical/facility alerts and an aggregated 60-second summary report.

### Task 3: Smart Medication Infusion Safety System
* **Infusion Rate Bounds:** 0.0 to 100.0 mg/hr commanded remotely via `medication-dosage`.
* **Safety Tiers:**
  * **Normal (0.0 - 79.9 mg/hr):** Solid Green LED (`GPIO 33`).
  * **Warning (80.0 mg/hr):** Solid Yellow LED (`GPIO 26`).
  * **Critical (> 80.0 mg/hr):** Red LED (`GPIO 25`) and Piezo Buzzer (`GPIO 14`) rapidly blink/beep at 3.3 Hz (150 ms period). The dosage is clamped at 80.0 mg/hr until confirmed.
* **Physical Bedside Interlock:** Clinician must physically press the button on `GPIO 13` to unlock dosages above 80 mg/hr.
* **Asymmetric Slew Limiter:** Increases are capped to $+5\text{ mg/hr/s}$ to prevent bolus shock; reductions apply instantaneously.
* **Permanent OLED Display:** The 128x64 OLED display permanently shows current dosage, status, and confirmation cues online and offline.

### Task 4: Intelligent Remote Bed Elevation
* **Actuation:** SG90 Servo on `GPIO 18` with kinematic ramp smoothing (1.5°/s, 2.0°/s, 5.0°/s).
* **Predefined Clinical Modes:**
  * **Sleeping Rest (10°):** Normal baseline physiological status.
  * **Breathing Support (45°):** Semi-Fowler's positioning automatically triggered by hypoxia ($\text{SpO}_2 \in [88\%, 91\%]$).
  * **Emergency (90°):** Resuscitation positioning triggered by critical vitals ($\text{SpO}_2 < 88\%$, $\text{HR} < 45$ or $> 120\text{ BPM}$, extreme body temperature $<34.0^\circ\text{C}$ or $>39.5^\circ\text{C}$).
* **Manual Override:** Clinicians can command angles ($0 - 90^\circ$) via `bed-angle`. Any change in clinical status automatically overrides manual positioning.

### Task 5: Smart Dynamic Sampling Rate (Adaptive Monitoring)
* **Energy-Efficient Stable Mode:** Sets vital and temperature acquisition to **60 seconds** when all physiological markers are normal.
* **Acute Crisis Mode:** Automatically accelerates sampling to **5 seconds** upon any abnormal vital sign.
* **Manual Slider Override:** Doctor can adjust sampling interval (5 s to 60 s) via `sampling-rate`. Shifts in patient status take precedence.

### Task 6: Fault-Tolerant Offline Detection & Buffering
* **Three Health States:** `SYSTEM_ONLINE`, `SYSTEM_DEGRADED` (detected when transport is online but a worker task misses its watchdog heartbeat), and `SYSTEM_OFFLINE` (Wi-Fi or MQTT disconnected).
* **Unified Quiet Bootup & Debounce:** An `OfflineBannerGate` suppresses the offline banner and buzzer during the first 10 seconds of power-up and ignores transient network hiccups under 5 seconds.
* **Non-Clobbering OLED Banner:** When offline, an inverted ticker bar (`LOGGING OFFLINE`) renders on the bottom row (`y = 54..63`) while medication dosage remains visible.
* **Acoustic Cadences:** Distinct failure cadences for Wi-Fi failure (rapid double-pip) vs. MQTT broker failure (slow beep). Medication critical alarms take absolute priority over network beeps.
* **Offline SRAM Buffer:** Outgoing telemetry packets are diverted to a 64-item **bounded FIFO queue** (sized up to 512 bytes per packet). If network disconnection persists beyond 64 messages (~3.2 minutes of continuous telemetry), the queue preserves earlier historical data and drops newest incoming entries until reconnection.
* **Reconnection Replay:** Replays missed records in strict FIFO order, sharing the 3000 ms round-robin slot to remain safely within Adafruit IO's 30 msgs/min quota.

---

## 3. Hardware Pinout Table

| Peripheral | Component ID | ESP32 GPIO | Description / Mode |
| :--- | :--- | :--- | :--- |
| **OLED Display** | `oled1` | `GPIO 21` (SDA)<br>`GPIO 22` (SCL) | SSD1306 128x64 I2C monochrome at address `0x3C` |
| **Body Temperature** | `temp1` | `GPIO 4` | Dallas DS18B20 1-Wire Digital Bus |
| **Motion Detector** | `pir1` | `GPIO 27` | HC-SR501 PIR sensor (Digital Input) |
| **Bed Elevation Servo** | `servo1` | `GPIO 18` | SG90 50 Hz PWM Servo Actuator (0 - 90°) |
| **Bedside Button** | `btn1` | `GPIO 13` | Momentary pushbutton with internal pull-up (Active-LOW) |
| **Red Status LED** | `led1` | `GPIO 25` | Medication Critical Indicator (Rapid 3.3 Hz blink / solid confirmed) |
| **Yellow Status LED**| `led2` | `GPIO 26` | Medication Warning Indicator (Solid at 80.0 mg/hr) |
| **Green Status LED** | `led3` | `GPIO 33` | Medication Normal Baseline Indicator (0 - 79.9 mg/hr) |
| **Piezo Buzzer** | `bz1` | `GPIO 14` | Acoustic transducer (Medication alarm + Network failure cadences) |

---

## 4. Interactive Simulation & Testing (Wokwi / Serial CLI)

The firmware includes an interactive Serial Command Line Interface (115200 baud) strictly bounded to the visual dial limits of the Adafruit IO dashboard gauges:

### Patient Vitals Simulation Commands:
* `SET_HR <40-140>`: Force patient heart rate (e.g. `SET_HR 105` for Warning, `SET_HR 130` for Emergency).
* `SET_SPO2 <80-100>`: Force blood oxygen saturation (e.g. `SET_SPO2 90` to trigger 45° Breathing Support; `SET_SPO2 85` to trigger 90° Emergency).
* `SET_TEMP <34-42>`: Force body temperature in °C (e.g. `SET_TEMP 39.8` to trigger critical hyperthermia).
* `RESET_VITALS`: Clear all vital overrides and resume nominal random physiological vitals.

### Facility Simulation Commands (Persist until RESET):
* `SET_ROOM <15-40>`: Force simulated ambient room temperature.
* `SET_OXYGEN <18-25>`: Force ambient facility oxygen percentage.
* `SET_AQI <0-300>`: Force Air Quality Index.
* `RESET_FACILITY`: Restore default room parameters (22.0 °C, 20.8% O2, 35 AQI).
* `SHOW_TASK2`: Print MQTT topic mappings in the console.

### Offline & Replay Simulation Commands (Task 6 in Wokwi):
* `SIM_OFFLINE WIFI` (or `SIM_OFFLINE`): Simulates a Wi-Fi link drop (`WiFi.disconnect()`), activating the rapid double-pip acoustic cadence (`BUZZER_MODE_WIFI_FAIL`), bottom OLED warning banner (`LOGGING OFFLINE`), and routing outbound telemetry into the 64-record FIFO RAM buffer.
* `SIM_OFFLINE MQTT`: Simulates an MQTT broker disconnect while maintaining Wi-Fi association, activating the slow single-beep cadence (`BUZZER_MODE_MQTT_FAIL`), bottom OLED warning banner, and FIFO RAM buffering.
* `SIM_ONLINE`: Restores network connectivity, allowing background re-registration and orderly FIFO replay of all buffered records to Adafruit IO.

---

## 5. How to Build & Run

### Compile Firmware
```bash
pio run
```
The compiled binaries will be output to:
* `.pio/build/esp32dev/firmware.bin`
* `.pio/build/esp32dev/firmware.factory.bin`

### Upload to Physical ESP32
```bash
pio run --target upload
```

### Automated Unit Testing
Unit tests are implemented under `test/test_main.cpp` with 15 unit tests covering strict numeric string parsers (NaN, Inf, float integers, negative boundary handling) and medication dosage safety clamping.
```bash
pio test -e esp32dev
```

---

## 6. Known Architectural Limitations & Design Notes

1. **Task Watchdog & SYSTEM_DEGRADED Visibility:**
   `SYSTEM_DEGRADED` indicates an internal FreeRTOS worker task has starved its heartbeat while Wi-Fi and MQTT transport remain intact. In practice, the ESP-IDF Task Watchdog Timer (TWDT) is configured with an 8000 ms panic threshold; if a task hangs permanently, the chip will intentionally reset ~2 seconds after entering `SYSTEM_DEGRADED` to guarantee fail-safe recovery in medical environments.
2. **Bounded FIFO Queue Behavior:**
   The offline buffer utilizes a fixed-capacity FreeRTOS queue holding up to 64 records of 576 bytes each (~36.8 KB SRAM). To avoid dropping high-priority initial failure states, it drops newest incoming entries when full rather than overwriting historical telemetry.
3. **Adafruit IO Free Quota Optimization:**
   Adafruit IO limits accounts to 30 data points per minute. The firmware paces telemetry round-robin at 3-second intervals and coordinates replayed messages to stay safely below this global ceiling.
4. **Wokwi Environmental Sensing:**
   In the Wokwi simulation environment, ambient room parameters (Room Temp, Oxygen %, AQI) and vitals (PPG, ECG) are generated via simulated algorithms with persistent UART CLI overrides (`SET_ROOM`, `SET_OXYGEN`, `SET_AQI`, `SET_HR`, `SET_SPO2`, `SET_TEMP`).
5. **Adafruit IO 10-Feed Limit on Free Tier:**
   Due to Adafruit IO's 10-feed restriction on standard accounts, all clinical alerts, facility alerts, and 60-second summary reports are multiplexed onto the single shared hospital-events feed.
6. **Replayed Telemetry Timestamps:**
   Replayed offline records carry the timestamp of broker reconnection/receipt rather than historic capture time due to Adafruit IO's standard MQTT format and absence of an onboard battery-backed RTC.
7. **Logical Role Separation:**
   The separation between Medical Staff and Facility Management is logical (dedicated feeds and dashboard views) rather than cryptographic Role-Based Access Control (RBAC).

---

## 7. Credentials Safety & Distribution Note

When distributing or submitting the project archive, **never include** `include/adafruit_io_config.local.h`.
The build system in `include/adafruit_io_config.h` automatically detects whether the local configuration file exists:
- If present: loads private credentials.
- If absent: falls back cleanly to placeholder values, keeping the repository safe and buildable without leaking secrets.


---

## 8. Running in Browser Wokwi (wokwi.com)

1. Open **[wokwi.com](https://wokwi.com)** and create a new **ESP32** Arduino project.
2. **Main Sketch:** Paste `src/main.cpp` into the `sketch.ino` tab.
3. **Hardware Schematic:** Paste `diagram.json` into the `diagram.json` tab.
4. **Library Manifest:** Create a new tab named `libraries.txt` and paste the contents of `libraries.txt`.
5. **Add Source Modules:** Upload or paste all remaining `.cpp` and `.h` files into the project (placed flat next to `sketch.ino`).
6. **Credentials (Private):** Add an optional tab named `adafruit_io_config.local.h` with your Adafruit IO username and key to stream live to your dashboard.
7. **Interactive Task 6 Demo:** In the Serial Monitor, type `SIM_OFFLINE WIFI` (tests double-pip cadence) or `SIM_OFFLINE MQTT` (tests single-beep cadence), let data accumulate in the buffer, then type `SIM_ONLINE` to watch the FIFO replay drain to the broker!
