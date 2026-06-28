// OuiTable.h
// Curated MAC OUI (first-3-octet) -> vendor name lookup.
//
// IMPORTANT SCOPE NOTE: this is a hand-curated subset (~100 entries) of
// common consumer/IoT vendors, NOT the full official IEEE OUI registry
// (which has 30,000+ entries). It will correctly identify many common
// router, phone, and smart-home devices, but plenty of legitimate vendors
// won't be in this table - those show as "Unknown" rather than a wrong
// guess. Treat matches as "likely vendor", not authoritative.

#pragma once

#include <Arduino.h>
#include <stdint.h>

struct OuiEntry {
    uint32_t prefix; // top 3 bytes as 0x AABBCC
    const char *vendor;
};

// Sorted ascending by prefix for binary search.
static const OuiEntry OUI_TABLE[] = {
    {0x000C29, "VMware"},
    {0x001A11, "Google"},
    {0x0024D7, "Cisco"},
    {0x00D861, "Espressif"},
    {0x086A0A, "Espressif"},
    {0x0C8C24, "Espressif"},
    {0x101539, "Espressif"},
    {0x14A78D, "Apple"},
    {0x183451, "Cisco"},
    {0x18FE34, "Espressif"},
    {0x1C5CF2, "Espressif"},
    {0x1CFE2B, "D-Link"},
    {0x205D47, "Apple"},
    {0x24A2E1, "Espressif"},
    {0x2C3AE8, "Espressif"},
    {0x2CF432, "Espressif"},
    {0x3C5AB4, "Google"},
    {0x3C6105, "Apple"},
    {0x40D32D, "Belkin"},
    {0x442B03, "Amazon"},
    {0x44650D, "Amazon"},
    {0x48D539, "Sonos"},
    {0x4C3B6A, "Roku"},
    {0x4CB199, "TP-Link"},
    {0x501A30, "Sony"},
    {0x509FA4, "Hewlett Packard"},
    {0x542696, "Espressif"},
    {0x5800E5, "Sonos"},
    {0x5C521E, "ASUS"},
    {0x5CCF7F, "Espressif"},
    {0x60018B, "Apple"},
    {0x609217, "Espressif"},
    {0x647002, "TP-Link"},
    {0x680571, "Apple"},
    {0x6C5697, "Sonos"},
    {0x6CB0CE, "Samsung"},
    {0x70B3D5, "Espressif"},
    {0x744DBD, "Apple"},
    {0x788C54, "Roku"},
    {0x7CD1C3, "TP-Link"},
    {0x84F3EB, "Espressif"},
    {0x8863DF, "Apple"},
    {0x909A4A, "Amazon"},
    {0x9C8E99, "Hewlett Packard"},
    {0xA0CEC8, "Apple"},
    {0xA42BB0, "TP-Link"},
    {0xA4C138, "Ring"},
    {0xA8BBCF, "Nest/Google"},
    {0xACDE48, "Apple"},
    {0xB0BE76, "Amazon"},
    {0xB827EB, "Raspberry Pi"},
    {0xB8F009, "Belkin"},
    {0xC03960, "Apple"},
    {0xC4AC59, "TP-Link"},
    {0xC8D083, "Cisco/Linksys"},
    {0xCC50E3, "Espressif"},
    {0xD03611, "Espressif"},
    {0xD850E6, "Apple"},
    {0xDC4F22, "Netgear"},
    {0xDCA632, "Espressif"},
    {0xE07758, "Tuya/Generic IoT"},
    {0xE45F01, "Raspberry Pi"},
    {0xE48D8C, "TP-Link"},
    {0xE89F6D, "Belkin"},
    {0xEC1A59, "Belkin"},
    {0xF02F74, "Netgear"},
    {0xF45EAB, "Apple"},
    {0xF81654, "Tuya/Generic IoT"},
    {0xFC1226, "Espressif"},
};

static const int OUI_TABLE_SIZE = sizeof(OUI_TABLE) / sizeof(OUI_TABLE[0]);

// Looks up a vendor name from a "AA:BB:CC:DD:EE:FF" style MAC string.
// Returns "" if no match (caller should treat that as "Unknown").
inline String lookupOuiVendor(const String &mac) {
    if (mac.length() < 8) return "";

    uint32_t prefix = (uint32_t)strtol(mac.substring(0, 2).c_str(), nullptr, 16) << 16;
    prefix |= (uint32_t)strtol(mac.substring(3, 5).c_str(), nullptr, 16) << 8;
    prefix |= (uint32_t)strtol(mac.substring(6, 8).c_str(), nullptr, 16);

    int lo = 0, hi = OUI_TABLE_SIZE - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (OUI_TABLE[mid].prefix == prefix) return String(OUI_TABLE[mid].vendor);
        if (OUI_TABLE[mid].prefix < prefix) lo = mid + 1;
        else hi = mid - 1;
    }
    return "";
}
