// PromiscSniffer.h
//
// Passive 802.11 beacon/probe-response sniffer using ESP-IDF promiscuous
// mode, as an alternative to repeatedly calling WiFi.scanNetworks().
//
// WHY THIS EXISTS: extensive testing on this board confirmed the Wi-Fi
// memory leak scales directly with how many times WiFi.scanNetworks() /
// esp_wifi_scan_start() gets called - more calls per minute = faster leak,
// regardless of dedup strategy, fragmentation fixes, or settle delays
// (all already tried and partially helpful, but not a full fix). A
// promiscuous-mode sniffer registers ONE callback once, then passively
// receives frames forever as they arrive over the air - it never calls
// the leaking function repeatedly at all, which should eliminate this
// category of leak entirely rather than mitigating it.
//
// TRADE-OFF: we lose the convenience of WiFi.scanNetworks() parsing
// frames for us. This file parses raw 802.11 frames by hand. It also only
// sees APs that are actively beaconing/responding to probes while we
// happen to be tuned to their channel - channel hopping (below) covers
// this the same way any passive WiFi scanner does.
//
// CAVEAT: this is new, non-trivial code that hasn't been tested on real
// hardware yet (written and reasoned through, not flashed/verified). Frame
// parsing offsets are correct per the 802.11 spec for standard (non-QoS)
// beacon/probe-response frames, but real-world testing may turn up edge
// cases (malformed frames, vendor-specific quirks) needing follow-up fixes.

#pragma once

#include <Arduino.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct PromiscBeacon {
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    bool     isOpen;       // simplified: privacy bit from capability info.
                            // Distinguishes open vs encrypted, but does NOT
                            // differentiate WPA/WPA2/WPA3 (that needs deeper
                            // RSN information-element parsing, not done here -
                            // a known, deliberate scope limit for this v1).
    char     ssid[33];
    uint8_t  ssidLen;
};

namespace PromiscSniffer {

inline QueueHandle_t queue = nullptr;
constexpr int QUEUE_LENGTH = 32; // small and fixed - if the queue fills
                                  // up because the drain side is slower
                                  // than incoming frames, new frames are
                                  // just dropped (logged as missed, not a
                                  // crash or unbounded growth).
inline volatile uint32_t droppedCount = 0;

// Runs in the WiFi driver's internal task context, NOT a hardware ISR -
// must stay fast and must not allocate (no String, no SD, no display).
inline void IRAM_ATTR rxCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    if (len < 36) return; // shorter than a minimal beacon header - malformed/truncated, skip

    uint16_t frameControl = payload[0] | (payload[1] << 8);
    uint8_t frameType = (frameControl >> 2) & 0x3;
    uint8_t frameSubtype = (frameControl >> 4) & 0xF;

    // Management frame (type 0), beacon (subtype 8) or probe response (subtype 5)
    if (frameType != 0 || (frameSubtype != 8 && frameSubtype != 5)) return;

    PromiscBeacon b;
    memcpy(b.bssid, payload + 10, 6); // Address2 / transmitter address = BSSID for beacons
    b.rssi = pkt->rx_ctrl.rssi;
    b.channel = pkt->rx_ctrl.channel;

    // Beacon/probe-response body: Timestamp(8) + Interval(2) + Capability(2)
    // = 12 bytes, starting right after the 24-byte MAC header.
    uint16_t capability = payload[34] | (payload[35] << 8);
    b.isOpen = !(capability & 0x0010); // bit 4 = Privacy

    b.ssidLen = 0;
    b.ssid[0] = '\0';

    // Tagged parameters start at offset 36. Walk tags looking for Tag 0 (SSID).
    int pos = 36;
    int safetyLimit = len - 2; // leave room to read [tag][len] safely
    while (pos < safetyLimit && pos < 36 + 200) { // 200-byte cap - plenty for SSID, avoids runaway loop on malformed data
        uint8_t tagNum = payload[pos];
        uint8_t tagLen = payload[pos + 1];
        if (pos + 2 + tagLen > len) break; // tag claims more data than the frame has - stop, don't read out of bounds

        if (tagNum == 0) { // SSID element
            uint8_t copyLen = tagLen > 32 ? 32 : tagLen;
            memcpy(b.ssid, payload + pos + 2, copyLen);
            b.ssid[copyLen] = '\0';
            b.ssidLen = copyLen;
            break;
        }
        pos += 2 + tagLen;
    }

    if (queue) {
        if (xQueueSend(queue, &b, 0) != pdTRUE) {
            droppedCount = droppedCount + 1; // queue full - drop rather than block the WiFi task
        }
    }
}

inline bool start() {
    queue = xQueueCreate(QUEUE_LENGTH, sizeof(PromiscBeacon));
    if (!queue) return false;

    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(&rxCallback);
    esp_wifi_set_promiscuous(true);
    return true;
}

inline void stop() {
    esp_wifi_set_promiscuous(false);
    if (queue) {
        vQueueDelete(queue);
        queue = nullptr;
    }
}

inline void setChannel(uint8_t channel) {
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

// Non-blocking drain - call repeatedly from the main loop. Returns true and
// fills `out` if a beacon was waiting, false if the queue was empty.
inline bool poll(PromiscBeacon &out) {
    if (!queue) return false;
    return xQueueReceive(queue, &out, 0) == pdTRUE;
}

} // namespace PromiscSniffer
