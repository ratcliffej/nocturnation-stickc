// Spike: S3 USB descriptor masquerade as FTDI FT232R.
//
// Goal: prove a NocturNation StickC S3 can advertise FTDI VID/PID over
// native USB and be enumerated by QLC+'s DMX USB plugin (which filters
// against a hardcoded FTDI/Atmel/Microchip/NXP whitelist - see
// plugins/dmxusb/src/dmxinterface.cpp:77-90 in the QLC+ source).
//
// This is a throwaway spike. It builds via its own platformio env
// (env:spike-s3-ftdi-vid) and replaces main.cpp for the duration of
// the test. Once we have the descriptor working + QLC+ enumeration
// confirmed, the descriptor setup moves into the real DmxBridgeMode
// enter() in the production firmware (Epic 7 B3).
//
// Test procedure (macOS):
//
//   1. pio run -e spike-s3-ftdi-vid -t upload
//   2. After the S3 reboots, plug + check enumeration:
//        ioreg -p IOUSB -l -w 0 | grep -B 4 -A 4 -i FTDI
//      The Stick should show with VID 0x0403, PID 0x6001, product
//      "FT232R USB UART", manufacturer "FTDI".
//   3. Open QLC+ -> Inputs/Outputs. Look for "DMX USB" / "Enttec"
//      entry pointing at the Stick.
//   4. If yes: spike succeeds. The S3 + FTDI VID + QLC+ enumeration
//      path is proven.
//   5. To verify the data path: in QLC+, configure the device as an
//      Enttec DMX USB Pro output. Move a slider in Simple Desk. The
//      Stick's onboard LED (GPIO 8 on S3 - the red status LED) should
//      blink each time a frame arrives (LED state mirrors most recent
//      byte's bit-0).

#include <Arduino.h>
#include <USB.h>

// FTDI's USB Vendor ID. The QLC+ DMX USB plugin whitelist accepts this
// at the enumeration filter, so advertising it lets QLC+ consider the
// device a candidate DMX widget.
static constexpr uint16_t kFtdiVid     = 0x0403;
// FT232R PID. The single-channel UART chip that the bulk of Enttec's
// USB DMX hardware uses.
static constexpr uint16_t kFt232rPid   = 0x6001;
// Enttec DMX USB Pro standard data rate.
static constexpr uint32_t kEnttecBaud  = 921600;
// Onboard LED on the M5StickC S3 (visible red indicator).
static constexpr int      kLedPin      = 8;

void setup() {
    pinMode(kLedPin, OUTPUT);
    digitalWrite(kLedPin, HIGH);   // LED on = booted + waiting

    // Override the default Espressif descriptors before bringing up
    // USB. Requires ARDUINO_USB_CDC_ON_BOOT=0 for this env (the
    // default S3 env has it =1, which would enumerate the device
    // before setup() runs and lock the descriptors in).
    USB.VID(kFtdiVid);
    USB.PID(kFt232rPid);
    USB.productName("FT232R USB UART");
    USB.manufacturerName("FTDI");
    USB.serialNumber("NN3SPIKE001");   // any short identifier; helps
                                       // distinguish from real FTDI
                                       // devices in `ioreg` output

    USB.begin();
    // Brief settle so the host has time to enumerate before we start
    // pushing data.
    delay(1500);

    Serial.begin(kEnttecBaud);
    digitalWrite(kLedPin, LOW);    // LED off after init - first byte
                                   // received in loop() pulses it back
                                   // on.
}

void loop() {
    while (Serial.available() > 0) {
        const int b = Serial.read();
        if (b < 0) break;
        // Mirror bit 0 of the most-recent received byte onto the LED.
        // QLC+'s output stream is a continuous flood of Enttec Pro
        // frames at ~44 Hz, so the LED will visibly flicker once
        // bytes are arriving. Doesn't prove correctness of any
        // particular frame, but does prove "bytes are coming down
        // the cable", which is what the spike is for.
        digitalWrite(kLedPin, (b & 1) ? HIGH : LOW);

        // Echo back so any host-side serial monitor can verify
        // bidirectionality. QLC+ ignores anything we write (it's a
        // one-way fixture output), so this is purely diagnostic.
        Serial.write(static_cast<uint8_t>(b));
    }
    delay(1);   // yield to FreeRTOS so we don't pin the CPU
}
