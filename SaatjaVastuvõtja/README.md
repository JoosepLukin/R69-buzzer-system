# SaatjaVastuvõtja — RF Bridge

PCB 3 of the R69 Buzzer System. A small bridge board that sits physically close to the event area and connects KontrollKast (wired, over Cat5 RS-485) to Vilkur units (wireless, ESP-NOW). It also forwards 433 MHz RF signals received from remotes to KontrollKast over RS-485.

The name combines the Estonian words *saatja* (sender) and *vastuvõtja* (receiver), reflecting its dual transmit/receive role.

---

## Hardware Summary

| Component | Details |
|-----------|---------|
| MCU | ESP32-C3 Supermini |
| Power input | Cat5 connector — 12 V from KontrollKast |
| Buck converter | 12 V → 3.3 V (ESP32 + logic) |
| 433 MHz RX | RXB6 module |
| RS-485 transceiver | SN65HVD72DGKR (half-duplex, same chip as KontrollKast) |
| RS-485 connector | Cat5 (12 V, GND, RS-485 A/B from KontrollKast) |

---

## GPIO Pinout (ESP32-C3 Supermini)

| GPIO | Direction | Function | Notes |
|------|-----------|----------|-------|
| 1 | Output | RS-485 DE/RE (mode) | HIGH = transmit, LOW = receive |
| 2 | Output | RS-485 TX | Serial1 TX |
| 3 | Input | RS-485 RX | Serial1 RX |
| 4 | Input | RXB6 433 MHz data | Interrupt-driven |

---

## RS-485 Interface

- **Transceiver**: SN65HVD72DGKR, same as KontrollKast
- **Mode pin** (GPIO 1): HIGH = transmit, LOW = receive
- **Baud rate**: 115200, 8N1
- **Physical**: Differential pair over Cat5 cable to KontrollKast

```cpp
Serial1.begin(115200, SERIAL_8N1, /*RX=*/3, /*TX=*/2);
```

Transmit procedure:
1. `digitalWrite(RS485_MODE, HIGH);`
2. `Serial1.println(jsonString);`
3. `Serial1.flush();`
4. `digitalWrite(RS485_MODE, LOW);`

---

## Behaviour Logic

### 433 MHz reception
1. RCSwitch library decodes PT2262/EV1527 codes on GPIO 4.
2. On valid code reception → relay immediately over RS-485 to KontrollKast:
   ```json
   {"type":"433rx","code":12345678,"bits":24}
   ```

### RS-485 reception (commands from KontrollKast)
All incoming JSON frames are parsed with ArduinoJson. Supported commands:

| `type` field | `action` / behaviour | ESP-NOW sent |
|---|---|---|
| `cmd` + `action:"blink"` | Forward blink command to target MAC (or broadcast) | `CMD_BLINK` with `duration`, `buzzer` |
| `cmd` + `action:"stop"` | Stop blinking | `CMD_STOP` |
| `cmd` + `action:"disable_433"` | Disable 433 MHz fallback on Vilkur | `CMD_DISABLE_433` |
| `ping` | Send ESP-NOW ping, await pong, relay reply | `CMD_PING` |
| `pair_enable` | Enter 433 MHz pairing mode: next received code sent back over RS-485 | — |

### ESP-NOW reception (from Vilkur)
Any incoming ESP-NOW frame is immediately relayed over RS-485 to KontrollKast:

```json
{"type":"espnow_rx","mac":"AA:BB:CC:DD:EE:FF","cmd":"pong","battery":11.82}
```

For ping replies specifically:
```json
{"type":"ping_reply","mac":"AA:BB:CC:DD:EE:FF","battery":11.82}
```

### ESP-NOW channel
Must match the rotary switch setting on the target Vilkur units (channel 1, 6, or 11). During testing, channel 1 is the default.

---

## ESP-NOW Message Structure

```cpp
struct EspNowMsg {
    uint8_t  cmd;       // CMD_BLINK / CMD_STOP / CMD_DISABLE_433 / CMD_PING / CMD_PONG
    uint16_t duration;  // blink duration ms (CMD_BLINK only)
    bool     buzzer;    // enable buzzer (CMD_BLINK only)
    float    battery;   // Vilkur battery V (CMD_PONG only)
};

#define CMD_BLINK       0x01
#define CMD_STOP        0x02
#define CMD_DISABLE_433 0x03
#define CMD_PING        0x04
#define CMD_PONG        0x05
```

ESP-NOW broadcast address `FF:FF:FF:FF:FF:FF` is used to reach all Vilkur units simultaneously when `mac:"broadcast"` is received from KontrollKast.

---

## Required Libraries

| Library | Source |
|---------|--------|
| `esp_now.h` | Built-in (ESP32 Arduino core) |
| `WiFi.h` | Built-in (ESP32 Arduino core) |
| RCSwitch (by sui77) | Arduino Library Manager |
| ArduinoJson (v6) | Arduino Library Manager |

---

## Test Sketch

See [`SaatjaVastuvõtja_Test/SaatjaVastuvõtja_Test.ino`](SaatjaVastuvõtja_Test/SaatjaVastuvõtja_Test.ino).

The test sketch exercises:
- 433 MHz reception (prints decoded codes to Serial + relays to KontrollKast over RS-485)
- RS-485 RX: parses JSON commands from KontrollKast, executes ESP-NOW sends
- ESP-NOW RX: relays Vilkur messages to KontrollKast over RS-485
- Serial mirror of all RS-485 traffic at 115200 baud for debug

### Test sequence (with all three boards connected)
1. Open Serial monitors on all three boards.
2. Trigger 433 MHz remote → code appears on KontrollKast Serial/screen and here.
3. From KontrollKast (touch "Ping All") → JSON frame visible on this board's Serial → ESP-NOW ping sent → Vilkur pong received → relay to KontrollKast.
4. From KontrollKast (touch "Blink All") → CMD_BLINK sent via ESP-NOW → Vilkur LEDs blink.
