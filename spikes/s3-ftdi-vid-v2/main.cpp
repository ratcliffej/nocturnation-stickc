// Spike v2: S3 USB descriptor masquerade as FTDI FT232R, build-flag
// edition.
//
// v1 disabled ARDUINO_USB_CDC_ON_BOOT and set VID/PID via runtime
// API in setup(), but that left the USB stack with no CDC class
// registered - the device enumerated with no usable endpoints and
// macOS hid it.
//
// v2 keeps CDC_ON_BOOT enabled (so the framework brings up USB-CDC
// before setup() runs) and overrides VID/PID/Manufacturer/Product
// via -D build flags at compile time. The arduino-esp32 framework
// reads USB_VID / USB_PID / USB_MANUFACTURER / USB_PRODUCT macros
// when initialising USB descriptors. Fewer moving parts; no runtime
// ordering surprises.
//
// Test procedure (macOS):
//
//   1. pio run -e spike-s3-ftdi-vid-v2 -t upload
//   2. After reboot, plug + check:
//        system_profiler SPUSBDataType | grep -B 1 -A 3 -i ftdi
//      Expected: a USB entry with Vendor ID 0x0403 (FTDI Ltd.) and
//      Product ID 0x6001, Manufacturer "FTDI", Product "FT232R USB
//      UART".
//   3. Open QLC+ -> Inputs/Outputs. Look for a DMX USB / Enttec entry.
//   4. If yes: bench-validate by configuring Universe 1 output as
//      Enttec Pro and moving a slider in Simple Desk - the S3's
//      onboard LED (GPIO 8) should flicker as bytes arrive.

#include <Arduino.h>

// Onboard red LED on M5StickC S3.
static constexpr int kLedPin = 8;

void setup() {
    pinMode(kLedPin, OUTPUT);
    digitalWrite(kLedPin, HIGH);   // boot indicator

    // Serial here is USB-CDC (because CDC_ON_BOOT=1 in the env). The
    // VID/PID/Manufacturer/Product descriptors are already baked from
    // the build flags - we don't touch USB.VID() at runtime.
    Serial.begin(921600);
    delay(1500);                   // give the host a moment to enumerate
    digitalWrite(kLedPin, LOW);    // boot done; first byte will pulse this
}

void loop() {
    while (Serial.available() > 0) {
        const int b = Serial.read();
        if (b < 0) break;
        // Mirror bit 0 of received byte onto the LED so we get a visible
        // signal that QLC+ data is reaching the cable.
        digitalWrite(kLedPin, (b & 1) ? HIGH : LOW);
        // Echo back; harmless if QLC+ ignores it, useful diagnostic if
        // we open a console reader on the host side.
        Serial.write(static_cast<uint8_t>(b));
    }
    delay(1);
}
