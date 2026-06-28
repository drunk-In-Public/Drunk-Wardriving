// main.cpp
// Rockbase NM-CYD-C5 wardriving firmware
// Passive Wi-Fi AP scanning, GPS tagging, WiGLE-CSV logging, clean touch UI.

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <esp_system.h>

#include "pins.h"
#include "WardrivingEngine.h"
#include "UiDashboard.h"
#include "splash_image.h"

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(HSPI); // separate logical SPI handle for the touch controller
XPT2046_Touchscreen touch(PIN_TOUCH_CS);

WardrivingEngine engine(PIN_GPS_RX, PIN_GPS_TX, GPS_BAUD);
UiDashboard ui(tft, engine);

// Calibrate these against your panel; values below are a reasonable ST7789/XPT2046 default.
#define TOUCH_MIN_X 200
#define TOUCH_MAX_X 3700
#define TOUCH_MIN_Y 240
#define TOUCH_MAX_Y 3800

static bool readTouch(int16_t &x, int16_t &y) {
    if (!touch.touched()) return false;
    TS_Point p = touch.getPoint();
    x = map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, tft.width());
    y = map(p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, tft.height());
    x = constrain(x, 0, tft.width() - 1);
    y = constrain(y, 0, tft.height() - 1);
    return true;
}

static void bootStatus(const char *msg) {
    // Boot diagnostics now go to Serial only — the splash screen stays on
    // screen during loading instead of a scrolling text log.
    Serial.println(msg);
}

static void showSplash() {
    Serial.println("[splash] start");
    tft.pushImage(0, 0, SPLASH_WIDTH, SPLASH_HEIGHT, SPLASH_IMAGE);
}

void setup() {
    Serial.begin(115200);
    uint32_t serialWaitStart = millis();
    while (!Serial && millis() - serialWaitStart < 4000) {
        delay(10);
    }
    delay(500);

    // Log why we booted - distinguishes a normal power-on from a panic,
    // watchdog timeout, or brownout reset, which matters a lot when
    // diagnosing crash/reboot reports.
    esp_reset_reason_t resetReason = esp_reset_reason();
    const char *reasonStr = "UNKNOWN";
    switch (resetReason) {
        case ESP_RST_POWERON:   reasonStr = "POWERON (normal)"; break;
        case ESP_RST_SW:        reasonStr = "SW (software reset)"; break;
        case ESP_RST_PANIC:     reasonStr = "PANIC (crash!)"; break;
        case ESP_RST_INT_WDT:   reasonStr = "INT_WDT (watchdog - interrupt)"; break;
        case ESP_RST_TASK_WDT:  reasonStr = "TASK_WDT (watchdog - task)"; break;
        case ESP_RST_WDT:       reasonStr = "WDT (watchdog - other)"; break;
        case ESP_RST_BROWNOUT:  reasonStr = "BROWNOUT (power sag!)"; break;
        default: break;
    }
    for (int i = 0; i < 3; i++) {
        Serial.println("[boot] RESET REASON: " + String(reasonStr));
        delay(80);
    } // extra grace period for the USB-CDC host reconnect on C5
    for (int i = 0; i < 3; i++) {
        Serial.println("[boot] starting setup()");
        delay(100);
    }

    // Prime TFT-related pins as plain GPIO and set known states BEFORE
    // tft.init() claims some of them for hardware SPI. This ordering matters
    // on the C5 — without it, pins can be left in a non-GPIO IOMUX function
    // from boot and pinMode/digitalWrite on them silently fails.
    pinMode(PIN_TFT_CS, OUTPUT);
    digitalWrite(PIN_TFT_CS, HIGH);
    pinMode(PIN_SPI_MOSI, OUTPUT);
    digitalWrite(PIN_SPI_MOSI, HIGH);
    pinMode(PIN_SPI_SCK, OUTPUT);
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, LOW); // backlight OFF for now - turned on only after splash is already drawn, so the display's transient init-time garbage/white GRAM content is never actually visible
    pinMode(PIN_TFT_DC, OUTPUT);
    digitalWrite(PIN_TFT_DC, HIGH);
    Serial.println("[boot] pins primed");

    // Display
    tft.init();

    // Rotate 90 degrees to landscape. Previously a *redundant* call to
    // setRotation(0) right after init() hung on this board - but that was
    // calling it with the same value init() already set internally. Setting
    // an actual different value (1 = landscape) is a different operation,
    // so we try it directly here, before any other tft drawing calls.
    tft.setRotation(1);

    // NOTE: deliberately NOT calling tft.invertDisplay() here. The splash
    // image's colors are permanently baked into its own pixel data (see
    // splash_image.h) specifically because calling invertDisplay() around
    // the splash previously caused a blank white-screen bug on this board.
    // The live "Invert colors" Options setting is applied later in
    // UiDashboard::begin(), which only affects the dashboard, not the splash.

    // No fillScreen() here - showSplash() pushes a full 320x240 image that
    // covers every pixel already, so clearing first was just an extra
    // visible black flash before the splash image painted over it.
    uint32_t splashStartMs = millis();
    showSplash();

    // Backlight turns on HERE, not before tft.init() - the display's GRAM
    // is uninitialized garbage (commonly white) until init() and the first
    // real draw complete, and ST7789 init sequences have real built-in
    // hardware delays (sleep-out timing etc.) that can take several hundred
    // milliseconds. With the backlight already on during that window (the
    // old order), that transient garbage was fully visible - confirmed via
    // video analysis showing a genuine ~0.8 second solid white screen
    // before the splash content appeared. Turning the backlight on only
    // now means nothing is visible until the splash is already correct.
    digitalWrite(PIN_TFT_BL, HIGH);

    bootStatus("1: tft.init() + setRotation(1) OK, splash shown");

    // Touch — shares the main SPI bus, separate CS
    touch.begin();
    bootStatus("3: touch.begin() OK");
    touch.setRotation(1); // match display rotation so touch coordinates line up
    bootStatus("4: touch.setRotation() OK");

    // SD card — shares the main SPI bus, separate CS. Retry a few times:
    // SD.begin() failing on the first attempt but succeeding moments later
    // is a known ESP32 SPI-bus-timing quirk, not necessarily a bad card.
    bool sdOk = false;
    for (int attempt = 1; attempt <= 4 && !sdOk; attempt++) {
        sdOk = SD.begin(PIN_SD_CS);
        if (!sdOk) {
            Serial.println("[boot] SD.begin() attempt " + String(attempt) + " failed, retrying...");
            SD.end();
            delay(250);
        }
    }
    if (!sdOk) {
        bootStatus("5: SD.begin() FAILED (continuing)");
        // Unmissable visual indicator - serial has proven unreliable for
        // catching this, and the old on-screen boot text is gone now that
        // the splash replaced it. SD failure means every scan cycle bails
        // out before ever touching Wi-Fi/BLE, looking exactly like "stuck
        // in scanning, never finds anything."
        tft.fillScreen(TFT_RED);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_RED);
        tft.setTextSize(2);
        tft.drawString("SD CARD ERROR", tft.width() / 2, tft.height() / 2 - 15);
        tft.setTextSize(1);
        tft.drawString("Check card is inserted/seated", tft.width() / 2, tft.height() / 2 + 15);
        tft.drawString("Scanning will not work without it", tft.width() / 2, tft.height() / 2 + 30);
        delay(4000);
    } else {
        bootStatus("5: SD.begin() OK");
    }

    bootStatus("6: calling engine.begin()...");
    engine.setBootStatusCallback(bootStatus);
    if (!engine.begin()) {
        bootStatus("6: engine.begin() FAILED");
    } else {
        bootStatus("6: engine.begin() OK");
    }

    bootStatus("7: calling ui.begin()...");

    // Hold the splash for a minimum duration so it's visible even if the
    // rest of boot finishes very quickly.
    const uint32_t MIN_SPLASH_MS = 3500;
    uint32_t elapsed = millis() - splashStartMs;
    if (elapsed < MIN_SPLASH_MS) delay(MIN_SPLASH_MS - elapsed);
    Serial.println("[splash] end");

    // Backlight off during the redraw transition, so whatever happens on
    // screen while the dashboard paints over the splash isn't visible at
    // all - trading the brief redraw flash for a short ~100ms blackout.
    digitalWrite(PIN_TFT_BL, LOW);
    delay(20); // let the backlight actually settle off before drawing underneath it

    ui.begin();
    bootStatus("8: ui.begin() done");

    delay(80); // remaining blackout time (20ms settle + 80ms here = ~100ms total)
    digitalWrite(PIN_TFT_BL, HIGH); // dashboard is fully drawn - safe to reveal now

    // If this boot was triggered by the low-memory self-restart, resume
    // scanning automatically into a new file rather than leaving the user
    // to notice the device rebooted and have to tap Start themselves.
    if (engine.consumeResumeFlag()) {
        Serial.println("[boot] resuming scan session after self-restart");
        engine.startSession();
        ui.requestFullRedraw();
    }
}

void loop() {
    static bool firstLoop = true;
    static uint32_t lastHeartbeat = 0;
    if (firstLoop) {
        Serial.println("[loop] first iteration reached");
        firstLoop = false;
    }
    if (millis() - lastHeartbeat > 2000) {
        Serial.println("[loop] heartbeat, millis=" + String(millis()));
        lastHeartbeat = millis();
    }

    // Check touch BEFORE the scan cycle, not after — engine.update() can
    // block for a couple seconds during a scan (Wi-Fi scan + BLE window),
    // during which loop() never returns to read touch input. Reading touch
    // first means a tap on STOP is caught and applied before the *next*
    // blocking scan starts, rather than possibly being missed mid-scan.
    int16_t tx = -1, ty = -1;
    bool touched = readTouch(tx, ty);
    ui.update(tx, ty, touched);

    bool scanHappened = engine.update();

    if (touched) delay(150); // crude debounce; fine for a single big button
    delay(scanHappened ? 0 : 20);
}
