# KontrollKast — Control Centre

PCB 2 of the R69 Buzzer System. The central control unit. Powered from a 12 V supply, it provides a touchscreen UI for configuration and status, three illuminated push-buttons for direct triggering, and communicates with Vilkur units indirectly through SaatjaVastuvõtja over RS-485. All ESP-NOW traffic passes through SaatjaVastuvõtja — KontrollKast itself has no wireless radio beyond the local 433 MHz receiver used only for pairing.

---

## Hardware Summary

| Component | Details |
|-----------|---------|
| MCU | ESP32-S3 |
| Power input | Barrel jack 12 V |
| Buck converter | 12 V → 3.3 V (ESP32 + logic) |
| Display | ST7796 320×480 SPI LCD |
| Touch | FT6336U capacitive touch (I2C, addr 0x38) |
| 433 MHz RX | RXB6 module (pairing only) |
| RS-485 transceiver | SN65HVD72DGKR (half-duplex) |
| RS-485 connector | Cat5 (also carries 12 V + GND to SaatjaVastuvõtja) |
| Buttons | 3× illuminated 12 V push-buttons |
| Button connectors | NS25 W4P: 12 V, GND, backlight MOSFET, button input GPIO |

---

## GPIO Pinout (ESP32-S3)

| GPIO | Direction | Function | Notes |
|------|-----------|----------|-------|
| 1 | Input | RXB6 433 MHz data | Pairing use only |
| 3 | Output | LCD backlight PWM | `ledcWrite` for brightness control |
| 4 | Output | RS-485 DE/RE (mode) | HIGH = transmit, LOW = receive |
| 5 | Output | RS-485 TX | Serial1 TX |
| 6 | Input | RS-485 RX | Serial1 RX |
| 8 | Output | LCD SCK (SPI) | Shared SPI bus |
| 9 | Input | LCD MISO (SPI) | Shared SPI bus |
| 10 | I/O | Touch CTP_SCL (I2C) | Wire SCL |
| 11 | Output | Touch CTP_RST | Active LOW reset |
| 12 | I/O | Touch CTP_SDA (I2C) | Wire SDA |
| 13 | Input | Touch CTP_INT | LOW when touch event pending |
| 14 | Output | SD card CS | SPI chip select |
| 15 | Output | LCD CS | SPI chip select |
| 16 | Output | LCD RST | Active LOW reset |
| 17 | Output | LCD DC (RS) | HIGH = data, LOW = command |
| 18 | Output | LCD MOSI (SPI) | Shared SPI bus |
| 21 | Output | Button 1 backlight MOSFET | Active HIGH |
| 35 | Output | Button 2 backlight MOSFET | Active HIGH |
| 36 | Input | Button 2 input | Active LOW (see W4P connector) |
| 39 | Output | Button 3 backlight MOSFET | Active HIGH |
| 40 | Input | Button 3 input | Active LOW |
| 48 | Input | Button 1 input | Active LOW |

---

## Button Connector (NS25 W4P)

Each button connector carries four signals:

| Pin | Signal |
|-----|--------|
| 1 | 12 V (backlight LED +) |
| 2 | GND (switch common) |
| 3 | Backlight MOSFET drain → LED – |
| 4 | Button switch output → GPIO input |

The button switch is wired between pin 4 and GND (pin 2). The GPIO uses `INPUT_PULLUP`; pressing the button pulls it LOW.

---

## Display — ST7796 + FT6336U

### SPI LCD (ST7796, 320×480)
```
ESP32-S3 GPIO  │ Ribbon pin │ Function
───────────────┼────────────┼──────────────────
GPIO 15        │ LCD_CS     │ Chip select (active LOW)
GPIO 16        │ LCD_RST    │ Reset (active LOW)
GPIO 17        │ LCD_RS/DC  │ Command=LOW / Data=HIGH
GPIO 18        │ SDI/MOSI   │ SPI data in
GPIO  8        │ SCK        │ SPI clock
GPIO  9        │ SDO/MISO   │ SPI data out
GPIO  3        │ LED        │ Backlight (PWM)
3.3 V          │ VCC        │ Logic power (backlight dim at 3.3 V)
```

> **Note**: The module datasheet recommends 5 V for full backlight brightness. At 3.3 V the backlight will be slightly dimmer but fully functional.

### Capacitive Touch (FT6336U, I2C address 0x38)
```
ESP32-S3 GPIO  │ Ribbon pin │ Function
───────────────┼────────────┼──────────────────
GPIO 10        │ CTP_SCL    │ I2C clock
GPIO 12        │ CTP_SDA    │ I2C data
GPIO 11        │ CTP_RST    │ Controller reset (active LOW)
GPIO 13        │ CTP_INT    │ Touch interrupt (active LOW)
```

Key FT6336U registers (read via Wire at address 0x38):

| Register | Name | Description |
|----------|------|-------------|
| 0x02 | TD_STATUS | Number of touch points (0 or 1) |
| 0x03 | P1_XH | Touch 1 X high byte (bits [3:0]) + event flag |
| 0x04 | P1_XL | Touch 1 X low byte |
| 0x05 | P1_YH | Touch 1 Y high byte (bits [3:0]) |
| 0x06 | P1_YL | Touch 1 Y low byte |

### TFT_eSPI Configuration

Add to `libraries/TFT_eSPI/User_Setup.h`:
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
#define SPI_FREQUENCY     27000000
#define SPI_READ_FREQUENCY 20000000
```

---

## RS-485 Interface

- **Transceiver**: SN65HVD72DGKR (3.3 V, half-duplex)
- **Mode pin** (GPIO 4): HIGH = transmit (DE+RE asserted), LOW = receive
- **Baud rate**: 115200, 8N1
- **Physical**: Differential pair over Cat5 cable; same cable carries 12 V and GND to power SaatjaVastuvõtja

```cpp
Serial1.begin(115200, SERIAL_8N1, /*RX=*/6, /*TX=*/5);
```

Transmit procedure:
1. `digitalWrite(RS485_MODE, HIGH);`
2. `Serial1.println(jsonString);`
3. `Serial1.flush();`
4. `digitalWrite(RS485_MODE, LOW);`

---

## Behaviour Logic

### Startup
1. Initialize display → splash screen.
2. Initialize touch controller.
3. Initialize RS-485 and send a broadcast ping to discover connected Vilkur units.
4. Show main UI.

### Main UI (touchscreen)
- **Status panel**: Connected Vilkur list (MAC, battery voltage, last-seen).
- **Manual trigger buttons** (on screen and physical buttons):
  - Blink All — broadcast `CMD_BLINK` to all Vilkurs via RS-485 → SaatjaVastuvõtja
  - Stop All — broadcast `CMD_STOP`
  - Ping All — send ping broadcast; update status panel on reply
- **Settings page** (touch navigation):
  - Blink duration (ms)
  - Buzzer on/off during blink
  - ESP-NOW channel selection (must match rotary switch on Vilkur)
  - 433 MHz pairing mode

### Physical buttons
Each physical button (1, 2, 3) can be assigned to a specific Vilkur or broadcast group. Pressing a button sends `CMD_BLINK` to its assigned target. The corresponding backlight LED lights up while the Vilkur is blinking.

### 433 MHz pairing mode
Activated from the settings page or by a long-press on button 1. The on-board RXB6 receiver listens for the next PT2262/EV1527 code and stores it as an authorised trigger code. This code is then relayed to SaatjaVastuvõtja (and onward to Vilkurs) so that the remote directly triggers blinking.

### Vilkur monitoring (ping)
Every 5 seconds KontrollKast sends `{"type":"ping","mac":"broadcast"}` over RS-485. SaatjaVastuvõtja forwards this as ESP-NOW `CMD_PING`. Vilkur replies with `CMD_PONG` (including battery voltage). SaatjaVastuvõtja relays the reply back over RS-485 as `{"type":"ping_reply",...}`. KontrollKast marks the Vilkur as connected and updates its voltage on screen.

---

## Required Libraries

| Library | Source |
|---------|--------|
| TFT_eSPI | Arduino Library Manager |
| ArduinoJson (v6) | Arduino Library Manager |
| RCSwitch (by sui77) | Arduino Library Manager |
| `Wire.h` | Built-in (ESP32 Arduino core) |
| `HardwareSerial` | Built-in (ESP32 Arduino core) |

---

## Test Sketch

See [`KontrollKast_Test/KontrollKast_Test.ino`](KontrollKast_Test/KontrollKast_Test.ino).

The test sketch exercises:
- ST7796 display (colour fill, text rendering)
- FT6336U touch (live X/Y display)
- Three physical buttons + backlight LEDs
- 433 MHz reception (code displayed on screen + Serial)
- RS-485 TX/RX (periodic ping to SaatjaVastuvõtja, display replies)
- On-screen touch buttons: Blink All, Stop All, Ping All, 433 Pair

Open Serial monitor at **115200 baud** for debug output.
