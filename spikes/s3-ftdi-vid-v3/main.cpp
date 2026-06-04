// Spike v3: S3 USB descriptor masquerade as FTDI FT232R - explicit
// USB.begin() in setup().
//
// v2 baked the FTDI VID/PID into the firmware image via build flags
// but the host still saw nothing. Root cause found in arduino-esp32
// framework cores/esp32/main.cpp lines 57 + 66:
//
//   #if ARDUINO_USB_CDC_ON_BOOT && !ARDUINO_USB_MODE
//       Serial.begin();
//   #endif
//   #if ARDUINO_USB_ON_BOOT && !ARDUINO_USB_MODE
//       USB.begin();
//   #endif
//
// Both conditions require !ARDUINO_USB_MODE. We need ARDUINO_USB_MODE=1
// for native USB-OTG with programmable descriptors, so the framework
// deliberately skips its USB / CDC startup - it expects the application
// to manage USB-OTG itself in this mode.
//
// v3 calls USB.begin() + Serial.begin() explicitly. With the build
// flags from v2 still in place, the global USB object reads our
// FTDI descriptors in its constructor; USB.begin() then registers
// them with TinyUSB; Serial.begin() brings up the CDC interface.
//
// Test procedure:
//
//   1. pio run -e spike-s3-ftdi-vid-v3 -t upload
//   2. After reboot, plug + check:
//        system_profiler SPUSBDataType | grep -B 1 -A 4 -E "0x0403|FT232|FTDI"
//   3. If FTDI shows up: open QLC+ -> Inputs/Outputs. Look for DMX USB
//      / Enttec entry pointing at the Stick.
//   4. Configure as Universe 1 output. Move a Simple Desk slider.
//      LED on GPIO 8 should flicker.

#include <Arduino.h>
#include <USB.h>

static constexpr int kLedPin = 8;

void setup() {
    pinMode(kLedPin, OUTPUT);
    digitalWrite(kLedPin, HIGH);   // boot indicator

    // Explicit USB-OTG startup. The framework skips this when
    // ARDUINO_USB_MODE=1; the application owns it.
    USB.begin();

    // Now bring up the CDC interface on top of USB-OTG.
    Serial.begin(921600);

    // Settle window for host enumeration.
    delay(2000);

    digitalWrite(kLedPin, LOW);    // boot done
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
