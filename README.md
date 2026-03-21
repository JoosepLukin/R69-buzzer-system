# R69 Buzzer System

A wireless buzzer/alert system built on three custom PCBs communicating via ESP-NOW, RS-485, and 433 MHz RF. The system is designed for event/competition use where remote flash+buzzer units (Vilkur) are triggered from a central control panel (KontrollKast) with an RF-bridge board in between.

---

## System Architecture

```
                        ┌──────────────┐
  [433 MHz Remote] ────►│   Vilkur     │──── LEDs + Buzzer
                   ──┐  │  (ESP32-C3)  │
                     │  └──────────────┘
                     │         ▲▼ ESP-NOW
                     │  ┌──────────────────────┐
                     └─►│  SaatjaVastuvõtja    │
                        │     (ESP32-C3)        │
                        │  433 MHz RX + ESP-NOW │
                        └──────────┬───────────┘
                                   │ RS-485 (Cat5 cable, 12 V + GND)
                        ┌──────────▼───────────┐
                        │    KontrollKast       │
                        │      (ESP32-S3)       │
                        │  Touchscreen + Buttons│
                        │  RS-485 + 433 MHz RX  │
                        └──────────────────────┘
```

### Communication flows

| Path | Protocol | Purpose |
|------|----------|---------|
| KontrollKast ↔ SaatjaVastuvõtja | RS-485 JSON (115200 baud) | Commands, status, sensor data |
| SaatjaVastuvõtja ↔ Vilkur | ESP-NOW (2.4 GHz) | Blink commands, pings, status |
| 433 MHz Remote → Vilkur | ASK/OOK PT2262/EV1527 | Direct trigger (fallback) |
| 433 MHz Remote → SaatjaVastuvõtja | ASK/OOK PT2262/EV1527 | Relay to KontrollKast |
| 433 MHz Remote → KontrollKast | ASK/OOK PT2262/EV1527 | Pairing only |

---

## PCB Overview

### PCB 1 — Vilkur (`Vilkur/`)
The flash/buzzer unit. Runs on a 3S Li-ion battery pack with BMS and USB-C charging. Drives two LED groups and a piezo buzzer via N-channel MOSFETs. Listens for both ESP-NOW commands and 433 MHz RF signals (433 MHz acts as a fallback: it is automatically disabled when ESP-NOW messages are active and re-enabled after 1 minute of silence).

- **MCU**: ESP32-C3 Supermini
- **Power**: 3S 18650 Li-ion → BMS → 12 V boost → 3.3 V buck
- **Outputs**: LED group 1, LED group 2, piezo buzzer (all MOSFET low-side)
- **Inputs**: 433 MHz RXB6, rotary channel switch, pairing button, external trigger, battery ADC

### PCB 2 — KontrollKast (`KontrollKast/`)
The control centre. Powered from 12 V mains/PSU. Features a 320×480 touchscreen for configuration and status, three illuminated push-buttons for manual triggering, a 433 MHz receiver for pairing remotes, and an RS-485 link to SaatjaVastuvõtja.

- **MCU**: ESP32-S3
- **Display**: ST7796 320×480 SPI + FT6336U capacitive touch
- **Outputs**: 3× button backlight LEDs (MOSFET low-side, 12 V)
- **Inputs**: 3× push-buttons, 433 MHz RXB6 (pairing only)
- **Comms**: RS-485 (SN65HVD72) to SaatjaVastuvõtja

### PCB 3 — SaatjaVastuvõtja (`SaatjaVastuvõtja/`)
The RF bridge. Powered over Cat5 from KontrollKast (12 V). Receives 433 MHz signals and ESP-NOW messages, relaying everything to KontrollKast over RS-485. Sends ESP-NOW commands to Vilkur units based on instructions from KontrollKast.

- **MCU**: ESP32-C3 Supermini
- **Inputs**: 433 MHz RXB6, ESP-NOW from Vilkur
- **Comms**: RS-485 (SN65HVD72) to KontrollKast, ESP-NOW to Vilkur

---

## RS-485 JSON Protocol

All frames are UTF-8 text terminated with `\n`. Baud rate: **115200, 8N1**.

### SaatjaVastuvõtja → KontrollKast

```json
{"type":"433rx","code":12345678,"bits":24}
{"type":"espnow_rx","mac":"AA:BB:CC:DD:EE:FF","cmd":"blink","battery":3.72}
{"type":"ping_reply","mac":"AA:BB:CC:DD:EE:FF","battery":3.72}
{"type":"espnow_ack","mac":"AA:BB:CC:DD:EE:FF","success":true}
```

### KontrollKast → SaatjaVastuvõtja

```json
{"type":"cmd","mac":"AA:BB:CC:DD:EE:FF","action":"blink","duration":5000,"buzzer":true}
{"type":"cmd","mac":"broadcast","action":"stop"}
{"type":"cmd","mac":"AA:BB:CC:DD:EE:FF","action":"disable_433"}
{"type":"ping","mac":"AA:BB:CC:DD:EE:FF"}
{"type":"ping","mac":"broadcast"}
{"type":"pair_enable"}
```

---

## ESP-NOW Message Structure

```cpp
// Shared between Vilkur and SaatjaVastuvõtja
struct EspNowMsg {
    uint8_t  cmd;       // see CMD_* constants below
    uint16_t duration;  // blink duration ms
    bool     buzzer;    // enable buzzer during blink
    float    battery;   // Vilkur battery voltage (V), sent in pong
};

// cmd values
#define CMD_BLINK       0x01
#define CMD_STOP        0x02
#define CMD_DISABLE_433 0x03
#define CMD_PING        0x04
#define CMD_PONG        0x05
```

---

## Directory Structure

```
R69-buzzer-system/
├── README.md                        ← this file
├── Vilkur/
│   ├── README.md                    ← Vilkur detailed docs
│   └── Vilkur_Test/
│       └── Vilkur_Test.ino          ← Arduino test sketch (ESP32-C3)
├── KontrollKast/
│   ├── README.md                    ← KontrollKast detailed docs
│   └── KontrollKast_Test/
│       └── KontrollKast_Test.ino    ← Arduino test sketch (ESP32-S3)
├── SaatjaVastuvõtja/
│   ├── README.md                    ← SaatjaVastuvõtja detailed docs
│   └── SaatjaVastuvõtja_Test/
│       └── SaatjaVastuvõtja_Test.ino ← Arduino test sketch (ESP32-C3)
├── Altium/                          ← PCB design files (Altium Designer)
│   ├── Buzzer/                      ← Vilkur PCB
│   ├── KontrollKast/                ← KontrollKast PCB
│   └── VastuvotjaMoodul/            ← SaatjaVastuvõtja PCB
└── ESP-NOW Mõõtmised/               ← Range/latency measurement data & visualiser
```

---

## Required Arduino Libraries

| Library | Board | Install via |
|---------|-------|------------|
| `esp_now.h` + `WiFi.h` | All | Built-in (ESP32 Arduino core) |
| RCSwitch | Vilkur, KontrollKast, SaatjaVastuvõtja | Arduino Library Manager |
| TFT_eSPI | KontrollKast | Arduino Library Manager |
| ArduinoJson (v6) | KontrollKast, SaatjaVastuvõtja | Arduino Library Manager |

### Arduino Board Packages
- **ESP32-C3 Supermini**: `esp32` by Espressif, board `ESP32C3 Dev Module`
- **ESP32-S3**: `esp32` by Espressif, board `ESP32S3 Dev Module`

### TFT_eSPI Configuration (KontrollKast)
Edit `libraries/TFT_eSPI/User_Setup.h`:
```cpp
#define ST7796_DRIVER
#define TFT_WIDTH  320
#define TFT_HEIGHT 480
#define TFT_CS    15
#define TFT_DC    17
#define TFT_RST   16
#define TFT_MOSI  18
#define TFT_SCLK   8
#define TFT_MISO   9
#define TFT_BL     3
#define SPI_FREQUENCY  27000000
```

---

## License

Apache License 2.0 — see [LICENSE](LICENSE).
