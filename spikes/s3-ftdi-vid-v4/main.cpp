// Spike v4: S3 USB descriptor masquerade as FTDI FT232R - correct
// init order (Serial.begin BEFORE USB.begin).
//
// v3 fixed the previous "USB never started" issue by calling USB.begin()
// in setup() (the framework skips this when ARDUINO_USB_MODE=1). The
// FTDI strings reached the linked ELF. But the *order* was wrong:
// USB.begin() finalises the TinyUSB device descriptor; any CDC class
// has to be registered BEFORE that call, or the device enumerates with
// zero usable interfaces and macOS hides it.
//
// Confirmed against the canonical arduino-esp32 USB example
// (libraries/USB/examples/CompositeDevice/CompositeDevice.ino:176/184):
// every interface's begin() is called first, USB.begin() is called LAST.
//
// With ARDUINO_USB_MODE=1, `Serial` is the USBCDC class (TinyUSB-backed),
// not HWCDC (USB-Serial/JTAG hardware). Calling Serial.begin() registers
// the CDC interface with TinyUSB; USB.begin() then assembles the full
// descriptor and starts the device.

#include <Arduino.h>
#include <USB.h>

static constexpr int kLedPin = 8;

void setup() {
    pinMode(kLedPin, OUTPUT);
    digitalWrite(kLedPin, HIGH);   // boot indicator

    // ORDER MATTERS:
    // 1. Register the CDC interface with TinyUSB.
    Serial.begin(921600);
    // 2. Finalise the device descriptor + start TinyUSB.
    USB.begin();

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
