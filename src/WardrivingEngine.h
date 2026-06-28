// WardrivingEngine.h
// Passive Wi-Fi AP scanning + GPS tagging + WiGLE-format CSV logging.
// Logic adapted from Bruce firmware's modules/gps/wardriving.cpp (passive-scan portions only).
// No transmit, deauth, or injection capability — scan + log only.

#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <TinyGPS++.h>
#include <SD.h>
#include <set>
#include <vector>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "PromiscSniffer.h"

struct ApRecord {
    String  bssid;
    String  ssid;     // SSID for Wi-Fi, device name (if any) for BLE
    String  authMode; // auth mode for Wi-Fi, "Misc [BLE]" for BLE
    int32_t channel;  // Wi-Fi channel; 0 for BLE
    int32_t rssi;
    double  lat;
    double  lng;
    String  type;     // "WIFI" or "BLE"
    String  gpsStatus; // "FRESH", "STALE", or "NONE" - GPS state at capture time
};

// Lightweight, heap-free record for the live on-screen list. Uses fixed
// char buffers instead of String members, so populating/sorting/clearing
// the display list every scan cycle does NOT churn the heap - this was the
// dominant fragmentation source (the old path pushed a String-heavy
// ApRecord for every network every cycle, then sorted/resized/cleared,
// generating hundreds of small malloc/free ops per cycle). Bruce keeps no
// such String-based per-record structure at all; this matches that intent.
struct DisplayRecord {
    char    label[33];  // SSID or vendor/placeholder, truncated to fit
    char    type[5];    // "WIFI" / "BLE"
    int32_t rssi;
    bool    isBle;
    char    secChar;    // 'O'=open, 'W'=WEP, 'P'=WPA/WPA2, '3'=WPA3, 'E'=enterprise, 'U'=unknown, '-'=n/a (BLE)
};

class WardrivingEngine {
public:
    // gpsRxPin/gpsTxPin: ESP32 pins wired to the GPS module (see pins.h)
    WardrivingEngine(int gpsRxPin, int gpsTxPin, uint32_t gpsBaud);

    // Call once at startup (after SD.begin() has succeeded).
    bool begin();

    // Call frequently from loop(). Internally rate-limits actual scans.
    // Returns true if a new scan cycle just completed (UI should refresh).
    bool update();

    // Optional callback for on-screen boot diagnostics (serial output has
    // proven unreliable on this board, so begin() can report progress here too).
    void setBootStatusCallback(void (*cb)(const char *)) { bootStatusCb = cb; }

    // Start / stop a logging session. While stopped, GPS is still read so the
    // UI can show fix status, but no scanning/logging happens.
    void startSession();
    void stopSession();
    bool isSessionActive() const { return sessionActive; }

    // Enable/disable BLE scanning alongside Wi-Fi. Defaults to on.
    void setBleEnabled(bool enabled) { bleEnabled = enabled; }
    bool isBleEnabled() const { return bleEnabled; }

    // True if the session was auto-stopped due to low memory rather than
    // the user pressing Stop. UI should show this distinctly.
    bool wasLowMemoryStop() const { return lowMemoryStop_; }
    void clearLowMemoryStopFlag() { lowMemoryStop_ = false; }

    // Call once at startup, after begin(). If true, a previous session was
    // running when the firmware self-restarted to reclaim memory - the
    // caller should immediately call startSession() again so scanning
    // resumes automatically rather than requiring the user to notice and
    // tap Start themselves.
    bool consumeResumeFlag() {
        Preferences p;
        p.begin("wardrive_eng", false);
        bool resume = p.getBool("resume", false);
        if (resume) p.putBool("resume", false);
        p.end();
        return resume;
    }

    // --- Run summary (for the post-scan summary screen) ---
    // Encryption breakdown counts ONLY new BSSIDs found during the most
    // recent run (resets on startSession()). Cross-session dedup means a
    // network already known from an earlier run this boot won't re-count
    // here even if driven past again - that's intentional.
    int openCount() const { return openCount_; }
    int wepCount() const { return wepCount_; }
    int wpaCount() const { return wpaCount_; } // WPA/WPA2/mixed PSK
    int wpa3Count() const { return wpa3Count_; }
    int enterpriseCount() const { return enterpriseCount_; }
    int unknownAuthCount() const { return unknownAuthCount_; }
    int newBleCount() const { return newBleCount_; } // new BLE devices found THIS run
    // elapsedMs() returns 0 once a session stops (gated on sessionActive) -
    // this snapshot preserves the final duration for the summary screen.
    uint32_t lastSessionDurationMs() const { return lastSessionDurationMs_; }

    // Wi-Fi scan band. DualBand scans both 2.4GHz and 5GHz (more coverage,
    // much slower - measured ~10s on this chip). TwoPointFourOnly restricts
    // to channels 1-13 (faster cycles, denser re-scanning while driving, but
    // misses 5GHz-only networks).
    enum class ScanBand { DualBand, TwoPointFourOnly };
    void setScanBand(ScanBand b) { scanBand = b; }
    ScanBand getScanBand() const { return scanBand; }

    // Promiscuous-mode passive sniffer instead of repeated WiFi.scanNetworks()
    // calls. Built specifically to test whether the confirmed "leak scales
    // with scan-call count" finding can be sidestepped by never repeatedly
    // calling the leaking function at all. New/untested on real hardware.
    void setSnifferMode(bool enabled) { snifferMode = enabled; }
    bool isSnifferMode() const { return snifferMode; }
    uint32_t snifferDroppedCount() const { return PromiscSniffer::droppedCount; }
    uint32_t snifferProcessedCount() const { return snifferProcessedCount_; }

    // --- Alert watchlist ---
    // MACs/addresses listed in /wardrive/alert.txt (one per line, '#' = comment)
    // trigger an alert when seen. Useful for tracking a known device.
    int alertCount() const { return alertCount_; }
    String lastAlertMessage() const { return lastAlertMessage_; }
    void clearLastAlert() { lastAlertMessage_ = ""; }

    // --- Last-cycle timing, for on-screen diagnostics (serial has proven
    // unreliable on this board) ---
    uint32_t lastWifiMs() const { return lastWifiMs_; }
    uint32_t lastBleMs() const { return lastBleMs_; }
    uint32_t lastCycleMs() const { return lastCycleMs_; }

    // --- Live stats for the UI ---
    int     totalApsFound() const { return (int)totalWifiSeenCount_; }
    int     totalBleFound() const { return (int)totalBleSeenCount_; }
    int     lastScanCount() const { return lastScanCount_; }
    double  distanceMeters() const { return distance; }
    uint32_t elapsedMs() const { return sessionActive ? millis() - sessionStartMs : 0; }
    String  currentFilename() const { return filename; }

    // gpsHasFix() = GPS currently reporting a live, fresh sentence (tight window).
    // hasUsableFix() = there's a last-known position good enough to log against,
    // even if the live feed just had a brief dropout. This is what scanning
    // should check, not gpsHasFix() — a single missed NMEA sentence shouldn't
    // stop an entire scan cycle.
    bool    gpsHasFix() const { return gps.location.isValid() && gps.location.age() < 5000; }
    bool    hasUsableFix() const { return lastValidFixMs != 0 && (millis() - lastValidFixMs) < FIX_STALE_MS; }
    bool    isFixStale() const { return hasUsableFix() && !gpsHasFix(); }
    uint32_t fixAgeMs() const { return lastValidFixMs == 0 ? UINT32_MAX : millis() - lastValidFixMs; }
    int     gpsSatellites() { return gps.satellites.isValid() ? gps.satellites.value() : 0; }
    double  gpsLat() { return gps.location.lat(); }
    double  gpsLng() { return gps.location.lng(); }

    // --- GPS debug counters (for diagnosing fix-loss issues) ---
    uint32_t nmeaSentenceCount() const { return gps.passedChecksum() + gps.failedChecksum(); }
    uint32_t checksumFailCount() const { return gps.failedChecksum(); }
    uint32_t sentencesWithFixCount() const { return gps.sentencesWithFix(); }

    // Most recent scan results, sorted strongest-signal first. Capped at a small
    // number so the UI list stays fast to redraw.
    const DisplayRecord *displayRecords() const { return displayRecords_; }
    int displayRecordCount() const { return displayCount_; }

private:
    TinyGPSPlus gps;
    HardwareSerial gpsSerial;
    int rxPin, txPin;
    uint32_t baud;

    bool sessionActive = false;
    bool bleEnabled = false; // defaulted off - see scanBle() comment for why
    ScanBand scanBand = ScanBand::DualBand;
    uint32_t sessionStartMs = 0;
    uint32_t lastScanMs = 0;
    // Scan cadence: DualBand cycles measure ~12s (10.4s wifi + 1.5s ble) on
    // this chip, so 22s leaves a safe idle gap for touch input. 2.4GHz-only
    // cycles measured ~3.45s (1.9s wifi + 1.5s ble), so 6s still leaves a
    // safe ~2.5s gap while giving meaningfully denser re-scanning.
    uint32_t scanIntervalForCurrentBand() const {
        return scanBand == ScanBand::DualBand ? 22000 : 6000;
    }
    static constexpr uint32_t FIX_STALE_MS = 20000;    // hold last fix this long before treating as "no fix" for logging purposes

    uint32_t lastValidFixMs = 0;
    uint32_t lastDistanceUpdateMs_ = 0; // for bounding implausible distance jumps from corrupted GPS reads
    double   lastValidLat = 0, lastValidLng = 0;

    bool initialPositionSet = false;
    double curLat = 0, curLng = 0, distance = 0;

    String filename;
    // Fixed-size, statically-allocated (zero heap) MAC dedup, replacing the
    // earlier std::set<uint64_t> approach. Even with small uint64_t keys,
    // std::set allocates/frees an individual tree node on every insert/
    // erase - that's still heap churn. ESP32 Marauder's wardriving feature
    // (justcallmekoko/ESP32Marauder, src/WiFiScan.cpp) uses a genuinely
    // fixed array with zero ongoing heap allocation instead, sized exactly
    // 100 entries for non-PSRAM boards (matches this exact hardware) vs
    // 500 with PSRAM - that sizing choice is deliberately copied here.
    // Trade-off: this is a bounded recent-history window, not true
    // unlimited-lifetime dedup - once 100 new networks have been seen, the
    // oldest entries get overwritten and could theoretically be re-logged
    // if encountered again much later. Given the alternative was a
    // confirmed (if slow) heap leak, this trade-off is the right call.
    static constexpr int WIFI_MAC_HISTORY_LEN = 100;
    static constexpr int BLE_MAC_HISTORY_LEN = 100;
    uint64_t wifiMacHistory_[WIFI_MAC_HISTORY_LEN] = {0};
    uint64_t bleMacHistory_[BLE_MAC_HISTORY_LEN] = {0};
    int wifiMacCursor_ = 0;
    int bleMacCursor_ = 0;
    uint32_t totalWifiSeenCount_ = 0; // cumulative count, NOT capped by array size - for the UI/summary "total found" display
    uint32_t totalBleSeenCount_ = 0;

    bool seenWifiMac(uint64_t key) const {
        for (int i = 0; i < WIFI_MAC_HISTORY_LEN; i++) if (wifiMacHistory_[i] == key) return true;
        return false;
    }
    void saveWifiMac(uint64_t key) {
        wifiMacHistory_[wifiMacCursor_] = key;
        wifiMacCursor_ = (wifiMacCursor_ + 1) % WIFI_MAC_HISTORY_LEN;
        totalWifiSeenCount_++;
    }
    bool seenBleMac(uint64_t key) const {
        for (int i = 0; i < BLE_MAC_HISTORY_LEN; i++) if (bleMacHistory_[i] == key) return true;
        return false;
    }
    void saveBleMac(uint64_t key) {
        bleMacHistory_[bleMacCursor_] = key;
        bleMacCursor_ = (bleMacCursor_ + 1) % BLE_MAC_HISTORY_LEN;
        totalBleSeenCount_++;
    }
    static constexpr int MAX_DISPLAY_RECORDS = 12;
    DisplayRecord displayRecords_[MAX_DISPLAY_RECORDS];
    int displayCount_ = 0;
    // Consider a record for the on-screen "strongest 12" list without any
    // heap allocation - inserts into the fixed array in RSSI order, evicting
    // the weakest if full. Replaces the old push_back/sort/resize/clear
    // cycle on a String-heavy vector.
    void considerForDisplay(const char *label, const char *type, int32_t rssi, bool isBle, char secChar);
    void clearDisplay() { displayCount_ = 0; }
    int lastScanCount_ = 0;
    uint32_t lastWifiMs_ = 0;
    uint32_t lastBleMs_ = 0;
    uint32_t lastCycleMs_ = 0;
    int openCount_ = 0, wepCount_ = 0, wpaCount_ = 0, wpa3Count_ = 0, enterpriseCount_ = 0, unknownAuthCount_ = 0;
    int newBleCount_ = 0;
    uint32_t lastSessionDurationMs_ = 0;
    uint32_t cycleNumber_ = 0;
    uint32_t lastDiagLogMs_ = 0;

    NimBLEScan *bleScan = nullptr;
    bool bleInitialized = false;
    int bleCycleCount_ = 0;

    // --- Promiscuous sniffer mode state ---
    bool snifferMode = false;
    File sniffFile_; // kept open for the whole session instead of per-cycle, since there are no discrete cycles in sniffer mode
    uint32_t snifferProcessedCount_ = 0;
    uint32_t lastChannelHopMs_ = 0;
    int channelHopIndex_ = 0;
    static constexpr uint32_t CHANNEL_HOP_INTERVAL_MS = 300;
    // 2.4GHz channels 1-13 always included. 5GHz channels only hopped when
    // in DualBand mode - this list covers common U-NII band channels.
    static const uint8_t CHANNELS_24[13];
    static const uint8_t CHANNELS_5[9];
    void startSniffer();
    void stopSniffer();
    void hopChannelIfDue();
    void drainSnifferQueue();
    void processSnifferBeacon(const PromiscBeacon &b);
    bool lowMemoryStop_ = false;
    static constexpr uint32_t LOW_MEMORY_STOP_THRESHOLD = 50000; // below this, full restart rather than risk a crash (crash was observed at ~34K, so this leaves real margin)
    static constexpr uint32_t WIFI_INPLACE_RESET_THRESHOLD = 75000; // try the riskier in-place WiFi reset before falling back to a full restart
    bool wifiInplaceResetAttempted_ = false; // only try once per "low heap episode" - if it didn't help, don't loop on it repeatedly
    static constexpr uint32_t BLE_RESET_HEAP_THRESHOLD = 70000; // force a BLE stack reset if free heap drops below this
    void (*bootStatusCb)(const char *) = nullptr;
    volatile bool stopRequested = false;
    static constexpr uint32_t BLE_SCAN_MS = 1500; // one steady blocking window per cycle - proven reliable, unlike chunked/async attempts

    std::set<String> alertAddresses; // lowercased MACs/addresses to watch for
    int alertCount_ = 0;
    String lastAlertMessage_ = "";

    void   feedGps();
    void   runScanCycle();
    void   scanWifi(File &f);
    int    processWifiScanResults(File &f, int n); // shared by both dual-band and 2.4GHz-only paths
    void   scanBle(File &f);
    void   writeRecordToCsv(File &f, const ApRecord &rec);
    void   createFilename();
    void   writeCsvHeaderIfNew(File &f, bool isNew);
    String authModeToString(wifi_auth_mode_t mode);
    void   loadAlertList();
    void   checkAlert(const String &address, const String &type, const String &label);
    // Packs "AA:BB:CC:DD:EE:FF" into a 48-bit integer for memory-efficient
    // dedup (avoids storing hundreds/thousands of heap-allocated Strings).
    static uint64_t macToUint64(const String &mac) {
        uint64_t v = 0;
        int hexDigits = 0;
        for (size_t i = 0; i < mac.length() && hexDigits < 12; i++) {
            char c = mac[i];
            int nibble;
            if (c >= '0' && c <= '9') nibble = c - '0';
            else if (c >= 'A' && c <= 'F') nibble = 10 + (c - 'A');
            else if (c >= 'a' && c <= 'f') nibble = 10 + (c - 'a');
            else continue;
            v = (v << 4) | nibble;
            hexDigits++;
        }
        return v;
    }
    // (Old std::set-based safety cap removed - the fixed-size arrays above
    // make this concept obsolete; they literally cannot grow unbounded.)
    String gpsStatusString() const {
        if (gpsHasFix()) return "FRESH";
        if (hasUsableFix()) return "STALE";
        return "NONE";
    }
    // Single-char security classification for the live display list.
    static char authModeToChar(const String &authMode) {
        if (authMode == "OPEN") return 'O';
        if (authMode == "WEP") return 'W';
        if (authMode == "WPA3_PSK" || authMode == "WPA2_WPA3_PSK") return '3';
        if (authMode == "WPA2_ENTERPRISE") return 'E';
        if (authMode == "WPA_PSK" || authMode == "WPA2_PSK" || authMode == "WPA_WPA2_PSK") return 'P';
        return 'U';
    }
    // Sanitizes a SSID/BLE-device-name string before it ever reaches a CSV
    // line. Two real, confirmed problems this fixes:
    // 1. Device names can legitimately contain a literal '"' character (e.g.
    //    smart TVs advertising as `85" Crystal UHD`) - without escaping,
    //    this breaks CSV quote-balance and can misalign fields for every
    //    subsequent row until another quote happens to "close" it again.
    //    Fix: double up embedded quotes, the standard CSV escape convention.
    // 2. BLE advertisement name fields can contain outright binary garbage
    //    (confirmed: a raw 0xFF byte, not valid text at all - likely
    //    manufacturer-specific data misread as a name). Fix: strip any
    //    byte outside printable ASCII range entirely.
    static String sanitizeForCsv(const String &input) {
        String out;
        out.reserve(input.length() + 4);
        for (size_t i = 0; i < input.length(); i++) {
            uint8_t c = (uint8_t)input[i];
            if (c < 32 || c > 126) continue; // strip non-printable/non-ASCII bytes entirely
            if (c == '"') { out += "\"\""; continue; } // CSV-escape embedded quotes
            out += (char)c;
        }
        return out;
    }
};
