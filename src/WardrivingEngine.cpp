// WardrivingEngine.cpp
#include "WardrivingEngine.h"
#include <esp_heap_caps.h>
#include <esp_bt.h>
#include <esp_wifi.h>

WardrivingEngine::WardrivingEngine(int gpsRxPin, int gpsTxPin, uint32_t gpsBaud)
    : gpsSerial(1), rxPin(gpsRxPin), txPin(gpsTxPin), baud(gpsBaud) {}

const uint8_t WardrivingEngine::CHANNELS_24[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
const uint8_t WardrivingEngine::CHANNELS_5[9] = {36, 40, 44, 48, 149, 153, 157, 161, 165};

void WardrivingEngine::startSniffer() {
    Serial.println("[sniffer] starting promiscuous-mode sniffer");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    snifferProcessedCount_ = 0;
    PromiscSniffer::droppedCount = 0;
    channelHopIndex_ = 0;
    lastChannelHopMs_ = millis();

    uint8_t firstChannel = (scanBand == ScanBand::DualBand) ? CHANNELS_24[0] : CHANNELS_24[0];
    PromiscSniffer::setChannel(firstChannel);
    PromiscSniffer::start();

    // Open one file for the whole session - there are no discrete scan
    // cycles in sniffer mode, beacons arrive continuously as they're heard.
    if (filename == "") createFilename();
    if (!SD.exists("/wardrive")) SD.mkdir("/wardrive");
    String path = "/wardrive/" + filename;
    bool isNew = !SD.exists(path);
    sniffFile_ = SD.open(path, isNew ? FILE_WRITE : FILE_APPEND);
    if (sniffFile_) writeCsvHeaderIfNew(sniffFile_, isNew);
}

void WardrivingEngine::stopSniffer() {
    Serial.println("[sniffer] stopping - processed=" + String(snifferProcessedCount_) +
                    " dropped=" + String(PromiscSniffer::droppedCount));
    PromiscSniffer::stop();
    if (sniffFile_) sniffFile_.close();
}

void WardrivingEngine::hopChannelIfDue() {
    if (millis() - lastChannelHopMs_ < CHANNEL_HOP_INTERVAL_MS) return;
    lastChannelHopMs_ = millis();

    channelHopIndex_++;
    int total24 = 13;
    int total5 = (scanBand == ScanBand::DualBand) ? 9 : 0;
    int totalChannels = total24 + total5;
    int idx = channelHopIndex_ % totalChannels;

    uint8_t ch = (idx < total24) ? CHANNELS_24[idx] : CHANNELS_5[idx - total24];
    PromiscSniffer::setChannel(ch);
}

void WardrivingEngine::drainSnifferQueue() {
    PromiscBeacon b;
    int processedThisTick = 0;
    // Cap how many we process per call so a sudden burst of frames doesn't
    // turn into a long blocking stretch - remaining items just wait for the
    // next tick instead.
    while (processedThisTick < 10 && PromiscSniffer::poll(b)) {
        processSnifferBeacon(b);
        processedThisTick++;
    }
}

void WardrivingEngine::processSnifferBeacon(const PromiscBeacon &b) {
    char bssidStr[18];
    snprintf(bssidStr, sizeof(bssidStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             b.bssid[0], b.bssid[1], b.bssid[2], b.bssid[3], b.bssid[4], b.bssid[5]);
    String bssid = bssidStr;

    uint64_t bssidKey = macToUint64(bssid);
    snifferProcessedCount_++;

    if (!seenWifiMac(bssidKey)) {
        saveWifiMac(bssidKey);

        String ssid = b.ssidLen ? sanitizeForCsv(String(b.ssid)) : "";
        // Note: simplified open-vs-encrypted only (see PromiscSniffer.h
        // caveat) - full WPA/WPA2/WPA3 differentiation would need RSN
        // information-element parsing, not implemented in this v1.
        String authMode = b.isOpen ? "OPEN" : "UNKNOWN";

        if (sniffFile_) {
            ApRecord rec{
                bssid, ssid, authMode, b.channel, b.rssi,
                gps.location.lat(), gps.location.lng(), "WIFI",
                gpsStatusString()
            };
            writeRecordToCsv(sniffFile_, rec);
            checkAlert(bssid, "WiFi", ssid);
        }

        if (b.isOpen) openCount_++;
        else unknownAuthCount_++; // can't tell WPA/WPA2/WPA3 apart yet - bucketed as unknown rather than guessing

        considerForDisplay(ssid.length() ? ssid.c_str() : "(hidden)", "WIFI", b.rssi, false,
                            b.isOpen ? 'O' : 'U');
    }
}

bool WardrivingEngine::begin() {
    auto report = [this](const char *msg) {
        Serial.println(msg);
        if (bootStatusCb) bootStatusCb(msg);
    };

    report("[engine] gpsSerial.begin()...");
    // Larger RX buffer than the default (~256 bytes) - tolerates longer
    // gaps without draining (blocking WiFi/BLE scans, the periodic BLE
    // stack reset) without overflowing and corrupting/dropping bytes,
    // which shows up as rising checksum failures. Must be set before begin().
    gpsSerial.setRxBufferSize(2048);
    gpsSerial.begin(baud, SERIAL_8N1, rxPin, txPin);
    report("[engine] gpsSerial.begin() OK");

    report("[engine] WiFi.mode(STA)...");
    WiFi.mode(WIFI_STA);
    report("[engine] WiFi.mode() OK");

    report("[engine] WiFi.disconnect()...");
    WiFi.disconnect();
    report("[engine] WiFi.disconnect() OK");

    report("[engine] loadAlertList()...");
    loadAlertList();
    report("[engine] loadAlertList() OK");

    return true;
}

void WardrivingEngine::startSession() {
    Serial.println("[scan] session started");
    sessionActive = true;
    stopRequested = false;
    wifiInplaceResetAttempted_ = false;
    sessionStartMs = millis();
    lastScanMs = 0;
    initialPositionSet = false;
    distance = 0;
    filename = "";
    // NOTE: the Wi-Fi/BLE MAC history arrays are deliberately NOT reset
    // here. They persist across multiple Start/Stop cycles within the same
    // power-on session. (They're now fixed-size rolling-history arrays,
    // not unbounded sets - see header comment for the Marauder-derived
    // rationale - so "persisting" just means the rolling window keeps
    // going rather than restarting empty.)
    clearDisplay();
    lastScanCount_ = 0;
    // Per-run encryption breakdown DOES reset - the summary screen reports
    // "what did THIS run find that was new", not a lifetime total.
    openCount_ = wepCount_ = wpaCount_ = wpa3Count_ = enterpriseCount_ = unknownAuthCount_ = 0;
    newBleCount_ = 0;

    if (snifferMode) startSniffer();
}

void WardrivingEngine::stopSession() {
    Serial.println("[scan] stop requested");
    stopRequested = true;
    lastSessionDurationMs_ = millis() - sessionStartMs; // snapshot before elapsedMs() gates to 0
    sessionActive = false;

    if (snifferMode) stopSniffer();

    WiFi.scanDelete();
    if (bleInitialized && bleScan != nullptr && bleScan->isScanning()) {
        bleScan->stop();
        Serial.println("[scan] ble scan stopped");
    }
    Serial.println("[scan] scan state cleared");

    // --- Heap reclaim pass ---
    // Safe to do thoroughly here, unlike the mid-scan reset attempts that
    // risked race conditions with in-flight radio operations - scanning has
    // already fully stopped by this point, so there's nothing concurrent to
    // conflict with.
    uint32_t heapBefore = ESP.getFreeHeap();

    // Fully power down Wi-Fi while idle - no reason to keep the radio
    // initialized between sessions, and this releases whatever internal
    // driver buffers it's holding.
    WiFi.mode(WIFI_OFF);

    // Fully tear down BLE if it was used, using the C5-specific raw IDF
    // call (validated against Bruce firmware's own C5 workaround) rather
    // than the NimBLE-Arduino wrapper, which is less trustworthy on this
    // chip based on earlier testing.
    if (bleInitialized) {
#if defined(CONFIG_IDF_TARGET_ESP32C5)
        esp_bt_controller_deinit();
#else
        NimBLEDevice::deinit(true);
#endif
        bleInitialized = false;
        bleScan = nullptr;
    }

    // NOTE: previously cleared the dedup caches here to reclaim heap they
    // were holding. No longer needed/meaningful - they're fixed-size
    // static arrays now (see header), so there's no heap to reclaim by
    // clearing them. Leaving them alone here also means the rolling MAC
    // history naturally continues across Start/Stop cycles instead of
    // restarting empty, which is a nice side benefit.

    Serial.println("[scan] heap reclaim pass complete - before=" + String(heapBefore) +
                    " after=" + String(ESP.getFreeHeap()));
}

bool WardrivingEngine::update() {
    feedGps();

    if (!sessionActive) return false;

    // Try a less drastic in-place WiFi reset first, before falling back to
    // a full device restart. This carries real risk - it's the same
    // category of operation (deinit/reinit while the radio stack's
    // internal tasks might still be doing background work) as an earlier
    // BLE attempt that may have caused its own crash. Only attempted once
    // per "low heap episode" (reset by a successful new session) so a
    // failed attempt doesn't loop forever - if it doesn't help, the full
    // restart below still catches it.
    // SKIPPED in sniffer mode - esp_wifi_deinit() would also tear down the
    // promiscuous-mode registration, and partially resetting that mid-
    // session is a different, un-tested risk on top of the one this was
    // already meant to mitigate. The whole point of sniffer mode is to
    // avoid needing this kind of reset in the first place.
    if (!snifferMode && ESP.getFreeHeap() < WIFI_INPLACE_RESET_THRESHOLD && !wifiInplaceResetAttempted_) {
        wifiInplaceResetAttempted_ = true;
        uint32_t heapBefore = ESP.getFreeHeap();
        Serial.println("[scan] attempting in-place WiFi reset. freeHeap before=" + String(heapBefore));

        esp_wifi_stop();
        esp_wifi_deinit();
        delay(150);

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        WiFi.disconnect();
        delay(150);

        Serial.println("[scan] in-place WiFi reset complete. freeHeap after=" + String(ESP.getFreeHeap()));
        // Fall through to the normal interval check below - if this crashes
        // instead, you'll see the device reboot here with no further log,
        // which itself is useful information (tells us this approach isn't
        // safe and we should stick to the full-restart-only approach).
    }

    // Safety net: a real memory leak exists somewhere in the Wi-Fi (and/or
    // BLE) scan path on this chip/library combination that I haven't been
    // able to pin down or fix at the application level. Rather than let it
    // run until a hard crash/reboot, stop the session cleanly before things
    // get critical - this preserves whatever was already logged to SD and
    // gives a clear, predictable reason instead of a silent crash.
    if (ESP.getFreeHeap() < LOW_MEMORY_STOP_THRESHOLD) {
        Serial.println("[scan] LOW MEMORY - self-restarting to reclaim memory. freeHeap=" + String(ESP.getFreeHeap()));
        // A full reboot unconditionally reclaims everything, unlike a
        // partial in-place reset (e.g. the earlier NimBLE deinit/reinit
        // attempt) which risked its own race conditions. Save a flag so
        // the next boot automatically resumes scanning into a new file,
        // making this look like a brief blip rather than something the
        // user has to notice and manually restart.
        Preferences p;
        p.begin("wardrive_eng", false);
        p.putBool("resume", true);
        p.end();
        delay(200);
        ESP.restart();
    }

    if (snifferMode) {
        // Continuous passive listening - drain whatever beacons arrived
        // and hop to the next channel, every single tick. No discrete
        // "cycle" here, so the old SCAN_INTERVAL gating doesn't apply to
        // Wi-Fi at all anymore.
        drainSnifferQueue();
        hopChannelIfDue();

        // BLE still uses the old periodic blocking-scan approach (separate
        // problem, unrelated to this sniffer change) - reuse the existing
        // interval timer just for that, if enabled.
        if (bleEnabled && millis() - lastScanMs >= scanIntervalForCurrentBand()) {
            lastScanMs = millis();
            if (sniffFile_) {
                uint32_t bleStart = millis();
                scanBle(sniffFile_);
                lastBleMs_ = millis() - bleStart;
            }
            return true; // let the UI know something happened, for redraw purposes
        }
        return false;
    }

    if (millis() - lastScanMs < scanIntervalForCurrentBand()) return false;

    // No GPS gate here at all - scanning runs regardless of fix status.
    // Each logged record is tagged with the GPS state at capture time
    // (fresh / stale / never-acquired) via writeRecordToCsv().
    lastScanMs = millis();
    runScanCycle();
    return true;
}

void WardrivingEngine::feedGps() {
    while (gpsSerial.available() > 0) {
        gps.encode(gpsSerial.read());
    }
    if (gps.location.isUpdated() && gps.location.isValid()) {
        double lat = gps.location.lat();
        double lng = gps.location.lng();

        // Sanity checks before trusting this update at all. A high
        // checksum-failure rate (UART noise) means an occasional corrupted
        // sentence can still slip past TinyGPS++'s own isValid() check with
        // numerically-garbage coordinates - without a bound, a single bad
        // reading can add a huge bogus one-time jump to distance that can
        // never be undone. (0,0) specifically is TinyGPS++'s "never had a
        // fix" sentinel, not a real location anyone is actually at.
        bool looksValid = !(lat == 0.0 && lng == 0.0);

        if (looksValid && initialPositionSet) {
            double jump = gps.distanceBetween(curLat, curLng, lat, lng);
            uint32_t elapsedMs = millis() - lastDistanceUpdateMs_;
            // Generous bound: ~360 km/h (100 m/s) sustained between updates
            // would already be implausible for any real wardriving vehicle -
            // anything faster than that is treated as a corrupted reading,
            // not real movement, and rejected rather than accumulated.
            double maxPlausibleMeters = (elapsedMs / 1000.0) * 100.0 + 10.0; // +10m floor for very short intervals
            if (jump > maxPlausibleMeters) {
                Serial.println("[gps] rejected implausible jump: " + String(jump, 1) +
                                "m in " + String(elapsedMs) + "ms (checksum noise?)");
                looksValid = false;
            }
        }

        if (looksValid) {
            if (initialPositionSet) {
                distance += gps.distanceBetween(curLat, curLng, lat, lng);
            } else {
                initialPositionSet = true;
            }
            curLat = lat;
            curLng = lng;
            lastDistanceUpdateMs_ = millis();

            // Record this as the last-known-good fix, independent of the tight
            // gpsHasFix() freshness window. A single missed/garbled sentence
            // afterward won't erase this — it just won't refresh it until the
            // next valid update.
            lastValidFixMs = millis();
            lastValidLat = lat;
            lastValidLng = lng;
        }
    }
}

void WardrivingEngine::createFilename() {
    char buf[40];
    snprintf(
        buf, sizeof(buf), "%02d%02d%02d_%02d%02d%02d_wardrive.csv",
        gps.date.year() % 100, gps.date.month(), gps.date.day(),
        gps.time.hour(), gps.time.minute(), gps.time.second()
    );
    filename = String(buf);
}

void WardrivingEngine::writeCsvHeaderIfNew(File &f, bool isNew) {
    if (!isNew) return;
    f.println(
        "WigleWifi-1.6,appRelease=0.2,model=NM-CYD-C5,release=0.2,"
        "device=ESP32-C5,display=ST7789,board=NM-CYD-C5,brand=DIY,star=Sol,body=4,subBody=1"
    );
    f.println(
        "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,"
        "AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type"
    );
}

// Shared WiGLE-row writer for both Wi-Fi and BLE records. Frequency is left
// blank for BLE rows (matches WiGLE's own convention for Bluetooth entries).
void WardrivingEngine::considerForDisplay(const char *label, const char *type, int32_t rssi, bool isBle, char secChar) {
    // Find insertion point (keep sorted strongest-first). All operations are
    // on a fixed-size stack-free array - no heap allocation whatsoever.
    if (displayCount_ < MAX_DISPLAY_RECORDS) {
        int idx = displayCount_;
        while (idx > 0 && displayRecords_[idx - 1].rssi < rssi) {
            displayRecords_[idx] = displayRecords_[idx - 1];
            idx--;
        }
        strncpy(displayRecords_[idx].label, label, sizeof(displayRecords_[idx].label) - 1);
        displayRecords_[idx].label[sizeof(displayRecords_[idx].label) - 1] = '\0';
        strncpy(displayRecords_[idx].type, type, sizeof(displayRecords_[idx].type) - 1);
        displayRecords_[idx].type[sizeof(displayRecords_[idx].type) - 1] = '\0';
        displayRecords_[idx].rssi = rssi;
        displayRecords_[idx].isBle = isBle;
        displayRecords_[idx].secChar = secChar;
        displayCount_++;
    } else if (rssi > displayRecords_[MAX_DISPLAY_RECORDS - 1].rssi) {
        // Stronger than our weakest - evict weakest, insert in order
        int idx = MAX_DISPLAY_RECORDS - 1;
        while (idx > 0 && displayRecords_[idx - 1].rssi < rssi) {
            displayRecords_[idx] = displayRecords_[idx - 1];
            idx--;
        }
        strncpy(displayRecords_[idx].label, label, sizeof(displayRecords_[idx].label) - 1);
        displayRecords_[idx].label[sizeof(displayRecords_[idx].label) - 1] = '\0';
        strncpy(displayRecords_[idx].type, type, sizeof(displayRecords_[idx].type) - 1);
        displayRecords_[idx].type[sizeof(displayRecords_[idx].type) - 1] = '\0';
        displayRecords_[idx].rssi = rssi;
        displayRecords_[idx].isBle = isBle;
        displayRecords_[idx].secChar = secChar;
    }
}

void WardrivingEngine::writeRecordToCsv(File &f, const ApRecord &rec) {
    char timeBuf[24];
    snprintf(
        timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d",
        gps.date.year(), gps.date.month(), gps.date.day(),
        gps.time.hour(), gps.time.minute(), gps.time.second()
    );

    // WiGLE's AccuracyMeters column expects an actual accuracy estimate IN
    // METERS, not raw HDOP (a small unitless dilution-of-precision ratio,
    // typically 1.0-3.0 for a good fix). Writing HDOP directly there was a
    // real bug: it both understates true accuracy (claiming e.g. "1.2
    // meters" when real accuracy is more like 6m) AND, if HDOP was never
    // actually valid (GPS module not outputting GPGSA, or no satellites
    // used in the fix yet), TinyGPS++ returns an invalid sentinel value
    // that got written blindly with no check at all - very plausibly
    // explaining confidently-placed-looking records that WiGLE still
    // refused to credit, despite a perfectly good fix and satellite count.
    double accuracyMeters;
    if (gps.hdop.isValid() && gps.hdop.hdop() < 50.0) {
        // Rough standard approximation: AccuracyMeters ~= HDOP * baseline UERE (~5m for civilian GPS)
        accuracyMeters = gps.hdop.hdop() * 5.0;
    } else {
        accuracyMeters = 15.0; // sane default for "don't actually know" rather than a garbage/sentinel value
    }

    char line[400];
    if (rec.type == "BLE") {
        snprintf(
            line, sizeof(line),
            "%s,\"%s\",%s,%s,0,,%ld,%f,%f,%f,%f,%s,,%s\r\n",
            rec.bssid.c_str(), rec.ssid.c_str(), rec.authMode.c_str(), timeBuf,
            (long)rec.rssi, rec.lat, rec.lng, gps.altitude.meters(), accuracyMeters,
            rec.gpsStatus.c_str(), rec.type.c_str()
        );
    } else {
        long freq = rec.channel != 14 ? 2407 + (long)rec.channel * 5 : 2484;
        snprintf(
            line, sizeof(line),
            "%s,\"%s\",[%s],%s,%ld,%ld,%ld,%f,%f,%f,%f,%s,,%s\r\n",
            rec.bssid.c_str(), rec.ssid.c_str(), rec.authMode.c_str(), timeBuf,
            (long)rec.channel, freq, (long)rec.rssi, rec.lat, rec.lng,
            gps.altitude.meters(), accuracyMeters, rec.gpsStatus.c_str(), rec.type.c_str()
        );
    }
    f.print(line);
}

String WardrivingEngine::authModeToString(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA_PSK";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2_PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA_WPA2_PSK";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3_PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2_WPA3_PSK";
        default:                        return "UNKNOWN";
    }
}

void WardrivingEngine::runScanCycle() {
    if (filename == "") createFilename();

    if (!SD.exists("/wardrive")) SD.mkdir("/wardrive");
    String path = "/wardrive/" + filename;
    bool isNew = !SD.exists(path);
    File f = SD.open(path, isNew ? FILE_WRITE : FILE_APPEND);
    if (!f) {
        Serial.println("[scan] SD open failed, skipping this cycle");
        return;
    }

    writeCsvHeaderIfNew(f, isNew);
    clearDisplay();

    uint32_t cycleStart = millis();
    cycleNumber_++;

    // Periodic diagnostics - every 30s while actively scanning, not every
    // cycle, to avoid spamming Serial. This is the data needed to catch a
    // slow memory leak before it causes a crash, rather than only after.
    if (millis() - lastDiagLogMs_ > 30000) {
        lastDiagLogMs_ = millis();
        Serial.println("[diag] cycle=" + String(cycleNumber_) +
                        " freeHeap=" + String(ESP.getFreeHeap()) +
                        " minFreeHeap=" + String(ESP.getMinFreeHeap()) +
                        " largestFreeBlock=" + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) +
                        " wifiAPs=" + String(totalWifiSeenCount_) +
                        " bleDevices=" + String(totalBleSeenCount_) +
                        " uptimeMs=" + String(millis()));
    }

    Serial.println("[scan] SCAN STARTED at t=" + String(cycleStart));

    uint32_t wifiStart = millis();
    scanWifi(f);
    uint32_t wifiMs = millis() - wifiStart;
    lastWifiMs_ = wifiMs;
    Serial.println("[scan] wifi scan complete, n=" + String(lastScanCount_) + ", took " + String(wifiMs) + "ms");

    if (!stopRequested && bleEnabled) {
        uint32_t bleStart = millis();
        scanBle(f);
        uint32_t bleMs = millis() - bleStart;
        lastBleMs_ = bleMs;
        Serial.println("[scan] ble scan complete, took " + String(bleMs) + "ms");
    } else {
        lastBleMs_ = 0;
    }

    f.close();

    // (Display list is already kept sorted strongest-first and capped at
    // MAX_DISPLAY_RECORDS by considerForDisplay() as records come in - no
    // post-cycle sort/resize needed, and crucially no heap churn.)

    uint32_t totalMs = millis() - cycleStart;
    lastCycleMs_ = totalMs;
    Serial.println("[scan] CYCLE COMPLETE - total blocking time: " + String(totalMs) + "ms");

    // Lightweight BLE-only reclaim during the idle gap before the next
    // cycle. Safe here too - no radio operation is in flight once we've
    // reached this point. Deliberately NOT clearing the WiFi/BLE dedup
    // caches here (unlike the full reclaim on Stop) - doing that every
    // cycle would re-log every already-seen network constantly, bloating
    // the CSV with duplicates. This only tears down/reinitializes the BLE
    // stack itself to release whatever it's holding internally.
    if (bleEnabled && bleInitialized) {
        uint32_t heapBefore = ESP.getFreeHeap();
#if defined(CONFIG_IDF_TARGET_ESP32C5)
        esp_bt_controller_deinit();
#else
        NimBLEDevice::deinit(true);
#endif
        bleInitialized = false;
        bleScan = nullptr;
        Serial.println("[scan] mid-session BLE reclaim - before=" + String(heapBefore) +
                        " after=" + String(ESP.getFreeHeap()));
        // scanBle() already re-initializes BLE from scratch via its
        // `if (!bleInitialized)` block, so the next cycle picks this up
        // automatically with no other change needed.
    }
}

void WardrivingEngine::loadAlertList() {
    alertAddresses.clear();
    if (!SD.exists("/wardrive")) SD.mkdir("/wardrive");

    if (SD.exists("/wardrive/alert.txt")) {
        File f = SD.open("/wardrive/alert.txt", FILE_READ);
        if (f) {
            while (f.available()) {
                String line = f.readStringUntil('\n');
                line.trim();
                if (line.length() > 0 && !line.startsWith("#")) {
                    line.toLowerCase();
                    alertAddresses.insert(line);
                }
            }
            f.close();
        }
    } else {
        File f = SD.open("/wardrive/alert.txt", FILE_WRITE);
        if (f) {
            f.println("# Alert MAC/BLE addresses to watch for, one per line.");
            f.println("# Lines starting with # are ignored.");
            f.println("# Example: aa:bb:cc:dd:ee:ff");
            f.close();
        }
    }
}

void WardrivingEngine::checkAlert(const String &address, const String &type, const String &label) {
    if (alertAddresses.empty()) return;
    String addrLower = address;
    addrLower.toLowerCase();
    if (alertAddresses.find(addrLower) == alertAddresses.end()) return;

    alertCount_++;
    lastAlertMessage_ = type + " ALERT: " + (label.length() ? label + " " : "") + address;
}

// Proven blocking Wi-Fi scan. This is the same call type that worked
// reliably before an earlier async-scan attempt introduced an indefinite
// hang on this board/framework - reverted back deliberately.
void WardrivingEngine::scanWifi(File &f) {
    // NOTE: previously toggled WiFi.mode(WIFI_OFF)/(WIFI_STA) around every
    // scan, theorizing a full radio reset might help the leak. Reverted -
    // Bruce firmware (known to run stably with this same library stack)
    // keeps WiFi continuously in STA mode for the whole session and never
    // cycles it between scans. Repeatedly tearing down/reinitializing the
    // driver's internal state every cycle, especially on this chip's
    // immature driver, is more likely to cause churn than prevent it.
    if (scanBand == ScanBand::DualBand) {
        // Matching Bruce's exact call here - WiFi.scanNetworks() with no
        // arguments at all, not even show_hidden=true. Never actually
        // tested removing this parameter before; it's the last simple,
        // concrete difference left after the fragmentation fix.
        int n = WiFi.scanNetworks();
        lastScanCount_ = processWifiScanResults(f, n);
        WiFi.scanDelete();
        vTaskDelay(120 / portTICK_PERIOD_MS); // let cleanup actually settle before reusing the radio - matches Bruce firmware's pattern
    } else {
        // 2.4GHz only: scan channels 1-13 individually. WiFi.scanNetworks()
        // has no simple "band" switch, but its channel parameter restricts
        // a single call to one channel - looping over just the 2.4GHz
        // channels skips all 5GHz channels entirely (the main source of the
        // long dual-band scan time), giving much faster cycles.
        int total = 0;
        for (int ch = 1; ch <= 13; ch++) {
            if (stopRequested) break;
            int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false,
                                       /*passive=*/false, /*max_ms_per_chan=*/120, ch);
            total += processWifiScanResults(f, n);
            WiFi.scanDelete();
            vTaskDelay(30 / portTICK_PERIOD_MS); // shorter than the dual-band delay since this repeats 13x
        }
        lastScanCount_ = total;
    }
}

int WardrivingEngine::processWifiScanResults(File &f, int n) {
    for (int i = 0; i < n; i++) {
        if (stopRequested) break;

        String bssid = WiFi.BSSIDstr(i);
        int32_t channel = WiFi.channel(i);
        int32_t rssi = WiFi.RSSI(i);
        String ssid = sanitizeForCsv(WiFi.SSID(i));
        String authMode = authModeToString(WiFi.encryptionType(i));
        char secChar = authModeToChar(authMode);

        uint64_t bssidKey = macToUint64(bssid);
        if (!seenWifiMac(bssidKey)) {
            saveWifiMac(bssidKey);

            // Build the full record only here, where it's actually needed
            // for the CSV write - not for every sighting.
            ApRecord rec{
                bssid, ssid, authMode,
                channel, rssi, gps.location.lat(), gps.location.lng(), "WIFI",
                gpsStatusString()
            };
            writeRecordToCsv(f, rec);
            checkAlert(bssid, "WiFi", ssid);

            // Bucket for the run-summary screen
            switch (secChar) {
                case 'O': openCount_++; break;
                case 'W': wepCount_++; break;
                case '3': wpa3Count_++; break;
                case 'E': enterpriseCount_++; break;
                case 'P': wpaCount_++; break;
                default: unknownAuthCount_++; break;
            }
        }

        // Heap-free display update - shows security type for every visible
        // row, not just newly-discovered ones, so already-seen networks
        // still display correctly while you're scrolled past them.
        considerForDisplay(ssid.length() ? ssid.c_str() : "(hidden)", "WIFI", rssi, false, secChar);

        if ((i & 0x0F) == 0) vTaskDelay(1);
    }
    return n;
}

// Proven blocking BLE scan window. Reverted from a rapid-chunking attempt
// that may have stressed this very new BLE stack with frequent start/stop
// cycles - one steady window is the call pattern known to work.
void WardrivingEngine::scanBle(File &f) {
    if (!bleInitialized) {
        NimBLEDevice::init("");
        bleScan = NimBLEDevice::getScan();
        bleScan->setActiveScan(true);
        bleScan->setInterval(100);
        bleScan->setWindow(99);
        bleInitialized = true;
    }

    // KNOWN ISSUE: BLE scanning leaks memory over time on this board/library
    // combination, confirmed via testing (heap declines steadily with BLE
    // on, stays stable with BLE off). Two mitigation attempts - a periodic
    // full BLE stack reset on a fixed cycle count, then the same reset
    // triggered by heap level - both still resulted in a crash at roughly
    // the same ~2 minute mark. The deinit()/init() reset itself may be
    // introducing a race condition with NimBLE's internal background task
    // (it does a lot of its work off the main thread), which would produce
    // the same symptom (hard reset) via a different cause. Rather than risk
    // a third unproven mitigation, BLE is disabled by default until a safer
    // fix is identified - Wi-Fi-only scanning has been stable throughout
    // all of this testing. Re-enable in Options at your own risk for now.
    if (ESP.getFreeHeap() < 60000) {
        Serial.println("[scan] BLE heap critically low (" + String(ESP.getFreeHeap()) +
                        ") - skipping this BLE scan to avoid crash");
        return;
    }

    if (bleScan->isScanning()) {
        bleScan->stop();
        vTaskDelay(50 / portTICK_PERIOD_MS); // settle before starting a fresh scan window
    }

    NimBLEScanResults results = bleScan->getResults(BLE_SCAN_MS, false);
    int count = results.getCount();

    for (int i = 0; i < count; i++) {
        if (stopRequested) break;

        const NimBLEAdvertisedDevice *device = results.getDevice(i);
        if (!device) continue;

        String address = device->getAddress().toString().c_str();
        String name = device->haveName() ? sanitizeForCsv(device->getName().c_str()) : "";
        int rssi = device->getRSSI();

        uint64_t addrKey = macToUint64(address);
        if (!seenBleMac(addrKey)) {
            saveBleMac(addrKey);

            // Build the full record only here, where it's actually needed
            // for the CSV write - not for every sighting.
            ApRecord rec{
                address, name, "Misc [BLE]", 0, rssi,
                gps.location.lat(), gps.location.lng(), "BLE",
                gpsStatusString()
            };
            writeRecordToCsv(f, rec);
            checkAlert(address, "BLE", name);
            newBleCount_++;
        }

        // Heap-free display update.
        considerForDisplay(name.length() ? name.c_str() : "(unnamed)", "BLE", rssi, true, '-');

        if ((i & 0x0F) == 0) vTaskDelay(1);
    }

    bleScan->clearResults();
    // Settle delay matching Bruce firmware's proven pattern (20ms when
    // devices were processed, 150ms on an empty scan) - gives the
    // underlying BLE stack's internal cleanup a chance to actually
    // complete before the next scan hammers it again.
    vTaskDelay((count == 0 ? 150 : 20) / portTICK_PERIOD_MS);
}
