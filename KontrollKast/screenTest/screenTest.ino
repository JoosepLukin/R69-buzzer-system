/*
 * screenTest.ino — Touchscreen paint app for KontrollKast PCB (ESP32-S3)
 *
 * Tests:
 *   - ST7796 320×480 SPI display (Arduino_GFX_Library)
 *   - FT6336U capacitive touch (I2C, Adafruit_FT6206 compatible)
 *
 * Usage:
 *   Draw on the canvas with your finger.
 *   Select colours from the right-side palette.
 *   Tap CLR to clear the canvas.
 *
 * Serial monitor: 115200 baud — prints raw and mapped touch coords.
 *
 * Required libraries:
 *   - Arduino_GFX_Library (moononournation)
 *   - Adafruit FT6206 (works with FT6336U — same I2C protocol)
 *
 * Board: ESP32S3 Dev Module
 */

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <Adafruit_FT6206.h>

// ─── Pin definitions (KontrollKast PCB) ──────────────────────────────────────
#define TFT_MOSI   18
#define TFT_MISO    9
#define TFT_SCLK    8
#define TFT_CS     15
#define TFT_DC     17
#define TFT_RST    16
#define TFT_BL      3

#define TOUCH_SDA  12
#define TOUCH_SCL  10
#define TOUCH_RST  11
#define TOUCH_INT  13

// ─── RGB565 colors ────────────────────────────────────────────────────────────
#define COLOR_BLACK     0x0000
#define COLOR_WHITE     0xFFFF
#define COLOR_RED       0xF800
#define COLOR_GREEN     0x07E0
#define COLOR_BLUE      0x001F
#define COLOR_YELLOW    0xFFE0
#define COLOR_CYAN      0x07FF
#define COLOR_MAGENTA   0xF81F
#define COLOR_ORANGE    0xFD20
#define COLOR_LIGHTGREY 0xC618
#define COLOR_DARKGREY  0x7BEF

// ─── Display setup ────────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO
);

Arduino_GFX *gfx = new Arduino_ST7796(
    bus,
    TFT_RST,
    0,      // native rotation for init
    true,   // IPS
    320,    // native width
    480     // native height
);

// ─── Touch setup ─────────────────────────────────────────────────────────────
Adafruit_FT6206 ctp = Adafruit_FT6206();

// ─── UI layout ────────────────────────────────────────────────────────────────
// Display is landscape (rotation 1): 480×320
const int SCREEN_W = 480;
const int SCREEN_H = 320;

const int PALETTE_W = 70;
const int DRAW_W = SCREEN_W - PALETTE_W;   // 410 px wide canvas

const int COLOR_BOX_SIZE = 30;
const int COLOR_BOX_X = DRAW_W + 20;

const int CLEAR_BTN_X = DRAW_W + 8;
const int CLEAR_BTN_Y = 270;
const int CLEAR_BTN_W = 54;
const int CLEAR_BTN_H = 36;

// ─── Drawing colors ───────────────────────────────────────────────────────────
const uint16_t palette[] = {
    COLOR_BLACK,
    COLOR_RED,
    COLOR_GREEN,
    COLOR_BLUE,
    COLOR_YELLOW,
    COLOR_CYAN,
    COLOR_MAGENTA,
    COLOR_ORANGE,
};
const int paletteCount = sizeof(palette) / sizeof(palette[0]);

int      selectedColorIndex = 0;
uint16_t currentColor       = COLOR_BLACK;

bool wasTouching = false;
int  lastX = -1;
int  lastY = -1;

// ─── Touch coordinate mapping ─────────────────────────────────────────────────
// Calibrate these values by reading Serial with the touch_calibrate sketch or
// by noting the raw values printed at each screen corner.
// Defaults below are a reasonable starting point for the FT6336U on this panel.
// Adjust TX_MAX, TX_MIN, TY_MIN, TY_MAX to match your measured values.
#define TX_MAX  319     // raw tx when finger is at top of landscape screen
#define TX_MIN    0     // raw tx when finger is at bottom
#define TY_MIN    0     // raw ty when finger is at left
#define TY_MAX  475     // raw ty when finger is at right

int mapTouchX(int tx, int ty) {
    int x = map(ty, TY_MIN, TY_MAX, 0, SCREEN_W - 1);
    return constrain(x, 0, SCREEN_W - 1);
}

int mapTouchY(int tx, int ty) {
    int y = map(tx, TX_MAX, TX_MIN, 0, SCREEN_H - 1);
    return constrain(y, 0, SCREEN_H - 1);
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
bool pointInRect(int x, int y, int rx, int ry, int rw, int rh) {
    return (x >= rx && x < rx + rw && y >= ry && y < ry + rh);
}

void drawPalette() {
    gfx->fillRect(DRAW_W, 0, PALETTE_W, SCREEN_H, COLOR_LIGHTGREY);
    gfx->drawLine(DRAW_W, 0, DRAW_W, SCREEN_H, COLOR_BLACK);

    for (int i = 0; i < paletteCount; i++) {
        int y = 10 + i * (COLOR_BOX_SIZE + 6);
        gfx->fillRect(COLOR_BOX_X, y, COLOR_BOX_SIZE, COLOR_BOX_SIZE, palette[i]);
        gfx->drawRect(COLOR_BOX_X, y, COLOR_BOX_SIZE, COLOR_BOX_SIZE, COLOR_BLACK);

        if (i == selectedColorIndex) {
            gfx->drawRect(COLOR_BOX_X - 2, y - 2, COLOR_BOX_SIZE + 4, COLOR_BOX_SIZE + 4, COLOR_BLACK);
            gfx->drawRect(COLOR_BOX_X - 3, y - 3, COLOR_BOX_SIZE + 6, COLOR_BOX_SIZE + 6, COLOR_BLACK);
        }
    }

    gfx->fillRect(CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H, COLOR_DARKGREY);
    gfx->drawRect(CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H, COLOR_BLACK);
    gfx->setTextColor(COLOR_WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(CLEAR_BTN_X + 6, CLEAR_BTN_Y + 10);
    gfx->print("CLR");
}

void clearCanvas() {
    gfx->fillRect(0, 0, DRAW_W, SCREEN_H, COLOR_WHITE);
    drawPalette();
}

void selectColor(int index) {
    if (index < 0 || index >= paletteCount) return;
    selectedColorIndex = index;
    currentColor = palette[index];
    drawPalette();
}

void handleUiTap(int x, int y) {
    for (int i = 0; i < paletteCount; i++) {
        int boxY = 10 + i * (COLOR_BOX_SIZE + 6);
        if (pointInRect(x, y, COLOR_BOX_X, boxY, COLOR_BOX_SIZE, COLOR_BOX_SIZE)) {
            selectColor(i);
            return;
        }
    }
    if (pointInRect(x, y, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H)) {
        clearCanvas();
    }
}

void drawOnCanvas(int x, int y) {
    if (x < 0 || x >= DRAW_W || y < 0 || y >= SCREEN_H) return;

    if (!wasTouching || lastX < 0 || lastY < 0) {
        gfx->fillCircle(x, y, 2, currentColor);
    } else {
        gfx->drawLine(lastX, lastY, x, y, currentColor);
        gfx->fillCircle(x, y, 2, currentColor);
    }
    lastX = x;
    lastY = y;
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("=== KontrollKast Screen Test ===");

    // Backlight on
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    // Touch interrupt pin
    pinMode(TOUCH_INT, INPUT_PULLUP);

    // Reset touch controller
    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);  delay(20);
    digitalWrite(TOUCH_RST, HIGH); delay(200);

    // I2C for touch
    Wire.begin(TOUCH_SDA, TOUCH_SCL);

    // Init display
    if (!gfx->begin()) {
        Serial.println("ERROR: Display init failed — check wiring");
        while (1) delay(100);
    }
    gfx->setRotation(1);   // landscape, 480×320
    gfx->fillScreen(COLOR_WHITE);
    Serial.println("Display OK");

    // Init touch (FT6336U is FT6206-compatible)
    if (!ctp.begin(40, &Wire)) {
        Serial.println("WARNING: Touch init failed — drawing disabled");
        Serial.println("Display still functional.");
    } else {
        Serial.println("Touch OK");
    }

    currentColor = palette[selectedColorIndex];
    clearCanvas();
    Serial.println("Ready — draw on screen");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    if (ctp.touched()) {
        TS_Point p = ctp.getPoint();

        int x = mapTouchX(p.x, p.y);
        int y = mapTouchY(p.x, p.y);

        Serial.print("raw: ");
        Serial.print(p.x);
        Serial.print(", ");
        Serial.print(p.y);
        Serial.print(" -> mapped: ");
        Serial.print(x);
        Serial.print(", ");
        Serial.println(y);

        if (x >= DRAW_W) {
            // Palette / CLR area
            handleUiTap(x, y);
            wasTouching = false;
            lastX = -1;
            lastY = -1;
            delay(150);
        } else {
            drawOnCanvas(x, y);
            wasTouching = true;
            delay(5);
        }
    } else {
        wasTouching = false;
        lastX = -1;
        lastY = -1;
        delay(5);
    }
}
