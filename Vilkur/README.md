# Vilkur — Flash & Buzzer Unit

PCB 1 of the R69 Buzzer System. A battery-powered wireless flash/buzzer node that responds to both ESP-NOW commands (primary) and 433 MHz RF signals (fallback). Multiple Vilkur units can operate simultaneously on the same ESP-NOW channel.

---

## Hardware Summary

| Component | Details |
|-----------|---------|
| MCU | ESP32-C3 Supermini |
| Battery | 3S Li-ion (three 18650 cells in holders) |
| BMS | Soldered directly to PCB |
| Charging | USB-C connector → boost charger module (5 V in, ~12.6 V out); charge indicator LED on underside (ON while charging) |
| On/Off switch | Connects battery pack to BMS + rest of circuit |
| Buck converter | 12 V → 3.3 V (powers ESP32 and logic) |
| Battery indicator | Push-to-read voltage display connected directly to battery |
| 433 MHz RX | RXB6 module |
| Outputs | LED group 1, LED group 2, piezo buzzer — all N-MOSFET low-side switched |
| Connectors | NS25 W2P for each MOSFET output; NS25 W2P for external signal |

---

## GPIO Pinout (ESP32-C3 Supermini)

| GPIO | Direction | Function | Notes |
|------|-----------|----------|-------|
| 0 | Input | RXB6 433 MHz data | Strapping pin — idle HIGH; avoid pulling LOW at boot |
| 1 | Output | Pairing feedback LED | Active HIGH |
| 2 | Output | Piezo buzzer (MOSFET gate) | 100 Ω series to gate; 10 kΩ pull-down |
| 3 | Output | LED group 2 (MOSFET gate) | 100 Ω series to gate; 10 kΩ pull-down |
| 4 | Output | LED group 1 (MOSFET gate) | 100 Ω series to gate; 10 kΩ pull-down |
| 5 | Input (ADC) | Battery voltage divider | Reads ¼ of battery voltage; multiply by 4 to get real V |
| 8 | Input | External signal | Pulled HIGH externally when connector is bridged |
| 9 | Input | Pairing tactile button | Active LOW (to GND); use INPUT_PULLUP |
| 10 | Input | Rotary switch position 1 | LOW = channel 1 selected; use INPUT_PULLUP |
| 20 | Input | Rotary switch position 2 | LOW = channel 6 selected; use INPUT_PULLUP |
| 21 | Input | Rotary switch position 3 | LOW = channel 11 selected; use INPUT_PULLUP |

### USB Data Pass-through
The USB-C data pins are routed via NS25 W2P connector to the ESP32-C3 Supermini's USB data pads, allowing firmware flashing without removing the module.

---

## Power Path

```
18650 × 3 (3S)
    │
    ├──► Battery voltage indicator (direct, push-to-read)
    │
    ▼
   BMS
    │
On/Off switch
    │
    ├──► USB boost charger module (USB-C in)
    │         └── Charge LED (bottom of PCB, ON while charging)
    │
    └──► 12 V rail
              │
              ├──► Buck converter → 3.3 V → ESP32-C3 + logic
              ├──► LED group 1 (via MOSFET)
              ├──► LED group 2 (via MOSFET)
              └──► Piezo buzzer (via MOSFET)
```

### Battery Voltage ADC
```
Battery (+) ──[R1]──┬──[R2]── GND
                    │
                  GPIO 5 (ADC)

Ratio: 1/4  →  GPIO5 reads ¼ of battery voltage
Real voltage = ADC_raw × (3.3 / 4095) × 4
Healthy 3S range: ~9.0 V (empty) – 12.6 V (full)
```

---

## MOSFET Output Circuit

Each of the three outputs (LED group 1, LED group 2, buzzer) uses the same circuit:

```
GPIO_x ──[100 Ω]── GATE
                     │
                   N-FET
                     │
                  DRAIN ── Load (–)   Load (+) ── 12 V (via NS25 connector)
                     │
                  SOURCE ── GND
                     │
                  [10 kΩ pull-down to GND]
```

---

## Rotary Switch — ESP-NOW Channel Selection

The 3-position rotary switch selects the Wi-Fi channel used by ESP-NOW. SaatjaVastuvõtja must be configured to the same channel.

| GPIO 10 | GPIO 20 | GPIO 21 | Channel |
|---------|---------|---------|---------|
| LOW | HIGH | HIGH | 1 (default) |
| HIGH | LOW | HIGH | 6 |
| HIGH | HIGH | LOW | 11 |
| HIGH | HIGH | HIGH | 1 (fallback) |

---

## Behaviour Logic

### Normal operation
1. On power-up: short self-test (blink + buzz).
2. Listen for ESP-NOW messages on the selected channel.
3. Listen for 433 MHz PT2262/EV1527 codes (fallback, always active unless disabled).

### ESP-NOW commands received
| Command | Action |
|---------|--------|
| `CMD_BLINK` | Blink LED groups in pattern, optionally enable buzzer, for `duration` ms |
| `CMD_STOP` | Immediately stop blinking/buzzing |
| `CMD_DISABLE_433` | Stop listening to 433 MHz receiver |
| `CMD_PING` | Reply with `CMD_PONG` including current battery voltage |

### 433 MHz fallback
- **Active by default**.
- Disabled when `CMD_DISABLE_433` is received via ESP-NOW.
- **Auto re-enabled** after **60 seconds** of no ESP-NOW messages (watchdog timer).
- Rationale: if the ESP-NOW link is lost the Vilkur remains controllable by 433 MHz remote.

### Pairing mode (button GPIO 9, hold 3 s)
1. Feedback LED (GPIO 1) blinks rapidly.
2. First 433 MHz code received is stored in NVM as the "paired code".
3. LED stops blinking; pairing complete.
4. Only the paired code triggers the unit in 433 MHz mode.

### External signal (GPIO 8)
When the NS25 connector is externally bridged to 3.3 V the pin reads HIGH. Can be used to trigger the unit from a wired signal.

---

## Required Libraries

| Library | Source |
|---------|--------|
| `esp_now.h` | Built-in (ESP32 Arduino core) |
| `WiFi.h` | Built-in (ESP32 Arduino core) |
| RCSwitch (by sui77) | Arduino Library Manager |

---

## Test Sketch

See [`Vilkur_Test/Vilkur_Test.ino`](Vilkur_Test/Vilkur_Test.ino).

The test sketch exercises:
- LED group 1 & 2 blink patterns
- Piezo buzzer
- Battery voltage ADC reading
- Rotary switch position detection
- External signal pin reading
- 433 MHz reception (prints received codes to Serial)
- ESP-NOW broadcast receive/transmit
- Pairing button (hold 3 s)

Open Serial monitor at **115200 baud** and follow the menu.
