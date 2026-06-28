// pins.h
// Pin map for the Rockbase NM-CYD-C5 (ESP32-C5).
// Source: Bruce firmware's boards/nm-cyd-c5/pins_arduino.h + connections.md
// All SPI peripherals (display, touch, SD) share one SPI bus with separate CS lines.

#pragma once

// --- Shared SPI bus ---
#define PIN_SPI_SCK   6
#define PIN_SPI_MISO  2
#define PIN_SPI_MOSI  7

// --- Display (ST7789) ---
#define PIN_TFT_CS    23
#define PIN_TFT_DC    24
#define PIN_TFT_RST   -1   // tied to chip RST
#define PIN_TFT_BL    25

// --- Touch (XPT2046, resistive) ---
#define PIN_TOUCH_CS  1

// --- SD card ---
#define PIN_SD_CS     10

// --- GPS (UART) ---
#define PIN_GPS_RX    4   // ESP32 RX  <- GPS TX
#define PIN_GPS_TX    5   // ESP32 TX  -> GPS RX
#define GPS_BAUD      9600

// --- Status LED (WS2812 RGB) ---
#define PIN_RGB_LED   27

// --- Misc / expansion (not used by this firmware, listed for reference) ---
// I2C SDA: 9   I2C SCL: 8   (shared with IR/RF pins on this board - unused here)
