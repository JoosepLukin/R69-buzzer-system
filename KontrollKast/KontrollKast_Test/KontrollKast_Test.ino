/*
 * KontrollKast_Test.ino — Hardware test sketch for KontrollKast PCB (ESP32-S3)
 *
 * Tests:
 *   - ST7796 320×480 SPI display (TFT_eSPI library)
 *   - FT6336U capacitive touch (I2C, no extra library needed)
 *   - 3× illuminated push-buttons + backlight MOSFETs
 *   - 433 MHz reception (RXB6 on GPIO 1, RCSwitch library)
 *   - RS-485 half-duplex (SN65HVD72, GPIO 4/5/6, ArduinoJson)
 *   - On-screen touch buttons: Blink All, Stop All, Ping All, 433 Pair
 *
 * Serial monitor: 115200 baud
 * Board: ESP32S3 Dev Module
 *
 * Required libraries:
 *   - TFT_eSPI  (configure User_Setup.h — see KontrollKast/README.md)
 *   - ArduinoJson v6
 *   - RCSwitch (by sui77)
 *
 * TFT_eSPI User_Setup.h key defines:
 *   #define ST7796_DRIVER
 *   #define TFT_CS 15, TFT_DC 17, TFT_RST 16
 *   #define TFT_MOSI 18, TFT_SCLK 8, TFT_MISO 9, TFT_BL 3
 *   #define SPI_FREQUENCY 27000000
 */

#include <Wire.h>
#include <TFT_eSPI.h>
#include <RCSwitch.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>

// ─── Pin definitions ─────────────────────────────────────────────────────────
#define PIN_433_DATA    1   // RXB6 data
#define PIN_LCD_BL      3   // Backlight PWM (also defined in TFT_eSPI User_Setup.h)
#define PIN_RS485_MODE  4   // DE/RE: HIGH=TX, LOW=RX
#define PIN_RS485_TX    5
#define PIN_RS485_RX    6
// LCD SPI pins handled by TFT_eSPI via User_Setup.h
#define PIN_TOUCH_SCL  10
#define PIN_TOUCH_RST  11
#define PIN_TOUCH_SDA  12
#define PIN_TOUCH_INT  13
#define PIN_BTN1_BL    21   // Button 1 backlight MOSFET
#define PIN_BTN2_BL    35   // Button 2 backlight MOSFET
#define PIN_BTN2_IN    36   // Button 2 input (active LOW)
#define PIN_BTN3_BL    39   // Button 3 backlight MOSFET
#define PIN_BTN3_IN    40   // Button 3 input (active LOW)
#define PIN_BTN1_IN    48   // Button 1 input (active LOW)

// ─── FT6336U touch (I2C address 0x38) ────────────────────────────────────────
#define FT6336U_ADDR  0x38

// ─── RS-485 ──────────────────────────────────────────────────────────────────
HardwareSerial RS485(1);    // UART1
#define RS485_BAUD  115200

// ─── Display ─────────────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();
#define TFT_W 320
#define TFT_H 480

// Colour palette
#define C_BG      TFT_BLACK
#define C_HEADER  0x1A3A      // dark blue-green
#define C_TEXT    TFT_WHITE
#define C_GREEN   TFT_GREEN
#define C_RED     TFT_RED
#define C_YELLOW  TFT_YELLOW
#define C_ORANGE  0xFD00      // orange
#define C_GRAY    0x7BEF

// ─── 433 MHz ─────────────────────────────────────────────────────────────────
RCSwitch rf433;
bool pairingMode433 = false;

// ─── Button state ─────────────────────────────────────────────────────────────
bool btnState[3]  = {false, false, false};
bool btnBl[3]     = {false, false, false};
const uint8_t BTN_IN[3]  = {PIN_BTN1_IN,  PIN_BTN2_IN,  PIN_BTN3_IN};
const uint8_t BTN_BL[3]  = {PIN_BTN1_BL,  PIN_BTN2_BL,  PIN_BTN3_BL};

// ─── On-screen button layout ──────────────────────────────────────────────────
struct ScreenBtn {
    int16_t x, y, w, h;
    const char *label;
    uint16_t color;
};

static const ScreenBtn screenBtns[] = {
    {  10, 310, 140, 50, "Blink All", C_ORANGE },
    { 170, 310, 140, 50, "Stop All",  C_RED    },
    {  10, 370, 140, 50, "Ping All",  C_GREEN  },
    { 170, 370, 140, 50, "433 Pair",  C_YELLOW },
};
static const int NUM_SCREEN_BTNS = 4;

// ─── Status log (last 6 lines) ─────────────────────────────────────────────────
#define LOG_LINES 6
String logLines[LOG_LINES];
int    logHead = 0;

void logAdd(const String &s) {
    logLines[logHead] = s;
    logHead = (logHead + 1) % LOG_LINES;
    Serial.println(s);
    redrawLog();
}

// ─── Forward declarations ─────────────────────────────────────────────────────
void touchBegin();
bool touchRead(int16_t &x, int16_t &y);
void drawUI();
void redrawLog();
void drawScreenBtns();
void redrawBtnStatus();
void rs485Send(const String &json);
void handleRS485();
void handleScreenTouch(int16_t x, int16_t y);
void sendBlinkAll();
void sendStopAll();
void sendPingAll();
void enter433Pairing();

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== KontrollKast Test Sketch ===");

    // ── Physical buttons & backlights ────────────────────────────────────────
    for (int i = 0; i < 3; i++) {
        pinMode(BTN_IN[i], INPUT_PULLUP);
        pinMode(BTN_BL[i], OUTPUT);
        digitalWrite(BTN_BL[i], LOW);
    }

    // ── RS-485 ───────────────────────────────────────────────────────────────
    pinMode(PIN_RS485_MODE, OUTPUT);
    digitalWrite(PIN_RS485_MODE, LOW);  // receive mode
    RS485.begin(RS485_BAUD, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);
    Serial.println("RS-485 ready");

    // ── 433 MHz ──────────────────────────────────────────────────────────────
    rf433.enableReceive(PIN_433_DATA);
    Serial.println("433 MHz ready");

    // ── Display ──────────────────────────────────────────────────────────────
    tft.init();
    tft.setRotation(0);   // portrait, USB at bottom — adjust if needed
    tft.fillScreen(C_BG);
    ledcSetup(0, 5000, 8);
    ledcAttachPin(PIN_LCD_BL, 0);
    ledcWrite(0, 200);    // ~78% brightness
    Serial.println("Display ready");

    // ── Touch ─────────────────────────────────────────────────────────────────
    touchBegin();
    Serial.println("Touch ready");

    // ── Draw UI ───────────────────────────────────────────────────────────────
    drawUI();
    logAdd("System started");

    // ── Send initial ping broadcast ───────────────────────────────────────────
    delay(500);
    sendPingAll();
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    // ── RS-485 incoming ──────────────────────────────────────────────────────
    handleRS485();

    // ── 433 MHz reception ────────────────────────────────────────────────────
    if (rf433.available()) {
        unsigned long code = rf433.getReceivedValue();
        int bits           = rf433.getReceivedBitlength();
        rf433.resetAvailable();

        String msg = "433RX code=" + String(code) + " bits=" + String(bits);
        logAdd(msg);

        if (pairingMode433) {
            // Store/send paired code
            logAdd("Paired: " + String(code));
            pairingMode433 = false;
            // Turn off LED indicator
            for (int i = 0; i < 3; i++) {
                digitalWrite(BTN_BL[i], btnBl[i] ? HIGH : LOW);
            }
        }
    }

    // ── Physical button handling ─────────────────────────────────────────────
    for (int i = 0; i < 3; i++) {
        bool pressed = !digitalRead(BTN_IN[i]);  // active LOW
        if (pressed && !btnState[i]) {
            btnState[i] = true;
            btnBl[i]    = !btnBl[i];
            digitalWrite(BTN_BL[i], btnBl[i] ? HIGH : LOW);
            String msg = "Button " + String(i+1) + (btnBl[i] ? " ON" : " OFF");
            logAdd(msg);
            redrawBtnStatus();

            // Button 1 → blink all, Button 2 → stop all, Button 3 → ping all
            if (i == 0) sendBlinkAll();
            if (i == 1) sendStopAll();
            if (i == 2) sendPingAll();
        }
        if (!pressed) btnState[i] = false;
    }

    // ── Touch handling ───────────────────────────────────────────────────────
    int16_t tx, ty;
    if (touchRead(tx, ty)) {
        // Update live touch coords on screen
        tft.fillRect(10, 270, 300, 30, C_BG);
        tft.setTextColor(C_GRAY, C_BG);
        tft.setTextSize(1);
        tft.setCursor(10, 275);
        tft.printf("Touch: X=%3d Y=%3d", tx, ty);

        handleScreenTouch(tx, ty);
        delay(80);  // simple debounce
    }

    // ── Periodic ping every 5 s ───────────────────────────────────────────────
    static uint32_t lastPingMs = 0;
    if (millis() - lastPingMs >= 5000) {
        lastPingMs = millis();
        sendPingAll();
    }
}

// ─── Touch (FT6336U, direct I2C) ─────────────────────────────────────────────
void touchBegin() {
    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    // Hardware reset
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);  delay(10);
    digitalWrite(PIN_TOUCH_RST, HIGH); delay(100);
    pinMode(PIN_TOUCH_INT, INPUT);
}

bool touchRead(int16_t &x, int16_t &y) {
    // Read 5 bytes starting at register 0x02 (TD_STATUS)
    Wire.beginTransmission(FT6336U_ADDR);
    Wire.write(0x02);
    Wire.endTransmission(false);
    Wire.requestFrom(FT6336U_ADDR, 5);

    if (Wire.available() < 5) return false;
    uint8_t touches = Wire.read();
    uint8_t xh      = Wire.read();
    uint8_t xl      = Wire.read();
    uint8_t yh      = Wire.read();
    uint8_t yl      = Wire.read();

    if ((touches & 0x0F) == 0) return false;

    x = (int16_t)(((xh & 0x0F) << 8) | xl);
    y = (int16_t)(((yh & 0x0F) << 8) | yl);
    return true;
}

// ─── Display helpers ──────────────────────────────────────────────────────────
void drawUI() {
    tft.fillScreen(C_BG);

    // Header
    tft.fillRect(0, 0, TFT_W, 45, C_HEADER);
    tft.setTextColor(C_TEXT, C_HEADER);
    tft.setTextSize(2);
    tft.setCursor(10, 12);
    tft.print("KontrollKast TEST");

    // Section labels
    tft.setTextColor(C_GRAY, C_BG);
    tft.setTextSize(1);
    tft.setCursor(10, 52);
    tft.print("Physical buttons:");

    tft.setCursor(10, 140);
    tft.print("RS-485 / ESP-NOW log:");

    tft.setCursor(10, 265);
    tft.print("Touch coordinates:");

    tft.setCursor(10, 295);
    tft.print("Commands:");

    redrawBtnStatus();
    drawScreenBtns();
    redrawLog();
}

void redrawBtnStatus() {
    for (int i = 0; i < 3; i++) {
        int xo = 10 + i * 105;
        tft.fillRect(xo, 62, 95, 70, btnBl[i] ? C_GREEN : 0x2104);
        tft.setTextColor(C_TEXT);
        tft.setTextSize(2);
        tft.setCursor(xo + 8, 72);
        tft.printf("BTN%d", i+1);
        tft.setTextSize(1);
        tft.setCursor(xo + 8, 100);
        tft.print(btnBl[i] ? "BL: ON " : "BL: OFF");
        tft.setCursor(xo + 8, 115);
        tft.print(btnState[i] ? "PRESSED" : "       ");
    }
}

void drawScreenBtns() {
    for (int i = 0; i < NUM_SCREEN_BTNS; i++) {
        const ScreenBtn &b = screenBtns[i];
        tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, b.color);
        tft.setTextColor(TFT_BLACK);
        tft.setTextSize(1);
        // Centre text
        int tw = strlen(b.label) * 6;
        tft.setCursor(b.x + (b.w - tw)/2, b.y + (b.h - 8)/2);
        tft.print(b.label);
    }
}

void redrawLog() {
    int y = 150;
    for (int i = 0; i < LOG_LINES; i++) {
        int idx = (logHead + i) % LOG_LINES;
        tft.fillRect(0, y, TFT_W, 16, C_BG);
        tft.setTextColor(C_TEXT, C_BG);
        tft.setTextSize(1);
        tft.setCursor(10, y + 2);
        if (logLines[idx].length() > 0) {
            // Truncate to fit display width
            String s = logLines[idx];
            if (s.length() > 51) s = s.substring(0, 51);
            tft.print(s);
        }
        y += 18;
    }
}

// ─── Screen touch handler ─────────────────────────────────────────────────────
void handleScreenTouch(int16_t x, int16_t y) {
    for (int i = 0; i < NUM_SCREEN_BTNS; i++) {
        const ScreenBtn &b = screenBtns[i];
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
            logAdd(String("Touch: ") + b.label);
            if (i == 0) sendBlinkAll();
            if (i == 1) sendStopAll();
            if (i == 2) sendPingAll();
            if (i == 3) enter433Pairing();
            break;
        }
    }
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

// ─── RS-485 RX ────────────────────────────────────────────────────────────────
void handleRS485() {
    static String rxBuf;
    while (RS485.available()) {
        char c = RS485.read();
        if (c == '\n') {
            if (rxBuf.length() > 0) {
                Serial.print("[RS485 RX] "); Serial.println(rxBuf);

                StaticJsonDocument<256> doc;
                DeserializationError err = deserializeJson(doc, rxBuf);
                if (!err) {
                    const char *type = doc["type"] | "";

                    if (strcmp(type, "ping_reply") == 0) {
                        String mac  = doc["mac"] | "??:??:??:??:??:??";
                        float  batt = doc["battery"] | 0.0f;
                        logAdd("Pong " + mac.substring(9) + " " + String(batt,1)+"V");
                    } else if (strcmp(type, "433rx") == 0) {
                        unsigned long code = doc["code"] | 0UL;
                        int bits = doc["bits"] | 0;
                        logAdd("SV 433: " + String(code) + " b=" + String(bits));
                    } else if (strcmp(type, "espnow_rx") == 0) {
                        const char *mac = doc["mac"] | "?";
                        const char *cmd = doc["cmd"] | "?";
                        logAdd(String("RX ") + String(mac).substring(9) + " " + cmd);
                    } else {
                        logAdd(String("RS485: ") + rxBuf.substring(0, 40));
                    }
                } else {
                    logAdd("RS485 parse err");
                }
                rxBuf = "";
            }
        } else {
            if (rxBuf.length() < 255) rxBuf += c;
        }
    }
}

// ─── Command senders ──────────────────────────────────────────────────────────
void sendBlinkAll() {
    String json = "{\"type\":\"cmd\",\"mac\":\"broadcast\","
                  "\"action\":\"blink\",\"duration\":5000,\"buzzer\":true}";
    rs485Send(json);
    logAdd(">> Blink All sent");
    // Light up all button backlights to indicate blinking
    for (int i = 0; i < 3; i++) {
        btnBl[i] = true;
        digitalWrite(BTN_BL[i], HIGH);
    }
    redrawBtnStatus();
}

void sendStopAll() {
    String json = "{\"type\":\"cmd\",\"mac\":\"broadcast\",\"action\":\"stop\"}";
    rs485Send(json);
    logAdd(">> Stop All sent");
    for (int i = 0; i < 3; i++) {
        btnBl[i] = false;
        digitalWrite(BTN_BL[i], LOW);
    }
    redrawBtnStatus();
}

void sendPingAll() {
    String json = "{\"type\":\"ping\",\"mac\":\"broadcast\"}";
    rs485Send(json);
    logAdd(">> Ping All");
}

void enter433Pairing() {
    pairingMode433 = true;
    logAdd("433 Pair: waiting for remote...");
    // Flash all backlights to indicate pairing mode
    for (int i = 0; i < 3; i++) {
        digitalWrite(BTN_BL[i], HIGH);
    }
    delay(200);
    for (int i = 0; i < 3; i++) {
        digitalWrite(BTN_BL[i], LOW);
    }
}
