/*
 * SaatjaVastuvotja_Test.ino — Hardware test sketch for SaatjaVastuvõtja PCB (ESP32-C3 Supermini)
 *
 * (Folder/file name uses ASCII to avoid build-system encoding issues)
 *
 * Tests:
 *   - 433 MHz reception (RXB6 on GPIO 4) → relay to KontrollKast over RS-485
 *   - RS-485 half-duplex (SN65HVD72, GPIO 1/2/3) → parse JSON commands
 *   - ESP-NOW TX to Vilkur (broadcast) based on RS-485 commands
 *   - ESP-NOW RX from Vilkur → relay over RS-485 to KontrollKast
 *   - Serial mirror of all RS-485 traffic at 115200 baud
 *
 * Serial monitor: 115200 baud
 * Board: ESP32C3 Dev Module
 *
 * Required libraries:
 *   - RCSwitch (by sui77)
 *   - ArduinoJson v6
 */

#include <WiFi.h>
#include <esp_now.h>
#include <RCSwitch.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>

// ─── Pin definitions ─────────────────────────────────────────────────────────
#define PIN_RS485_MODE  1   // DE/RE: HIGH=TX, LOW=RX
#define PIN_RS485_TX    2
#define PIN_RS485_RX    3
#define PIN_433_DATA    4   // RXB6 data

// ─── ESP-NOW message struct (must match Vilkur) ───────────────────────────────
#define CMD_BLINK       0x01
#define CMD_STOP        0x02
#define CMD_DISABLE_433 0x03
#define CMD_PING        0x04
#define CMD_PONG        0x05

struct EspNowMsg {
    uint8_t  cmd;
    uint16_t duration;
    bool     buzzer;
    float    battery;
};

// ─── RS-485 ──────────────────────────────────────────────────────────────────
HardwareSerial RS485(1);   // UART1
#define RS485_BAUD 115200

// ─── 433 MHz ─────────────────────────────────────────────────────────────────
RCSwitch rf433;
bool pairingMode = false;

// ─── ESP-NOW ─────────────────────────────────────────────────────────────────
uint8_t broadcastAddr[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// Pending ping: wait for pong from a specific Vilkur
#define PING_TIMEOUT_MS 2000
struct PendingPing {
    bool     active;
    uint8_t  mac[6];        // 0xFF*6 = broadcast (wait for any)
    uint32_t sentMs;
};
PendingPing pendingPing = {};

// ─── Helpers ─────────────────────────────────────────────────────────────────
void macToStr(const uint8_t *mac, char *buf) {
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool strToMac(const char *s, uint8_t *mac) {
    int v[6];
    if (sscanf(s, "%02X:%02X:%02X:%02X:%02X:%02X",
               &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
    return true;
}

bool isBroadcast(const uint8_t *mac) {
    for (int i = 0; i < 6; i++) if (mac[i] != 0xFF) return false;
    return true;
}

// ─── RS-485 TX ────────────────────────────────────────────────────────────────
void rs485Send(const String &json) {
    digitalWrite(PIN_RS485_MODE, HIGH);
    delayMicroseconds(100);
    RS485.println(json);
    RS485.flush();
    delayMicroseconds(100);
    digitalWrite(PIN_RS485_MODE, LOW);
    Serial.print("[RS485 TX] "); Serial.println(json);
}

// ─── ESP-NOW TX ───────────────────────────────────────────────────────────────
void espnowSend(const uint8_t *mac, const EspNowMsg &msg) {
    // Add peer if needed (ignore error if already added)
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;    // use current channel
    peer.encrypt = false;
    esp_now_add_peer(&peer);  // ignore duplicate error

    esp_err_t r = esp_now_send(mac, (const uint8_t*)&msg, sizeof(msg));
    char macStr[18]; macToStr(mac, macStr);
    Serial.printf("[ESP-NOW TX] to=%s cmd=0x%02X result=%s\n",
                  macStr, msg.cmd, r == ESP_OK ? "OK" : "ERR");
}

// ─── ESP-NOW callbacks ────────────────────────────────────────────────────────
void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
    if (len < (int)sizeof(EspNowMsg)) return;
    const EspNowMsg *msg = (const EspNowMsg*)data;

    char macStr[18]; macToStr(mac, macStr);
    Serial.printf("[ESP-NOW RX] from=%s cmd=0x%02X battery=%.2f\n",
                  macStr, msg->cmd, msg->battery);

    // Build JSON response for KontrollKast
    StaticJsonDocument<200> doc;

    if (msg->cmd == CMD_PONG) {
        // Check if this resolves a pending ping
        if (pendingPing.active) {
            if (isBroadcast(pendingPing.mac) ||
                memcmp(pendingPing.mac, mac, 6) == 0) {
                pendingPing.active = false;
            }
        }
        doc["type"]    = "ping_reply";
        doc["mac"]     = macStr;
        doc["battery"] = serialized(String(msg->battery, 2));
    } else {
        // Generic relay
        const char *cmdName = "unknown";
        if (msg->cmd == CMD_BLINK)       cmdName = "blink";
        else if (msg->cmd == CMD_STOP)   cmdName = "stop";
        else if (msg->cmd == CMD_PING)   cmdName = "ping";

        doc["type"]    = "espnow_rx";
        doc["mac"]     = macStr;
        doc["cmd"]     = cmdName;
        doc["battery"] = serialized(String(msg->battery, 2));
    }

    String out;
    serializeJson(doc, out);
    rs485Send(out);
}

void onEspNowSend(const uint8_t *mac, esp_now_send_status_t status) {
    char macStr[18]; macToStr(mac, macStr);
    Serial.printf("[ESP-NOW TX] ack from=%s %s\n",
                  macStr, status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");

    // Send ack back to KontrollKast
    StaticJsonDocument<150> doc;
    doc["type"]    = "espnow_ack";
    doc["mac"]     = macStr;
    doc["success"] = (status == ESP_NOW_SEND_SUCCESS);
    String out;
    serializeJson(doc, out);
    rs485Send(out);
}

// ─── RS-485 command handler ───────────────────────────────────────────────────
void handleRS485Command(const String &line) {
    Serial.print("[RS485 RX] "); Serial.println(line);

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, line);
    if (err) {
        Serial.println("  JSON parse error");
        return;
    }

    const char *type = doc["type"] | "";

    // ── cmd ──────────────────────────────────────────────────────────────────
    if (strcmp(type, "cmd") == 0) {
        const char *macStr = doc["mac"] | "broadcast";
        const char *action = doc["action"] | "";

        uint8_t targetMac[6];
        bool    broadcast = (strcmp(macStr, "broadcast") == 0);
        if (broadcast) {
            memcpy(targetMac, broadcastAddr, 6);
        } else {
            if (!strToMac(macStr, targetMac)) {
                Serial.println("  Invalid MAC");
                return;
            }
        }

        EspNowMsg msg = {};

        if (strcmp(action, "blink") == 0) {
            msg.cmd      = CMD_BLINK;
            msg.duration = doc["duration"] | 3000;
            msg.buzzer   = doc["buzzer"]   | true;
            Serial.printf("  → BLINK to %s dur=%u buzz=%d\n",
                          macStr, msg.duration, (int)msg.buzzer);
            espnowSend(targetMac, msg);

        } else if (strcmp(action, "stop") == 0) {
            msg.cmd = CMD_STOP;
            Serial.printf("  → STOP to %s\n", macStr);
            espnowSend(targetMac, msg);

        } else if (strcmp(action, "disable_433") == 0) {
            msg.cmd = CMD_DISABLE_433;
            Serial.printf("  → DISABLE_433 to %s\n", macStr);
            espnowSend(targetMac, msg);

        } else {
            Serial.printf("  Unknown action: %s\n", action);
        }
    }

    // ── ping ─────────────────────────────────────────────────────────────────
    else if (strcmp(type, "ping") == 0) {
        const char *macStr = doc["mac"] | "broadcast";
        bool broadcast = (strcmp(macStr, "broadcast") == 0);

        uint8_t targetMac[6];
        if (broadcast) {
            memcpy(targetMac, broadcastAddr, 6);
        } else {
            if (!strToMac(macStr, targetMac)) {
                Serial.println("  Invalid MAC");
                return;
            }
        }

        EspNowMsg msg = {};
        msg.cmd = CMD_PING;
        Serial.printf("  → PING to %s\n", macStr);
        espnowSend(targetMac, msg);

        // Record pending ping
        pendingPing.active = true;
        memcpy(pendingPing.mac, targetMac, 6);
        pendingPing.sentMs = millis();
    }

    // ── pair_enable ───────────────────────────────────────────────────────────
    else if (strcmp(type, "pair_enable") == 0) {
        pairingMode = true;
        Serial.println("  → 433 MHz pairing mode enabled");
    }

    else {
        Serial.printf("  Unknown type: %s\n", type);
    }
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println("\n=== SaatjaVastuvotja Test Sketch ===");

    // ── RS-485 ───────────────────────────────────────────────────────────────
    pinMode(PIN_RS485_MODE, OUTPUT);
    digitalWrite(PIN_RS485_MODE, LOW);
    RS485.begin(RS485_BAUD, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);
    Serial.println("RS-485 ready (RX=GPIO3 TX=GPIO2 MODE=GPIO1)");

    // ── 433 MHz ──────────────────────────────────────────────────────────────
    rf433.enableReceive(PIN_433_DATA);
    Serial.println("433 MHz ready (GPIO4)");

    // ── ESP-NOW ──────────────────────────────────────────────────────────────
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    // Use channel 1 by default; should match Vilkur rotary switch
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ERROR: esp_now_init() failed");
    } else {
        esp_now_register_recv_cb(onEspNowRecv);
        esp_now_register_send_cb(onEspNowSend);

        // Pre-register broadcast peer
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, broadcastAddr, 6);
        peer.channel = 0;
        peer.encrypt = false;
        esp_now_add_peer(&peer);

        Serial.print("ESP-NOW ready. My MAC: ");
        Serial.println(WiFi.macAddress());
    }

    Serial.println("Waiting for RS-485 commands / 433 MHz signals...");
    Serial.println("All RS-485 traffic is mirrored to this Serial port.\n");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    // ── RS-485 incoming ──────────────────────────────────────────────────────
    static String rs485Buf;
    while (RS485.available()) {
        char c = RS485.read();
        if (c == '\n') {
            rs485Buf.trim();
            if (rs485Buf.length() > 0) {
                handleRS485Command(rs485Buf);
            }
            rs485Buf = "";
        } else {
            if (rs485Buf.length() < 255) rs485Buf += c;
        }
    }

    // ── 433 MHz ──────────────────────────────────────────────────────────────
    if (rf433.available()) {
        unsigned long code = rf433.getReceivedValue();
        int bits           = rf433.getReceivedBitlength();
        int protocol       = rf433.getReceivedProtocol();
        rf433.resetAvailable();

        Serial.printf("[433] code=%lu bits=%d protocol=%d\n", code, bits, protocol);

        if (pairingMode) {
            // Send pairing code back to KontrollKast and exit pairing mode
            StaticJsonDocument<100> doc;
            doc["type"]     = "pair_code";
            doc["code"]     = (unsigned long)code;
            doc["bits"]     = bits;
            doc["protocol"] = protocol;
            String out;
            serializeJson(doc, out);
            rs485Send(out);
            pairingMode = false;
            Serial.println("[433] Pairing done");
        } else {
            // Relay all received codes to KontrollKast
            StaticJsonDocument<100> doc;
            doc["type"] = "433rx";
            doc["code"] = (unsigned long)code;
            doc["bits"] = bits;
            String out;
            serializeJson(doc, out);
            rs485Send(out);
        }
    }

    // ── Pending ping timeout ──────────────────────────────────────────────────
    if (pendingPing.active && millis() - pendingPing.sentMs >= PING_TIMEOUT_MS) {
        char macStr[18]; macToStr(pendingPing.mac, macStr);
        Serial.printf("[PING] Timeout for %s\n", macStr);

        StaticJsonDocument<150> doc;
        doc["type"]    = "ping_timeout";
        doc["mac"]     = macStr;
        String out;
        serializeJson(doc, out);
        rs485Send(out);

        pendingPing.active = false;
    }
}
