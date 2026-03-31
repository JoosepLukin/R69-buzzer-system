/*
 * Vilkur_Test.ino — Hardware test sketch for Vilkur PCB (ESP32-C3 Supermini)
 *
 * Tests:
 *   - LED group 1 (GPIO 4), LED group 2 (GPIO 3), piezo buzzer (GPIO 2)
 *   - Pairing feedback LED (GPIO 1)
 *   - Battery voltage ADC (GPIO 5, ×4 divider)
 *   - Rotary channel switch (GPIO 10/20/21, active-LOW, 3-position)
 *   - External signal input (GPIO 8)
 *   - Pairing button (GPIO 9, active-LOW, hold 3 s for pairing mode)
 *   - 433 MHz reception (RXB6 on GPIO 0, RCSwitch library)
 *   - ESP-NOW receive/transmit (broadcast)
 *
 * Serial monitor: 115200 baud
 * Board: ESP32C3 Dev Module
 * Required libraries: RCSwitch (by sui77)
 *
 * RS-485 JSON protocol (shared across all boards):
 *   CMD_BLINK       0x01  — blink LEDs (+optional buzzer) for duration ms
 *   CMD_STOP        0x02  — stop blinking
 *   CMD_DISABLE_433 0x03  — stop listening to 433 MHz
 *   CMD_PING        0x04  — ping: Vilkur replies with CMD_PONG + battery voltage
 *   CMD_PONG        0x05  — ping reply
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <RCSwitch.h>

// ─── Pin definitions ─────────────────────────────────────────────────────────
#define PIN_433_DATA    0   // RXB6 data output (strapping pin – idle HIGH)
#define PIN_LED_PAIR    1   // Pairing feedback LED
#define PIN_BUZZER      2   // Piezo buzzer via MOSFET
#define PIN_LED2        3   // LED group 2 via MOSFET
#define PIN_LED1        4   // LED group 1 via MOSFET
#define PIN_BATT_ADC    5   // Battery ADC (reads ¼ of real voltage)
#define PIN_EXT_SIG     8   // External signal (HIGH when bridged)
#define PIN_PAIR_BTN    9   // Pairing button (active LOW)
#define PIN_ROT_CH1    10   // Rotary switch pos 1 → channel 1  (LOW = selected)
#define PIN_ROT_CH2    20   // Rotary switch pos 2 → channel 6
#define PIN_ROT_CH3    21   // Rotary switch pos 3 → channel 11

// ─── ESP-NOW message struct (must match SaatjaVastuvõtja) ────────────────────
#define CMD_BLINK       0x01
#define CMD_STOP        0x02
#define CMD_DISABLE_433 0x03
#define CMD_PING        0x04
#define CMD_PONG        0x05

struct EspNowMsg {
    uint8_t  cmd;
    uint16_t duration;   // ms (used with CMD_BLINK)
    bool     buzzer;     // enable buzzer with blink
    float    battery;    // battery voltage (sent in CMD_PONG)
};

// ─── Globals ─────────────────────────────────────────────────────────────────
RCSwitch rf433;

bool     blink433Active  = false;   // currently listening to 433 MHz?
uint32_t lastEspNowMs    = 0;       // last ESP-NOW message timestamp
bool     blinkActive     = false;   // currently executing a blink command
uint32_t blinkEndMs      = 0;
bool     blinkBuzzer     = false;
uint32_t blinkToggleMs   = 0;
bool     blinkState      = false;

bool     pairingMode     = false;
uint32_t pairBtnDownMs   = 0;
uint32_t pairedCode      = 0;       // stored 433 MHz code (0 = not paired)

uint8_t  broadcastAddr[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ─── Forward declarations ─────────────────────────────────────────────────────
void menuPrint();
int  readRotaryChannel();
float readBatteryVoltage();
void selfTest();
void startBlink(uint16_t durationMs, bool buzz);
void stopBlink();
void updateBlink();
void sendPong();
void enable433(bool enable);
void onEspNowRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);
void onEspNowSend(const wifi_tx_info_t *tx_info, esp_now_send_status_t status);

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Vilkur Test Sketch ===");

    // Output pins
    pinMode(PIN_LED_PAIR, OUTPUT);
    pinMode(PIN_BUZZER,   OUTPUT);
    pinMode(PIN_LED2,     OUTPUT);
    pinMode(PIN_LED1,     OUTPUT);
    digitalWrite(PIN_LED_PAIR, LOW);
    digitalWrite(PIN_BUZZER,   LOW);
    digitalWrite(PIN_LED2,     LOW);
    digitalWrite(PIN_LED1,     LOW);

    // Input pins
    pinMode(PIN_EXT_SIG,  INPUT);
    pinMode(PIN_PAIR_BTN, INPUT_PULLUP);
    pinMode(PIN_ROT_CH1,  INPUT_PULLUP);
    pinMode(PIN_ROT_CH2,  INPUT_PULLUP);
    pinMode(PIN_ROT_CH3,  INPUT_PULLUP);

    // ADC
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db); // full-scale ~3.3 V

    // 433 MHz – RCSwitch
    rf433.enableReceive(PIN_433_DATA);
    blink433Active = true;
    Serial.println("433 MHz: listening");

    // ESP-NOW
    int channel = readRotaryChannel();
    Serial.printf("Rotary switch: channel %d\n", channel);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ERROR: esp_now_init() failed");
    } else {
        esp_now_register_recv_cb(onEspNowRecv);
        esp_now_register_send_cb(onEspNowSend);

        // Register broadcast peer
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, broadcastAddr, 6);
        peer.channel = channel;
        peer.encrypt = false;
        esp_now_add_peer(&peer);

        Serial.printf("ESP-NOW ready on channel %d\n", channel);
        Serial.print("My MAC: ");
        Serial.println(WiFi.macAddress());
    }

    selfTest();
    menuPrint();
    lastEspNowMs = millis();
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    // ── Serial menu ─────────────────────────────────────────────────────────
    if (Serial.available()) {
        char c = Serial.read();
        while (Serial.available()) Serial.read(); // flush

        switch (c) {
            case '1':
                Serial.println(">> Test LED group 1 (3 blinks)");
                for (int i = 0; i < 3; i++) {
                    digitalWrite(PIN_LED1, HIGH); delay(300);
                    digitalWrite(PIN_LED1, LOW);  delay(200);
                }
                break;

            case '2':
                Serial.println(">> Test LED group 2 (3 blinks)");
                for (int i = 0; i < 3; i++) {
                    digitalWrite(PIN_LED2, HIGH); delay(300);
                    digitalWrite(PIN_LED2, LOW);  delay(200);
                }
                break;

            case '3':
                Serial.println(">> Test buzzer (500 ms)");
                digitalWrite(PIN_BUZZER, HIGH); delay(500);
                digitalWrite(PIN_BUZZER, LOW);
                break;

            case '4': {
                float v = readBatteryVoltage();
                Serial.printf(">> Battery voltage: %.2f V (ADC raw: %d)\n",
                              v, analogRead(PIN_BATT_ADC));
                break;
            }

            case '5': {
                int ch = readRotaryChannel();
                Serial.printf(">> Rotary switch: channel %d  "
                              "(GPIO10=%d GPIO20=%d GPIO21=%d)\n",
                              ch,
                              digitalRead(PIN_ROT_CH1),
                              digitalRead(PIN_ROT_CH2),
                              digitalRead(PIN_ROT_CH3));
                break;
            }

            case '6':
                Serial.printf(">> External signal pin (GPIO8): %s\n",
                              digitalRead(PIN_EXT_SIG) ? "HIGH (connected)" : "LOW");
                break;

            case '7':
                Serial.println(">> 433 MHz listening – trigger your remote (any key to stop)...");
                while (!Serial.available()) {
                    if (rf433.available()) {
                        Serial.printf("   433 RX: code=%lu bits=%d protocol=%d\n",
                                      rf433.getReceivedValue(),
                                      rf433.getReceivedBitlength(),
                                      rf433.getReceivedProtocol());
                        rf433.resetAvailable();
                    }
                    delay(50);
                }
                Serial.read(); // consume key
                break;

            case '8':
                Serial.print(">> My MAC address: ");
                Serial.println(WiFi.macAddress());
                break;

            default:
                menuPrint();
                break;
        }
        menuPrint();
    }

    // ── 433 MHz passive reception (when active) ──────────────────────────────
    if (blink433Active && rf433.available()) {
        unsigned long code = rf433.getReceivedValue();
        int bits           = rf433.getReceivedBitlength();
        rf433.resetAvailable();

        Serial.printf("[433] Received code=%lu bits=%d\n", code, bits);

        // If not paired yet, accept any code; otherwise check match
        if (pairedCode == 0 || code == (unsigned long)pairedCode) {
            Serial.println("[433] Triggering blink (fallback)");
            startBlink(3000, true);
        }
    }

    // ── Blink state machine ──────────────────────────────────────────────────
    updateBlink();

    // ── Pairing button (hold 3 s) ────────────────────────────────────────────
    if (!digitalRead(PIN_PAIR_BTN)) {               // button pressed (active LOW)
        if (pairBtnDownMs == 0) pairBtnDownMs = millis();

        if (!pairingMode && millis() - pairBtnDownMs >= 3000) {
            pairingMode = true;
            Serial.println("[PAIR] Pairing mode: waiting for 433 MHz code...");
        }
    } else {
        pairBtnDownMs = 0;
        if (pairingMode) {
            // Check for received code in pairing mode
            if (rf433.available()) {
                pairedCode = rf433.getReceivedValue();
                rf433.resetAvailable();
                pairingMode = false;
                Serial.printf("[PAIR] Paired code stored: %lu\n", pairedCode);
                // Confirm with 5 fast blinks on feedback LED
                for (int i = 0; i < 5; i++) {
                    digitalWrite(PIN_LED_PAIR, HIGH); delay(100);
                    digitalWrite(PIN_LED_PAIR, LOW);  delay(100);
                }
            } else {
                // Blink feedback LED while waiting
                digitalWrite(PIN_LED_PAIR, (millis() / 200) % 2);
            }
        }
    }

    // ── 433 MHz watchdog: re-enable after 60 s of ESP-NOW silence ───────────
    if (!blink433Active && millis() - lastEspNowMs >= 60000) {
        Serial.println("[433] Re-enabling 433 MHz (ESP-NOW silent for 60 s)");
        enable433(true);
    }
}

// ─── Helpers ─────────────────────────────────────────────────────────────────
void menuPrint() {
    Serial.println("\n--- Vilkur Test Menu ---");
    Serial.println("  1  Test LED group 1");
    Serial.println("  2  Test LED group 2");
    Serial.println("  3  Test piezo buzzer");
    Serial.println("  4  Read battery voltage");
    Serial.println("  5  Read rotary switch position");
    Serial.println("  6  Read external signal pin");
    Serial.println("  7  433 MHz listen (interactive)");
    Serial.println("  8  Print MAC address");
    Serial.println("Hold pairing button 3 s to enter 433 pairing mode");
    Serial.print  ("ESP-NOW listening for blink/ping commands.\n>");
}

int readRotaryChannel() {
    if (!digitalRead(PIN_ROT_CH1)) return 1;
    if (!digitalRead(PIN_ROT_CH2)) return 6;
    if (!digitalRead(PIN_ROT_CH3)) return 11;
    return 1; // default
}

float readBatteryVoltage() {
    // ADC reads ¼ of real battery voltage; reference ≈ 3.3 V, 12-bit
    int raw = analogRead(PIN_BATT_ADC);
    return raw * (3.3f / 4095.0f) * 4.0f;
}

void selfTest() {
    Serial.println("[SELF-TEST] LED1 → LED2 → Buzzer");
    digitalWrite(PIN_LED1, HIGH); delay(300); digitalWrite(PIN_LED1, LOW);
    delay(100);
    digitalWrite(PIN_LED2, HIGH); delay(300); digitalWrite(PIN_LED2, LOW);
    delay(100);
    digitalWrite(PIN_BUZZER, HIGH); delay(200); digitalWrite(PIN_BUZZER, LOW);
    Serial.println("[SELF-TEST] Done");
}

void startBlink(uint16_t durationMs, bool buzz) {
    blinkActive    = true;
    blinkEndMs     = millis() + durationMs;
    blinkBuzzer    = buzz;
    blinkToggleMs  = millis();
    blinkState     = true;
    digitalWrite(PIN_LED1, HIGH);
    digitalWrite(PIN_LED2, HIGH);
    if (buzz) digitalWrite(PIN_BUZZER, HIGH);
}

void stopBlink() {
    blinkActive = false;
    digitalWrite(PIN_LED1,    LOW);
    digitalWrite(PIN_LED2,    LOW);
    digitalWrite(PIN_BUZZER,  LOW);
}

void updateBlink() {
    if (!blinkActive) return;
    if (millis() >= blinkEndMs) {
        stopBlink();
        return;
    }
    if (millis() - blinkToggleMs >= 250) {  // 2 Hz blink
        blinkToggleMs = millis();
        blinkState = !blinkState;
        digitalWrite(PIN_LED1,   blinkState ? HIGH : LOW);
        digitalWrite(PIN_LED2,   blinkState ? HIGH : LOW);
        if (blinkBuzzer)
            digitalWrite(PIN_BUZZER, blinkState ? HIGH : LOW);
    }
}

void enable433(bool enable) {
    blink433Active = enable;
    if (enable) {
        rf433.enableReceive(PIN_433_DATA);
    } else {
        rf433.disableReceive();
    }
    Serial.printf("[433] %s\n", enable ? "Enabled" : "Disabled");
}

void sendPong() {
    EspNowMsg msg = {};
    msg.cmd     = CMD_PONG;
    msg.battery = readBatteryVoltage();
    esp_now_send(broadcastAddr, (uint8_t*)&msg, sizeof(msg));
    Serial.printf("[ESP-NOW] Sent PONG, battery=%.2f V\n", msg.battery);
}

// ─── ESP-NOW callbacks ────────────────────────────────────────────────────────
void onEspNowRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    lastEspNowMs = millis();

    if (len < (int)sizeof(EspNowMsg)) return;
    const EspNowMsg *msg = (const EspNowMsg*)data;
    const uint8_t *mac = recv_info->src_addr;

    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("[ESP-NOW] RX from %s cmd=0x%02X\n", macStr, msg->cmd);

    switch (msg->cmd) {
        case CMD_BLINK:
            Serial.printf("  → BLINK duration=%u ms buzzer=%s\n",
                          msg->duration, msg->buzzer ? "yes" : "no");
            startBlink(msg->duration, msg->buzzer);
            break;

        case CMD_STOP:
            Serial.println("  → STOP");
            stopBlink();
            break;

        case CMD_DISABLE_433:
            Serial.println("  → DISABLE_433");
            enable433(false);
            break;

        case CMD_PING:
            Serial.println("  → PING → sending PONG");
            sendPong();
            break;

        default:
            Serial.printf("  → Unknown cmd 0x%02X\n", msg->cmd);
            break;
    }
}

void onEspNowSend(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
    Serial.printf("[ESP-NOW] TX %s\n",
                  status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}
