#include <Arduino.h>
#include "M5Unified.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <arduinoFFT.h>
#include "pixmob_protocol.h"
#include "dal/dal.h"     // Epic 2 in-progress: bring up the DAL alongside
                          // M5Unified. M5Unified call sites get migrated to
                          // DAL helpers one at a time in subsequent commits.




// ===== Hardware =====
const uint16_t IR_PIN = 19;
IRsend irsend(IR_PIN);

enum Mode
{
  MODE_OFF,
  MODE_RED,
  MODE_GREEN,
  MODE_BLUE,
  MODE_YELLOW,
  MODE_WHITISH,
  MODE_COUNT
};
const char *modeNames[MODE_COUNT] = {"OFF", "RED", "GREEN", "BLUE", "YELLOW", "WHITE"};
Mode currentMode = MODE_RED;

// ===== Audio / beat detection =====
const int SAMPLE_RATE = 16000;
const size_t FFT_SIZE = 512; // ~32 ms window at 16 kHz
const int BASS_BIN_LO = 2;   // ~62 Hz
const int BASS_BIN_HI = 7;   // ~187 Hz

int16_t micBuf[FFT_SIZE];
double vReal[FFT_SIZE];
double vImag[FFT_SIZE];

ArduinoFFT<double> FFT(vReal, vImag, FFT_SIZE, (double)SAMPLE_RATE);

float baselineFlux = 100.0f;
float prevBassEnergy = 0.0f;
float currentFlux = 0.0f;
unsigned long lastBeatMs = 0;
unsigned long lastDrawMs = 0;

const float BASELINE_ALPHA = 0.02f;
const float BEAT_MULTIPLIER = 2.5f;
const float FLUX_FLOOR = 2000.0f; // absolute minimum to count as a beat
const unsigned long BEAT_REFRACTORY_MS = 200;

// ===== BPM tracking =====
const size_t IBI_BUFFER_SIZE = 8;
unsigned long ibiBuffer[IBI_BUFFER_SIZE] = {0};
size_t ibiIndex = 0;
size_t ibiCount = 0;
float estimatedBPM = 0.0f; // 0 = not yet estimated

// === Audio gate ===
const float VOLUME_GATE = 500.0f;   // tune for your environment
float currentLevel = 0.0f;          // for diagnostic display

// Working buffer for the encoder. 80 entries is plenty for any 9-byte command.
static uint16_t irBuf[80];

bool beatModeActive = false;
bool beatModePaused = false;

void drawIdleUI();
void drawBeatUI();
void sendCurrentIR();

uint16_t modeColour()
{
  switch (currentMode)
  {
  case MODE_RED:
    return RED;
  case MODE_GREEN:
    return GREEN;
  case MODE_BLUE:
    return BLUE;
  case MODE_YELLOW:
    return YELLOW;
  case MODE_WHITISH:
    return WHITE;
  default:
    return BLACK;
  }
}

void setBeatMode(bool on)
{
  if (on == beatModeActive)
    return;
  beatModeActive = on;
  if (on)
  {
    M5.Speaker.end();
    M5.Mic.begin();
    baselineFlux = 100.0f;
    prevBassEnergy = 0.0f;
    currentFlux = 0.0f;
    // Reset BPM tracking on each entry to beat mode
    ibiIndex = 0;
    ibiCount = 0;
    estimatedBPM = 0.0f;
    lastBeatMs = 0;
    drawBeatUI();
  }
  else
  {
    M5.Mic.end();
    drawIdleUI();
  }
}

bool detectBeat()
{
  if (!M5.Mic.isEnabled())
    return false;
  if (!M5.Mic.record(micBuf, FFT_SIZE, SAMPLE_RATE))
    return false;

  // Compute mean absolute amplitude (cheap volume proxy)
  uint32_t sum = 0;
  for (size_t i = 0; i < FFT_SIZE; i++) sum += abs(micBuf[i]);
  currentLevel = (float)sum / FFT_SIZE;

  // Gate: if it's quiet, skip the rest entirely
  if (currentLevel < VOLUME_GATE) {
    prevBassEnergy = 0.0f;   // reset so we don't "remember" loud bass during silence
    return false;
  }


  for (size_t i = 0; i < FFT_SIZE; i++)
  {
    vReal[i] = (double)micBuf[i];
    vImag[i] = 0.0;
  }

  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  // Sum magnitudes in the bass band (~62-187 Hz)
  float bassEnergy = 0.0f;
  for (int i = BASS_BIN_LO; i <= BASS_BIN_HI; i++)
  {
    bassEnergy += (float)vReal[i];
  }

  // Spectral flux: rectified rise in bass-band energy
  float flux = bassEnergy - prevBassEnergy;
  if (flux < 0)
    flux = 0;
  prevBassEnergy = bassEnergy;
  currentFlux = flux;

  baselineFlux = baselineFlux * (1.0f - BASELINE_ALPHA) + flux * BASELINE_ALPHA;

  unsigned long now = millis();
  if (flux > baselineFlux * BEAT_MULTIPLIER && flux > FLUX_FLOOR && (now - lastBeatMs) > BEAT_REFRACTORY_MS)
  {
    // Record inter-beat interval before updating lastBeatMs
    if (lastBeatMs > 0)
    {
      unsigned long ibi = now - lastBeatMs;
      // Reject anything outside reasonable musical range (50-200 BPM)
      if (ibi >= 300 && ibi <= 1200)
      {
        ibiBuffer[ibiIndex] = ibi;
        ibiIndex = (ibiIndex + 1) % IBI_BUFFER_SIZE;
        if (ibiCount < IBI_BUFFER_SIZE)
          ibiCount++;

        if (ibiCount >= 3)
        {
          // Copy to a sortable buffer
          unsigned long sorted[IBI_BUFFER_SIZE];
          for (size_t i = 0; i < ibiCount; i++)
            sorted[i] = ibiBuffer[i];

          // Insertion sort - fine for small buffer
          for (size_t i = 1; i < ibiCount; i++)
          {
            unsigned long key = sorted[i];
            size_t j = i;
            while (j > 0 && sorted[j - 1] > key)
            {
              sorted[j] = sorted[j - 1];
              j--;
            }
            sorted[j] = key;
          }

          // Median is middle element (or average of two middle for even counts)
          unsigned long medianIbi;
          if (ibiCount % 2 == 1)
          {
            medianIbi = sorted[ibiCount / 2];
          }
          else
          {
            medianIbi = (sorted[ibiCount / 2 - 1] + sorted[ibiCount / 2]) / 2;
          }

          if (medianIbi > 50)
            estimatedBPM = 60000.0f / (float)medianIbi;
        }
      }
    }

    lastBeatMs = now;
    return true;
  }
  return false;
}

void sendCurrentIR() {
  uint8_t r = 0, g = 0, b = 0;
  switch (currentMode) {
    case MODE_RED:     r = 0xFF; g = 0x00; b = 0x00; break;
    case MODE_GREEN:   r = 0x00; g = 0xFF; b = 0x00; break;
    case MODE_BLUE:    r = 0x00; g = 0x00; b = 0xFF; break;
    case MODE_YELLOW:  r = 0xFF; g = 0xFF; b = 0x00; break;
    case MODE_WHITISH: r = 0xFF; g = 0xFF; b = 0xFF; break;
    default: return;
  }

  // Choose envelope based on current BPM
  pixmob::Time attack, sustain, release;
  if (estimatedBPM > 160.0f) {
    // Very fast: 0+32+96 = 128ms
    attack  = pixmob::T_0_MS;
    sustain = pixmob::T_32_MS;
    release = pixmob::T_96_MS;
  } else if (estimatedBPM > 100.0f || estimatedBPM == 0.0f) {
    // Medium / unknown (default): 32+96+192 = 320ms - your current "punchy"
    attack  = pixmob::T_32_MS;
    sustain = pixmob::T_96_MS;
    release = pixmob::T_96_MS;
  } else {
    // Slow ballad: 32+192+192 = 416ms - more presence
    attack  = pixmob::T_32_MS;
    sustain = pixmob::T_192_MS;
    release = pixmob::T_192_MS;
  }

  size_t n = pixmob::buildSingleColor(irBuf, 80, r, g, b,
                                      attack, sustain, release);
  if (n > 0) irsend.sendRaw(irBuf, n, 38);
}

void drawIdleUI()
{
  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(10, 10);
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.printf("%s", modeNames[currentMode]);
  M5.Display.setCursor(10, 50);
  M5.Display.setTextSize(2);
  M5.Display.printf("A: Test\n B: Change colour\n P: Toggle Beat Mode");
  M5.Display.setCursor(10, 110);
  M5.Display.printf("Batt: %d%%", M5.Power.getBatteryLevel());

  // Hello World from the DAL: prove begin() ran by surfacing the active
  // device count and the host profile's capability count.
  const auto* host = nocturnation::dal::DAL::profile_of("local");
  const unsigned caps = host
      ? (unsigned)(host->input_capability_count + host->output_capability_count)
      : 0;
  M5.Display.setCursor(10, 128);
  M5.Display.setTextSize(1);
  M5.Display.printf("DAL: %u dev, %u cap",
                    (unsigned)nocturnation::dal::DAL::active_device_count(),
                    caps);
}

void drawBeatUI()
{
  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(10, 5);
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.printf(" %s%s", modeNames[currentMode], beatModePaused ? " : Muted" : "");

  M5.Display.setCursor(10, 40);
  M5.Display.setTextSize(2);
  if (estimatedBPM > 0.0f)
    M5.Display.printf(" BPM: %.0f", estimatedBPM);
    //M5.Display.printf(" BPM: %.0f Lvl:%.0f", estimatedBPM, currentLevel);
    else
    M5.Display.printf(" BPM: ---");

  M5.Display.setCursor(10, 70);
  M5.Display.printf(" Batt: %d%%", M5.Power.getBatteryLevel());

  // Level meter showing flux as a multiple of baseline
  const int meterX = 10, meterY = 110, meterW = 220, meterH = 14;
  M5.Display.drawRect(meterX, meterY, meterW, meterH, WHITE);
  float ratio = (baselineFlux > 1.0f) ? currentFlux / baselineFlux : 0.0f;
  int barW = constrain((int)(ratio * 50.0f), 0, meterW - 2);
  M5.Display.fillRect(meterX + 1, meterY + 1, barW, meterH - 2, GREEN);
  int thrX = meterX + (int)(BEAT_MULTIPLIER * 50.0f);
  if (thrX < meterX + meterW)
  {
    M5.Display.drawFastVLine(thrX, meterY - 2, meterH + 4, RED);
  }
}


// New function for one-time bracelet group setup.
// Hold one bracelet in front of the StickC, fire this, label the bracelet.
void assignBraceletToGroup(uint8_t groupId) {
  size_t n = pixmob::buildSetGroupId(irBuf, 80, /*groupSel=*/0, groupId);
  if (n > 0) {
    // Send three times to be safe
    irsend.sendRaw(irBuf, n, 38); delay(50);
    irsend.sendRaw(irBuf, n, 38); delay(50);
    irsend.sendRaw(irBuf, n, 38);
  }
}

// And a targeted-colour helper for the constellation work:
void sendColourToGroup(uint8_t r, uint8_t g, uint8_t b,
                      pixmob::Time attack, pixmob::Time sustain, pixmob::Time release,
                      uint8_t groupId) {
  size_t n = pixmob::buildSingleColor(irBuf, 80, r, g, b,
                                      attack, sustain, release,
                                      pixmob::CHANCE_100, groupId);
  if (n > 0) irsend.sendRaw(irBuf, n, 38);
}


void smoothHueCycle(float cyclesPerSec, uint16_t durationMs) {
  const uint16_t STEP_MS = 50;
  float hue = 0.0f;
  float hueStep = cyclesPerSec * 360.0f * (STEP_MS / 1000.0f);  // degrees per step

  unsigned long start = millis();
  while (millis() - start < durationMs) {
    // HSV->RGB (saturation=1, value=1)
    float h = hue / 60.0f;
    int sector = (int)h;
    float f = h - sector;
    uint8_t v = 255;
    uint8_t p = 0;
    uint8_t q = (uint8_t)(255.0f * (1.0f - f));
    uint8_t t = (uint8_t)(255.0f * f);
    uint8_t r, g, b;
    switch (sector) {
      case 0:  r = v; g = t; b = p; break;
      case 1:  r = q; g = v; b = p; break;
      case 2:  r = p; g = v; b = t; break;
      case 3:  r = p; g = q; b = v; break;
      case 4:  r = t; g = p; b = v; break;
      default: r = v; g = p; b = q; break;
    }

    size_t n = pixmob::buildSingleColor(irBuf, 80, r, g, b,
                                        pixmob::T_0_MS,
                                        pixmob::T_96_MS,
                                        pixmob::T_32_MS);
    if (n > 0) irsend.sendRaw(irBuf, n, 38);

    hue += hueStep;
    if (hue >= 360.0f) hue -= 360.0f;
    delay(STEP_MS);
  }
}

void starlight(uint16_t durationMs) {
  unsigned long start = millis();

  while (millis() - start < durationMs) {
    // Pick a colour - cool palette for "stars"
    uint8_t pick = random(4);
    uint8_t r, g, b;
    switch (pick) {
      case 0: r = 0xFF; g = 0xFF; b = 0xFF; break;  // white
      case 1: r = 0xC0; g = 0xC0; b = 0xFF; break;  // pale blue (hot star)
      case 2: r = 0xFF; g = 0xE0; b = 0xC0; break;  // warm white
      case 3: r = 0xFF; g = 0xFF; b = 0xC0; break;  // pale yellow (sun-like)
    }

    size_t n = pixmob::buildSingleColor(irBuf, 80, r, g, b,
                                        pixmob::T_192_MS,    // gentle attack
                                        pixmob::T_96_MS,     // brief sustain
                                        pixmob::T_480_MS,    // long fade
                                        pixmob::CHANCE_16);  // sparse twinkles
    if (n > 0) irsend.sendRaw(irBuf, n, 38);

    delay(200 + random(300));  // irregular timing - 200-500 ms
  }
}


void setup()
{
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  irsend.begin();

  // Bring up the DAL. With the StickC HAL stub at zero declared capabilities,
  // this composes an empty "local" host profile and registers it - enough to
  // prove the wiring. As HAL backends land, capabilities will appear here.
  nocturnation::dal::DAL::begin();

  drawIdleUI();
}

void loop()
{
  M5.update();

  // Advance HAL/DAL state. With no orchestration subscribers yet, the
  // HAL's button polling fires no callbacks and this is effectively a
  // no-op. Wiring it up now means orchestration can subscribe later
  // without touching the loop structure.
  nocturnation::dal::DAL::loop_tick();

  if (M5.BtnB.wasPressed())
  {
    currentMode = static_cast<Mode>((currentMode + 1) % MODE_COUNT);
    if (beatModeActive)
      drawBeatUI();
    else
      drawIdleUI();
  }

  if (M5.BtnA.wasPressed() && !beatModeActive)
  {
    sendCurrentIR();
    //starlight(1000);
    //smoothHueCycle(1.0f, 10000);
    delay(20);
  }
  if (M5.BtnA.wasPressed() && beatModeActive)
  {
    beatModePaused = !beatModePaused;
    sendCurrentIR();
    delay(20);
  }

  if (M5.BtnPWR.wasClicked())
  {
    setBeatMode(!beatModeActive);
  }

  if (beatModeActive)
  {
    if (detectBeat())
    {
      M5.Display.fillScreen(modeColour());
      if (!beatModePaused)
        sendCurrentIR();
      delay(30);
      drawBeatUI();
    }
    else
    {
      unsigned long now = millis();
      if (now - lastDrawMs > 50)
      {
        drawBeatUI();
        lastDrawMs = now;
      }
    }
  }
  else
  {
    delay(5);
  }
}