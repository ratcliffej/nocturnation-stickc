// Spike v5: S3 USB descriptor masquerade as FTDI FT232R - via USB-OTG
// (TinyUSB), not USB-Serial/JTAG.
//
// The bench finding that drove this: with ARDUINO_USB_MODE=1, macOS
// system_profiler showed our Manufacturer + Product + Serial Number
// strings applied correctly, but VID/PID stayed at Espressif's
// 0x303A:0x1001. Root cause: ARDUINO_USB_MODE has the opposite
// semantics to what spikes v1-v4 assumed:
//
//   ARDUINO_USB_MODE=1 -> use USB-Serial/JTAG hardware controller.
//                         Strings programmable; VID/PID fixed in ROM.
//   ARDUINO_USB_MODE=0 -> use USB-OTG via TinyUSB. Full descriptor
//                         control including VID/PID.
//
// v5 sets ARDUINO_USB_MODE=0. With it, the framework's main.cpp
// auto-starts USB.begin() + Serial.begin() (those #if blocks gate on
// !ARDUINO_USB_MODE), so the app doesn't need to call either
// explicitly. The compile-time build flags USB_VID / USB_PID / etc.
// are read by USB.cpp's global object constructor as before.
//
// Test procedure:
//   1. pio run -e spike-s3-ftdi-vid-v5 -t upload
//   2. After reboot + replug, in macOS:
//      System Information -> USB -> look for "FT232R USB UART"
//      Expected this time: Vendor ID 0x0403, Product ID 0x6001
//   3. If yes: open QLC+ -> Inputs/Outputs. DMX USB / Enttec entry
//      should appear (FTDI VID/PID + correct USB class makes it past
//      both the QLC+ filter and any macOS driver requirements).

#include <Arduino.h>

static constexpr int kLedPin = 8;

void setup() {
    pinMode(kLedPin, OUTPUT);
    digitalWrite(kLedPin, HIGH);

    // With ARDUINO_USB_MODE=0 the framework has already brought up
    // USB-OTG + CDC interface via TinyUSB before this runs. Serial
    // is USBCDC (TinyUSB-backed). Just set the baud and go.
    Serial.begin(921600);
    delay(1500);
    digitalWrite(kLedPin, LOW);
}

void loop() {
    while (Serial.available() > 0) {
        const int b = Serial.read();
        if (b < 0) break;
        digitalWrite(kLedPin, (b & 1) ? HIGH : LOW);
        Serial.write(static_cast<uint8_t>(b));
    }
    delay(1);
}
