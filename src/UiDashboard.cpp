// UiDashboard.cpp
#include "UiDashboard.h"
#include "OuiTable.h"
// Note: Orbitron_Light_24 is NOT explicitly included here - TFT_eSPI.h
// (included via UiDashboard.h) already pulls it in automatically via its
// own User_Setups/User_Custom_Fonts.h. Including it again here caused a
// duplicate-definition build error, since this auto-generated font file
// has no include-guard against being included twice.

// --- Simple color palette (clean, high-contrast, not "hacker green on black") ---
#define COL_BG        TFT_BLACK
#define COL_BAR_BG    0x10A2   // dark slate blue
#define COL_TEXT      TFT_WHITE
#define COL_DIM       0x8410   // grey
#define COL_GOOD      0x07E0   // green  - GPS fix / OK
#define COL_WARN      0xFD20   // amber  - no fix / waiting
#define COL_ACCENT    0x051D   // muted blue accent
#define COL_BTN_START 0x0500   // green-ish for "Start"
#define COL_BTN_STOP  0xB000   // red-ish for "Stop"
#define COL_ROW_ALT   0x18E3   // subtle row stripe

UiDashboard::UiDashboard(TFT_eSPI &tft_, WardrivingEngine &engine_)
    : tft(tft_), engine(engine_) {}

void UiDashboard::begin() {
    prefs.begin("wardrive_ui", false);
    invertColors = prefs.getBool("invert", false);
    applyInvertSetting();

    bool wantDualBand = prefs.getBool("dualband", true);
    engine.setScanBand(wantDualBand ? WardrivingEngine::ScanBand::DualBand
                                     : WardrivingEngine::ScanBand::TwoPointFourOnly);

    engine.setBleEnabled(prefs.getBool("ble_enabled", false));
    engine.setSnifferMode(prefs.getBool("sniffer_mode", false)); // defaults off - new/untested on real hardware

    // No fillScreen() here - drawStatusBar()/drawApList()/drawButton()
    // already cover the entire screen between them with no gaps, so a
    // separate full-screen clear was just an extra visible flash before
    // the real dashboard content painted over it a moment later.
    fullRedraw = true;
}

void UiDashboard::applyInvertSetting() {
    // Always call this explicitly and unconditionally. A previous attempt
    // to skip this call when it "shouldn't" change anything (assuming
    // ST7789 powers up non-inverted by default) was WRONG - tft.init()
    // apparently sets some internal inversion state that doesn't match
    // that assumption, and skipping the explicit call left the display
    // defaulting to the wrong (light) colors. Always sending the command
    // explicitly is the only way to guarantee correctness here, even if it
    // means a flash is possible at this transition - wrong default colors
    // is a much worse problem than a cosmetic flash.
    tft.invertDisplay(invertColors);
}

bool UiDashboard::touchInButton(int16_t x, int16_t y) const {
    int h = tft.height();
    return y >= (h - BUTTON_H) && y <= h;
}

bool UiDashboard::touchInGear(int16_t x, int16_t y) const {
    int w = tft.width();
    return x >= (w - GEAR_SIZE) && x <= w && y >= 0 && y <= GEAR_SIZE;
}

String UiDashboard::formatElapsed(uint32_t ms) const {
    uint32_t s = ms / 1000;
    uint32_t h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    char buf[12];
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", h, m, sec);
    return String(buf);
}

void UiDashboard::drawStatusBar() {
    int w = tft.width();
    tft.fillRect(0, 0, w, STATUS_H, COL_BAR_BG);

    bool fix = engine.gpsHasFix();
    bool stale = engine.isFixStale();
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(fix ? COL_GOOD : (stale ? COL_WARN : TFT_RED), COL_BAR_BG);
    tft.setTextSize(1);
    tft.drawString(fix ? "GPS: Locked In Bro" : (stale ? "GPS: STALE" : "GPS: NO FIX"), 6, 4);

    tft.setTextColor(COL_DIM, COL_BAR_BG);
    tft.drawString(
        "Sats:" + String(engine.gpsSatellites()) +
        " NMEA:" + String(engine.nmeaSentenceCount()) +
        " CkFail:" + String(engine.checksumFailCount()),
        6, 18
    );

    tft.setTextColor(COL_TEXT, COL_BAR_BG);
    tft.drawString("AP:" + String(engine.totalApsFound()) + " BLE:" + String(engine.totalBleFound()), w - 140, 4);
    tft.drawString(formatElapsed(engine.elapsedMs()), w - 120, 18);

    char distBuf[32];
    snprintf(distBuf, sizeof(distBuf), "%.2f km  H:%u", engine.distanceMeters() / 1000.0, (unsigned)ESP.getFreeHeap());
    tft.setTextColor(COL_DIM, COL_BAR_BG);
    tft.drawString(distBuf, 6, 34);

    if (engine.alertCount() > 0) {
        tft.setTextColor(COL_WARN, COL_BAR_BG);
        tft.drawString("Alerts: " + String(engine.alertCount()), w - 150, 34);
    } else if (engine.isSessionActive()) {
        // Pulsing "scanning" indicator instead of the old W:/B:/T: timer
        // readout. Can't animate mid-scan (the chip is fully blocked during
        // the actual radio call), but pulses visibly during every idle gap
        // between cycles, and stays lit the whole time a session is active.
        bool pulseOn = (millis() / 700) % 2 == 0;
        uint16_t dotColor = pulseOn ? COL_WARN : COL_DIM;
        tft.fillCircle(w - 145, 38, 6, dotColor);
        tft.setTextColor(COL_TEXT, COL_BAR_BG);
        tft.drawString("Chugging AP's", w - 132, 34);
    } else {
        tft.fillCircle(w - 145, 38, 6, COL_BAR_BG); // clear the dot when idle
        tft.drawString("                ", w - 132, 34); // clear leftover text (sized for "Chugging AP's")
    }

    tft.drawFastHLine(0, STATUS_H - 1, w, COL_ACCENT);
    drawGearIcon();
}

void UiDashboard::drawGearIcon() {
    int w = tft.width();
    // Simple "menu/settings" hamburger icon - reliable to draw at small size,
    // recognizable without needing a bitmap asset.
    int x0 = w - GEAR_SIZE + 6;
    int x1 = w - 6;
    for (int i = 0; i < 3; i++) {
        int y = 8 + i * 6;
        tft.drawFastHLine(x0, y, x1 - x0, COL_TEXT);
    }
}

void UiDashboard::drawApList() {
    int w = tft.width();
    int top = LIST_TOP;
    int bottom = tft.height() - LIST_BOTTOM_MARGIN;
    int rowH = 22;

    tft.fillRect(0, top, w, bottom - top, COL_BG);

    const DisplayRecord *records = engine.displayRecords();
    int count = engine.displayRecordCount();
    if (count == 0) {
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.drawString(
            engine.isSessionActive() ? "Scanning..." : "Press Start to begin scanning",
            10, top + 12
        );
        return;
    }

    int y = top + 4;
    for (int i = 0; i < count && y + rowH <= bottom; i++, y += rowH) {
        if (i % 2 == 1) tft.fillRect(0, y, w, rowH, COL_ROW_ALT);

        const DisplayRecord &r = records[i];
        uint16_t rowBg = (i % 2 == 1) ? COL_ROW_ALT : COL_BG;
        tft.setTextDatum(TL_DATUM);

        // Small type tag (W/B) so mixed Wi-Fi + BLE results are distinguishable at a glance
        tft.setTextColor(r.isBle ? COL_ACCENT : COL_DIM, rowBg);
        tft.drawString(r.isBle ? "[B]" : "[W]", 6, y + 4);

        tft.setTextColor(COL_TEXT, rowBg);
        String label = r.label;
        if (label.length() > 13) label = label.substring(0, 12) + "...";
        tft.drawString(label, 32, y + 4);

        // Security indicator - red for open networks (the classic
        // wardriving "found something interesting" signal), green for
        // WPA3, neutral for everything else. Blank for BLE (n/a).
        if (r.secChar != '-') {
            uint16_t secColor;
            switch (r.secChar) {
                case 'O': secColor = TFT_RED; break;
                case 'W': secColor = COL_WARN; break;
                case '3': secColor = COL_GOOD; break;
                default:  secColor = COL_DIM; break;
            }
            tft.setTextColor(secColor, rowBg);
            char secBuf[2] = {r.secChar, '\0'};
            tft.drawString(secBuf, w - 78, y + 4);
        }

        tft.setTextColor(COL_GOOD, rowBg);
        tft.drawString(String(r.rssi) + "dB", w - 50, y + 4);
    }
}

void UiDashboard::drawSummaryScreen() {
    int w = tft.width(), h = tft.height();

    tft.fillScreen(COL_BG);

    tft.fillRect(0, 0, w, STATUS_H, COL_BAR_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_TEXT, COL_BAR_BG);
    tft.setFreeFont(&Orbitron_Light_24);
    tft.drawString("Run Summary", 10, 12);
    tft.setFreeFont(nullptr);
    tft.setTextSize(1);
    tft.drawFastHLine(0, STATUS_H - 1, w, COL_ACCENT);

    int y = STATUS_H + 4;
    int lineH = 14;

    if (summaryLowMemoryStop) {
        tft.fillRect(0, y, w, 16, COL_WARN);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_BLACK, COL_WARN);
        tft.drawString("AUTO-STOPPED: Low memory", w / 2, y + 8);
        y += 18;
    }
    int leftX = 16, rightX = w - 16;

    auto row = [&](const String &label, const String &value, uint16_t valColor) {
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.drawString(label, leftX, y);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(valColor, COL_BG);
        tft.drawString(value, rightX, y);
        y += lineH;
    };

    char durBuf[16];
    uint32_t totalSec = summaryDurationMs / 1000;
    snprintf(durBuf, sizeof(durBuf), "%02lu:%02lu:%02lu", totalSec / 3600, (totalSec / 60) % 60, totalSec % 60);

    char distBuf[20];
    snprintf(distBuf, sizeof(distBuf), "%.2f km", summaryDistanceM / 1000.0);

    row("Duration", String(durBuf), COL_TEXT);
    row("Distance", String(distBuf), COL_TEXT);
    row("New Wi-Fi networks", String(summaryWifiTotal), COL_GOOD);
    row("New BLE devices", String(summaryBleTotal), COL_GOOD);

    y += 2;
    tft.drawFastHLine(leftX, y, w - leftX * 2, COL_ACCENT);
    y += 6;

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("Encryption breakdown:", leftX, y);
    y += lineH;

    // Compact two-line layout instead of one row per type - six separate
    // rows don't fit in the vertical space left on a 320x240 panel.
    tft.setTextColor(summaryOpen > 0 ? COL_WARN : COL_TEXT, COL_BG);
    tft.drawString("Open:" + String(summaryOpen) + "  WEP:" + String(summaryWep) +
                   "  WPA/2:" + String(summaryWpa), leftX, y);
    y += lineH;
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString("WPA3:" + String(summaryWpa3) + "  Ent:" + String(summaryEnterprise) +
                   "  Other:" + String(summaryUnknown), leftX, y);

    int by = h - BUTTON_H;
    tft.fillRect(0, by, w, BUTTON_H, COL_ACCENT);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, COL_ACCENT);
    tft.setFreeFont(&Orbitron_Light_24);
    tft.drawString("DONE", w / 2, by + BUTTON_H / 2);
    tft.setFreeFont(nullptr);
    tft.setTextSize(1);
}

void UiDashboard::drawOptionsScreen() {
    int w = tft.width(), h = tft.height();

    tft.fillScreen(COL_BG);

    // Header bar, reuses the same style as the dashboard's status bar
    tft.fillRect(0, 0, w, STATUS_H, COL_BAR_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_TEXT, COL_BAR_BG);
    tft.setFreeFont(&Orbitron_Light_24);
    tft.drawString("Options", 10, 12);
    tft.setFreeFont(nullptr);
    tft.setTextSize(1);
    tft.drawFastHLine(0, STATUS_H - 1, w, COL_ACCENT);

    int rowH = 26, gap = 4;
    int row1Top = STATUS_H + 2;
    int row2Top = row1Top + rowH + gap;
    int row3Top = row2Top + rowH + gap;
    int row4Top = row3Top + rowH + gap;

    // Row 1: Invert display colors (toggle switch)
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString("Invert display colors", 16, row1Top + (rowH / 2) - 4);
    int toggleW = 44, toggleH = 20;
    int toggleX = w - toggleW - 16, toggleY = row1Top + (rowH - toggleH) / 2;
    tft.fillRoundRect(toggleX, toggleY, toggleW, toggleH, toggleH / 2, invertColors ? COL_GOOD : COL_DIM);
    int knobR = (toggleH / 2) - 3;
    int knobX = invertColors ? (toggleX + toggleW - knobR - 3) : (toggleX + knobR + 3);
    tft.fillCircle(knobX, toggleY + toggleH / 2, knobR, TFT_WHITE);
    tft.drawFastHLine(0, row1Top + rowH + gap / 2, w, COL_ACCENT);

    // Row 2: Wi-Fi scan band (cycling pill)
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString("Wi-Fi scan band", 16, row2Top + (rowH / 2) - 4);
    bool dual = (engine.getScanBand() == WardrivingEngine::ScanBand::DualBand);
    String bandLabel = dual ? "Dual-Band" : "2.4GHz Only";
    int pillW = 100, pillH = 20;
    int pillX = w - pillW - 16, pillY = row2Top + (rowH - pillH) / 2;
    tft.fillRoundRect(pillX, pillY, pillW, pillH, pillH / 2, dual ? COL_ACCENT : COL_GOOD);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, dual ? COL_ACCENT : COL_GOOD);
    tft.drawString(bandLabel, pillX + pillW / 2, pillY + pillH / 2);
    tft.drawFastHLine(0, row2Top + rowH + gap / 2, w, COL_ACCENT);

    // Row 3: BLE scanning on/off (toggle switch)
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString("BLE scanning", 16, row3Top + (rowH / 2) - 4);
    bool bleOn = engine.isBleEnabled();
    int toggleX3 = w - toggleW - 16, toggleY3 = row3Top + (rowH - toggleH) / 2;
    tft.fillRoundRect(toggleX3, toggleY3, toggleW, toggleH, toggleH / 2, bleOn ? COL_GOOD : COL_DIM);
    int knobX3 = bleOn ? (toggleX3 + toggleW - knobR - 3) : (toggleX3 + knobR + 3);
    tft.fillCircle(knobX3, toggleY3 + toggleH / 2, knobR, TFT_WHITE);
    tft.drawFastHLine(0, row3Top + rowH + gap / 2, w, COL_ACCENT);

    // Row 4: Sniffer mode on/off (toggle switch) - experimental promiscuous-mode
    // Wi-Fi scanning, built to test whether avoiding repeated scanNetworks()
    // calls sidesteps the confirmed per-call memory leak.
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString("Sniffer mode (experimental)", 16, row4Top + (rowH / 2) - 4);
    bool sniffOn = engine.isSnifferMode();
    int toggleX4 = w - toggleW - 16, toggleY4 = row4Top + (rowH - toggleH) / 2;
    tft.fillRoundRect(toggleX4, toggleY4, toggleW, toggleH, toggleH / 2, sniffOn ? COL_GOOD : COL_DIM);
    int knobX4 = sniffOn ? (toggleX4 + toggleW - knobR - 3) : (toggleX4 + knobR + 3);
    tft.fillCircle(knobX4, toggleY4 + toggleH / 2, knobR, TFT_WHITE);
    tft.drawFastHLine(0, row4Top + rowH + gap / 2, w, COL_ACCENT);

    // Back button, same region/style as the dashboard's start/stop button
    int by = h - BUTTON_H;
    tft.fillRect(0, by, w, BUTTON_H, COL_ACCENT);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, COL_ACCENT);
    tft.setFreeFont(&Orbitron_Light_24);
    tft.drawString("BACK", w / 2, by + BUTTON_H / 2);
    tft.setFreeFont(nullptr);
    tft.setTextSize(1);
}

void UiDashboard::drawButton() {
    int w = tft.width(), h = tft.height();
    int y = h - BUTTON_H;
    bool active = engine.isSessionActive();

    tft.fillRect(0, y, w, BUTTON_H, active ? COL_BTN_STOP : COL_BTN_START);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, active ? COL_BTN_STOP : COL_BTN_START);
    tft.setFreeFont(&Orbitron_Light_24);
    tft.drawString(active ? "Stop Chugging" : "Start Chugging", w / 2, y + BUTTON_H / 2);
    tft.setFreeFont(nullptr); // revert to the small built-in font for everything else
    tft.setTextSize(1);
}

void UiDashboard::drawAlertBanner() {
    int w = tft.width();
    int bannerH = 30;
    int y = STATUS_H; // sits just below the status bar, briefly covering the top of the list

    tft.fillRect(0, y, w, bannerH, COL_WARN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_BLACK, COL_WARN);
    tft.setTextSize(1);

    String msg = engine.lastAlertMessage();
    if (msg.length() > 38) msg = msg.substring(0, 37) + "...";
    tft.drawString(msg, w / 2, y + bannerH / 2);
}

bool UiDashboard::update(int16_t touchX, int16_t touchY, bool touched) {
    int w = tft.width(), h = tft.height();

    // TEMP DIAGNOSTIC: raw touch indicator, independent of any button
    // hit-testing logic. If this dot never appears while tapping during a
    // scan, the touch hardware itself isn't being read/reached - if it DOES
    // appear but Stop still doesn't trigger, the bug is in the hit-test/
    // button logic instead, not touch detection.
    static bool lastTouchedDrawn = false;
    if (touched != lastTouchedDrawn) {
        tft.fillCircle(8, h - BUTTON_H - 10, 5, touched ? TFT_RED : COL_BG);
        lastTouchedDrawn = touched;
    }

    // --- Summary screen mode (shown once, right after Stop) ---
    if (currentScreen == Screen::Summary) {
        if (fullRedraw) {
            drawSummaryScreen();
            fullRedraw = false;
        }
        if (touched && touchInButton(touchX, touchY)) {
            Serial.println("[ui] UI returned to normal mode (Start button shown)");
            currentScreen = Screen::Dashboard;
            fullRedraw = true;
            return true;
        }
        return false;
    }

    // --- Options screen mode ---
    if (currentScreen == Screen::Options) {
        if (fullRedraw) {
            drawOptionsScreen();
            fullRedraw = false;
        }
        if (touched) {
            // Back button: bottom bar, same region as the dashboard's start/stop button
            if (touchInButton(touchX, touchY)) {
                currentScreen = Screen::Dashboard;
                fullRedraw = true;
                return true;
            }
            int rowH = 26, gap = 4;
            int row1Top = STATUS_H + 2;
            int row2Top = row1Top + rowH + gap;
            int row3Top = row2Top + rowH + gap;
            int row4Top = row3Top + rowH + gap;

            // Row 1: Invert colors
            if (touchY >= row1Top && touchY <= row1Top + rowH) {
                invertColors = !invertColors;
                applyInvertSetting();
                prefs.putBool("invert", invertColors);
                drawOptionsScreen();
                return true;
            }
            // Row 2: Scan band
            if (touchY >= row2Top && touchY <= row2Top + rowH) {
                bool nowDual = (engine.getScanBand() == WardrivingEngine::ScanBand::DualBand);
                bool wantDual = !nowDual;
                engine.setScanBand(wantDual ? WardrivingEngine::ScanBand::DualBand
                                             : WardrivingEngine::ScanBand::TwoPointFourOnly);
                prefs.putBool("dualband", wantDual);
                drawOptionsScreen();
                return true;
            }
            // Row 3: BLE scanning on/off
            if (touchY >= row3Top && touchY <= row3Top + rowH) {
                bool wantBle = !engine.isBleEnabled();
                engine.setBleEnabled(wantBle);
                prefs.putBool("ble_enabled", wantBle);
                drawOptionsScreen();
                return true;
            }
            // Row 4: Sniffer mode on/off
            if (touchY >= row4Top && touchY <= row4Top + rowH) {
                bool wantSniff = !engine.isSnifferMode();
                engine.setSnifferMode(wantSniff);
                prefs.putBool("sniffer_mode", wantSniff);
                drawOptionsScreen();
                return true;
            }
        }
        return false;
    }

    // --- Dashboard mode ---
    bool consumed = false;

    if (touched && touchInGear(touchX, touchY)) {
        currentScreen = Screen::Options;
        fullRedraw = true;
        return true;
    }

    if (touched && touchInButton(touchX, touchY)) {
        if (engine.isSessionActive()) {
            Serial.println("[ui] Stop button pressed");
            engine.stopSession();

            // Snapshot stats for the summary screen - the engine's own
            // per-run counters reset on the next startSession(), so capture
            // them now rather than reading the engine live while this
            // screen is showing.
            summaryOpen = engine.openCount();
            summaryWep = engine.wepCount();
            summaryWpa = engine.wpaCount();
            summaryWpa3 = engine.wpa3Count();
            summaryEnterprise = engine.enterpriseCount();
            summaryUnknown = engine.unknownAuthCount();
            summaryWifiTotal = summaryOpen + summaryWep + summaryWpa + summaryWpa3 + summaryEnterprise + summaryUnknown;
            summaryBleTotal = engine.newBleCount();
            summaryDistanceM = engine.distanceMeters();
            summaryDurationMs = engine.lastSessionDurationMs();
            summaryLowMemoryStop = false;

            currentScreen = Screen::Summary;
            fullRedraw = true;
            return true;
        } else {
            Serial.println("[ui] Start button pressed");
            engine.startSession();
        }
        fullRedraw = true;
        consumed = true;
    }

    // New alert? Show a banner over the list for a few seconds.
    if (engine.alertCount() > lastAlertCountSeen) {
        lastAlertCountSeen = engine.alertCount();
        alertBannerUntilMs = millis() + 4000;
        drawAlertBanner();
    } else if (alertBannerUntilMs != 0 && millis() > alertBannerUntilMs) {
        alertBannerUntilMs = 0;
        drawApList(); // banner expired, restore the list underneath it
    }

    bool sessionChanged = (engine.isSessionActive() != lastSessionState);
    bool gpsChanged = (engine.gpsHasFix() != lastGpsFix);
    bool apCountChanged = ((engine.totalApsFound() + engine.totalBleFound()) != lastCombinedCountDrawn);
    bool timeToRefreshStatus = (millis() - lastStatusDrawMs > 1000);

    // Only actually worth redrawing if something visible has genuinely
    // changed - prevents the periodic timer from triggering a pointless
    // full-bar wipe+redraw (visible as flicker) when heap/distance/sats
    // are sitting perfectly still, which is the common case while idle.
    uint32_t curHeap = ESP.getFreeHeap();
    int curSats = engine.gpsSatellites();
    uint32_t curCkFail = engine.checksumFailCount();
    long curDistanceCm = (long)(engine.distanceMeters() * 100);
    bool statusValuesChanged = (curHeap != lastHeapDrawn) || (curSats != lastSatsDrawn) ||
                                (curCkFail != lastCkFailDrawn) || (curDistanceCm != lastDistanceCmDrawn);

    if (fullRedraw) {
        // No fillScreen() here either - same reasoning as begin(): these
        // three draw calls already cover the entire screen with no gaps,
        // so clearing first just creates a visible flash before they paint
        // over it a moment later. This is what actually ran right after
        // the splash screen, since begin() sets fullRedraw=true and this
        // is the first place that gets checked.
        drawStatusBar();
        drawApList();
        drawButton();
        fullRedraw = false;
        lastStatusDrawMs = millis();
        lastCombinedCountDrawn = engine.totalApsFound() + engine.totalBleFound();
        lastSessionState = engine.isSessionActive();
        lastGpsFix = engine.gpsHasFix();
        return consumed;
    }

    if ((timeToRefreshStatus && statusValuesChanged) || sessionChanged || gpsChanged) {
        drawStatusBar();
        lastStatusDrawMs = millis();
        lastGpsFix = engine.gpsHasFix();
        lastHeapDrawn = curHeap;
        lastSatsDrawn = curSats;
        lastCkFailDrawn = curCkFail;
        lastDistanceCmDrawn = curDistanceCm;
    }
    if (apCountChanged && alertBannerUntilMs == 0) {
        drawApList();
        lastCombinedCountDrawn = engine.totalApsFound() + engine.totalBleFound();
    } else if (apCountChanged) {
        // count changed but banner is showing — remember it, list redraws when banner clears
        lastCombinedCountDrawn = engine.totalApsFound() + engine.totalBleFound();
    }
    if (sessionChanged) {
        lastSessionState = engine.isSessionActive();
        if (!lastSessionState) {
            Serial.println("[ui] UI returned to normal mode (Start button shown)");

            if (engine.wasLowMemoryStop()) {
                // Engine stopped itself, not the user - show the summary
                // screen with a clear reason instead of silently going
                // back to the dashboard, so this doesn't look like an
                // unexplained, unexpected stop.
                Serial.println("[ui] session was auto-stopped due to low memory");
                summaryOpen = engine.openCount();
                summaryWep = engine.wepCount();
                summaryWpa = engine.wpaCount();
                summaryWpa3 = engine.wpa3Count();
                summaryEnterprise = engine.enterpriseCount();
                summaryUnknown = engine.unknownAuthCount();
                summaryWifiTotal = summaryOpen + summaryWep + summaryWpa + summaryWpa3 + summaryEnterprise + summaryUnknown;
                summaryBleTotal = engine.newBleCount();
                summaryDistanceM = engine.distanceMeters();
                summaryDurationMs = engine.lastSessionDurationMs();
                summaryLowMemoryStop = true;
                engine.clearLowMemoryStopFlag();
                currentScreen = Screen::Summary;
                fullRedraw = true;
                return true;
            }
        }
        drawButton();
    }

    return consumed;
}
