// UiDashboard.h
// A focused, single-screen touch UI: status bar + live AP list + one big
// start/stop button. No nested menus — wardriving is the only thing this
// firmware does, so the UI doesn't need to be one either.

#pragma once

#include <TFT_eSPI.h>
#include <Preferences.h>
#include "WardrivingEngine.h"

class UiDashboard {
public:
    UiDashboard(TFT_eSPI &tft, WardrivingEngine &engine);

    void begin();

    // Call every loop iteration. Pass the current touch point (or {-1,-1} if
    // no touch). Returns true if the touch was consumed by the UI.
    bool update(int16_t touchX, int16_t touchY, bool touched);

    // Force a full redraw (e.g. after returning from a different screen).
    void requestFullRedraw() { fullRedraw = true; }

private:
    TFT_eSPI &tft;
    WardrivingEngine &engine;
    Preferences prefs;
    bool fullRedraw = true;

    enum class Screen { Dashboard, Options, Summary };
    Screen currentScreen = Screen::Dashboard;
    bool invertColors = false;

    // Snapshot of stats at the moment Stop was pressed, for the summary screen
    // (engine's own counters reset on the next startSession(), so we capture
    // them here rather than reading the engine live while showing this screen).
    int summaryWifiTotal = 0, summaryBleTotal = 0;
    int summaryOpen = 0, summaryWep = 0, summaryWpa = 0, summaryWpa3 = 0, summaryEnterprise = 0, summaryUnknown = 0;
    double summaryDistanceM = 0;
    uint32_t summaryDurationMs = 0;
    bool summaryLowMemoryStop = false;

    // Layout (320x240 landscape)
    static constexpr int STATUS_H = 56;
    static constexpr int BUTTON_H = 48;
    static constexpr int LIST_TOP = STATUS_H;
    static constexpr int LIST_BOTTOM_MARGIN = BUTTON_H;
    static constexpr int GEAR_SIZE = 28; // tappable corner icon size

    uint32_t lastStatusDrawMs = 0;
    int lastCombinedCountDrawn = -1;
    bool lastSessionState = false;
    bool lastGpsFix = false;
    // Tracks the actual displayed values so the periodic 1s timer only
    // triggers a real redraw (and the visible full-bar wipe that causes)
    // when something has genuinely changed - eliminates pointless flicker
    // while idle/static, when heap/distance/sats aren't actually moving.
    uint32_t lastHeapDrawn = 0xFFFFFFFF;
    int lastSatsDrawn = -1;
    uint32_t lastCkFailDrawn = 0xFFFFFFFF;
    long lastDistanceCmDrawn = -1; // distance in centimeters, for exact-enough comparison without float equality issues

    uint32_t alertBannerUntilMs = 0;
    int lastAlertCountSeen = 0;

    void drawStatusBar();
    void drawApList();
    void drawButton();
    void drawAlertBanner();
    void drawGearIcon();
    void drawOptionsScreen();
    void drawSummaryScreen();
    bool touchInButton(int16_t x, int16_t y) const;
    bool touchInGear(int16_t x, int16_t y) const;
    String formatElapsed(uint32_t ms) const;
    void applyInvertSetting();
};
