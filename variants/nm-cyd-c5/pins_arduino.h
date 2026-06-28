// pins_arduino.h
// Minimal Arduino-core variant file for the Rockbase NM-CYD-C5 (ESP32-C5).
//
// The arduino-esp32 core requires a pins_arduino.h for whatever variant name
// the board JSON specifies. The official board definition we're using
// references a variant ("pinouts") that isn't bundled with every framework
// release, so this project supplies its own minimal variant instead of
// depending on one shipped inside the framework package.
//
// This only defines what the Arduino core / SPI / Wire libraries require to
// compile. The actual application pin usage lives in src/pins.h.

#pragma once

#include <stdint.h>

#define EXTERNAL_NUM_INTERRUPTS 30
#define NUM_DIGITAL_PINS        30
#define NUM_ANALOG_INPUTS       6

#define analogInputToDigitalPin(p)  (((p) < NUM_ANALOG_INPUTS) ? (p) : -1)
#define digitalPinToInterrupt(p)    (((p) < NUM_DIGITAL_PINS) ? (p) : -1)
#define digitalPinHasPWM(p)         ((p) < NUM_DIGITAL_PINS)

static const uint8_t LED_BUILTIN = 8;
#define BUILTIN_LED LED_BUILTIN

// UART0 (default Serial / USB-CDC console)
static const uint8_t TX = 21;
static const uint8_t RX = 20;

// Default I2C (unused by this firmware, defined for library compatibility)
static const uint8_t SDA = 8;
static const uint8_t SCL = 9;

// Default SPI bus — matches the shared display/touch/SD bus on this board
// (see src/pins.h for the authoritative pin map used by the application)
static const uint8_t SS   = 23; // default CS — app overrides per-peripheral
static const uint8_t MOSI = 7;
static const uint8_t MISO = 2;
static const uint8_t SCK  = 6;

// Analog pins — present for core/library compatibility; not used by this firmware
static const uint8_t A0 = 0;
static const uint8_t A1 = 1;
static const uint8_t A2 = 2;
static const uint8_t A3 = 3;
static const uint8_t A4 = 4;
static const uint8_t A5 = 5;
