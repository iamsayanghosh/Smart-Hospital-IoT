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
 │ • FreeRTOS System Timers       │       │ P3: bedTask (50 Hz / 20 ms)             │
 └────────────────────────────────┘       │ P3: vitalTask (Adaptive 5s - 60s)       │
                                          │ P3: ecgTask (Fixed 5 Hz / 200 ms)       │
                                          │ P2: temperatureTask (Adapt 5s - 60s)    │
                                          │ P2: motionTask (Fixed 1000 ms)          │
                                          │ P2: environmentTask (Fixed 1000 ms)     │
                                          │ P2: mqttTask (Round-robin 3000 ms)      │
                                          │ P2: samplingTask (Supervisory 1000ms)   │
                                          │ P2: alertTask (Event Semaphore)         │
                                          │ P2: offlineTask (Cadence & Health 50ms) │
                                          │ P1: displayTask (OLED & Aggregator 100ms│
                                          └─────────────────────────────────────────┘
```

---

## 2. Assignment Task Implementation

### Task 1: Multi-Sensor Expansion Using FreeRTOS
* **Physical Temperature:** Dallas DS18B20 on 1-Wire bus (`GPIO 4`).
* **Physical Motion:** HC-SR501 PIR sensor on `GPIO 27`.
* **Simulated Photoplethysmogram (PPG):** Optical Heart Rate (`50 - 100 BPM`) and $\text{SpO}_2$ (`92 - 100 %`).
* **Simulated Electrocardiogram (ECG):** Real-time Gaussian superposition model generating P/Q/R/S/T cardiac waveform at 5 Hz (200 ms).
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
* **Three Health States:** `SYSTEM_ONLINE`, `SYSTEM_DEGRADED` (task heartbeat loss), and `SYSTEM_OFFLINE` (Wi-Fi or MQTT down).
* **Unified Quiet Bootup & Debounce:** An `OfflineBannerGate` suppresses the offline banner and buzzer during the first 10 seconds of power-up and ignores transient network hiccups under 5 seconds.
* **Non-Clobbering OLED Banner:** When offline, an inverted ticker bar (`LOGGING OFFLINE`) renders on the bottom row (`y = 54..63`) while medication dosage remains visible.
* **Acoustic Cadences:** Distinct failure cadences for Wi-Fi failure (rapid double-pip) vs. MQTT broker failure (slow beep). Medication critical alarms take absolute priority over network beeps.
* **Offline SRAM Buffer:** Outgoing telemetry packets are diverted to a 64-item ring buffer (sized up to 512 bytes per packet).
* **Reconnection Replay:** Replays missed records in FIFO order, sharing the 3000 ms round-robin slot to remain safely within Adafruit IO's 30 msgs/min quota.

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

### Credentials Safety Note
Before sharing or zipping this repository, **do not include** `include/adafruit_io_config.local.h`.
Use the provided `include/adafruit_io_config.local.h.example` template for distribution.

