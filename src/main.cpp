#include <Arduino.h>
#include "M5Unified.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>
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
// Sample rate, FFT size, and bass-band bin range are now owned by the HAL
// Mic backend (src/hal_stickc/mic_stickc.cpp); orchestration receives band-
// summary AudioFrameEvents and runs the beat-detection logic below on each
// frame.

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
void onButtonEvent(const char* source,
                   const nocturnation::dal::ButtonPressEvent& ev);
void onAudioFrame(const char* source,
                  const nocturnation::dal::AudioFrameEvent& ev);

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
    // Reset analysis state and turn the mic on through the DAL.
    baselineFlux   = 100.0f;
    prevBassEnergy = 0.0f;
    currentFlux    = 0.0f;
    ibiIndex       = 0;
    ibiCount       = 0;
    estimatedBPM   = 0.0f;
    lastBeatMs     = 0;
    nocturnation::dal::DAL::start_audio_input("local", 16000, 512);
    drawBeatUI();
  }
  else
  {
    nocturnation::dal::DAL::stop_audio_input("local");
    drawIdleUI();
  }
}

// Beat-detection orchestration. Consumes spectrum frames from the DAL
// (sourced by the StickC HAL Mic + the LocalDriver bridge) and runs the
// same flux / threshold / refractory / BPM-tracking logic the prototype's
// detectBeat() did, then drives the visible response (screen flash,
// optional IR send, redraw) when a beat fires.
void onAudioFrame(const char*,
                  const nocturnation::dal::AudioFrameEvent& ev)
{
  using namespace nocturnation::dal;

  if (!beatModeActive) return;       // mic might still be running mid-shutdown

  currentLevel = ev.overall_rms;

  // Volume gate.
  if (currentLevel < VOLUME_GATE) {
    prevBassEnergy = 0.0f;
    return;
  }

  // Spectral flux: rectified rise in bass-band energy.
  float flux = ev.bass_energy - prevBassEnergy;
  if (flux < 0) flux = 0;
  prevBassEnergy = ev.bass_energy;
  currentFlux    = flux;

  // Adaptive baseline (EMA).
  baselineFlux = baselineFlux * (1.0f - BASELINE_ALPHA) + flux * BASELINE_ALPHA;

  const unsigned long now = millis();
  const bool is_beat = flux > baselineFlux * BEAT_MULTIPLIER
                    && flux > FLUX_FLOOR
                    && (now - lastBeatMs) > BEAT_REFRACTORY_MS;

  if (!is_beat) return;

  // BPM tracking - record IBI before updating lastBeatMs.
  if (lastBeatMs > 0) {
    const unsigned long ibi = now - lastBeatMs;
    if (ibi >= 300 && ibi <= 1200) {  // 50..200 BPM window
      ibiBuffer[ibiIndex] = ibi;
      ibiIndex = (ibiIndex + 1) % IBI_BUFFER_SIZE;
      if (ibiCount < IBI_BUFFER_SIZE) ibiCount++;

      if (ibiCount >= 3) {
        unsigned long sorted[IBI_BUFFER_SIZE];
        for (size_t i = 0; i < ibiCount; ++i) sorted[i] = ibiBuffer[i];
        for (size_t i = 1; i < ibiCount; ++i) {
          unsigned long key = sorted[i];
          size_t j = i;
          while (j > 0 && sorted[j - 1] > key) {
            sorted[j] = sorted[j - 1];
            --j;
          }
          sorted[j] = key;
        }
        unsigned long medianIbi = (ibiCount % 2 == 1)
            ? sorted[ibiCount / 2]
            : (sorted[ibiCount / 2 - 1] + sorted[ibiCount / 2]) / 2;
        if (medianIbi > 50) estimatedBPM = 60000.0f / (float)medianIbi;
      }
    }
  }
  lastBeatMs = now;

  // Beat response: flash the screen, fire IR (unless muted), redraw the
  // beat UI. Same actions and ordering as the prototype's loop did.
  DAL::fire_display_clear("local", DisplayClearEvent{modeColour()});
  if (!beatModePaused) sendCurrentIR();
  delay(30);
  drawBeatUI();
  lastDrawMs = millis();   // prevent the loop body's periodic redraw
                           // from firing again immediately after this.
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
  using namespace nocturnation::dal;

  DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

  DAL::fire_display_show_text("local", DisplayShowTextEvent{
      10, 10, modeNames[currentMode], WHITE, BLACK, 3});

  DAL::fire_display_show_text("local", DisplayShowTextEvent{
      10, 50, "A: Test\n B: Change colour\n P: Toggle Beat Mode",
      WHITE, BLACK, 2});

  char batt_buf[24];
  snprintf(batt_buf, sizeof(batt_buf), "Batt: %d%%",
           M5.Power.getBatteryLevel());
  DAL::fire_display_show_text("local", DisplayShowTextEvent{
      10, 110, batt_buf, WHITE, BLACK, 2});

  // DAL self-status footer.
  const auto* host = DAL::profile_of("local");
  const unsigned caps = host
      ? (unsigned)(host->input_capability_count + host->output_capability_count)
      : 0;
  char dal_buf[64];
  snprintf(dal_buf, sizeof(dal_buf), "DAL: %u dev, %u cap, %u drv",
           (unsigned)DAL::active_device_count(),
           caps,
           (unsigned)DAL::registered_driver_count());
  DAL::fire_display_show_text("local", DisplayShowTextEvent{
      10, 128, dal_buf, WHITE, BLACK, 1});
}

void drawBeatUI()
{
  using namespace nocturnation::dal;

  DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

  // Mode title (with optional " : Muted" suffix)
  char title_buf[32];
  snprintf(title_buf, sizeof(title_buf), " %s%s",
           modeNames[currentMode],
           beatModePaused ? " : Muted" : "");
  DAL::fire_display_show_text("local", DisplayShowTextEvent{
      10, 5, title_buf, WHITE, BLACK, 3});

  // BPM
  char bpm_buf[24];
  if (estimatedBPM > 0.0f) {
    snprintf(bpm_buf, sizeof(bpm_buf), " BPM: %.0f", estimatedBPM);
  } else {
    snprintf(bpm_buf, sizeof(bpm_buf), " BPM: ---");
  }
  DAL::fire_display_show_text("local", DisplayShowTextEvent{
      10, 40, bpm_buf, WHITE, BLACK, 2});

  // Battery
  char batt_buf[24];
  snprintf(batt_buf, sizeof(batt_buf), " Batt: %d%%",
           M5.Power.getBatteryLevel());
  DAL::fire_display_show_text("local", DisplayShowTextEvent{
      10, 70, batt_buf, WHITE, BLACK, 2});

  // Level meter showing flux as a multiple of baseline. Composed from
  // FillRect primitives because the DAL has no draw-rect (outline) or
  // draw-line capability yet; 1-pixel-wide fill rects substitute cleanly.
  const int meterX = 10, meterY = 110, meterW = 220, meterH = 14;

  // Frame: top, bottom, left, right edges (1px each)
  DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
      meterX, meterY,           meterW, 1,      WHITE});
  DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
      meterX, meterY + meterH-1, meterW, 1,      WHITE});
  DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
      meterX, meterY,           1,      meterH, WHITE});
  DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
      meterX + meterW-1, meterY, 1,     meterH, WHITE});

  // Bar
  float ratio = (baselineFlux > 1.0f) ? currentFlux / baselineFlux : 0.0f;
  int   barW  = constrain((int)(ratio * 50.0f), 0, meterW - 2);
  DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
      meterX + 1, meterY + 1, barW, meterH - 2, GREEN});

  // Threshold marker (1-pixel-wide vertical line, 4px taller than the meter)
  int thrX = meterX + (int)(BEAT_MULTIPLIER * 50.0f);
  if (thrX < meterX + meterW) {
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        thrX, meterY - 2, 1, meterH + 4, RED});
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

  // Bring up the DAL. The StickC HAL backend now declares Display + Buttons
  // + IMU + Battery; the LocalDriver translates display events into HAL
  // calls and bridges HAL button events up to subscribers.
  nocturnation::dal::DAL::begin();

  // Subscribe to button events from the host. The handler dispatches by
  // (id, kind) to the same logic the old M5.BtnX.wasPressed/wasClicked
  // checks used to live in loop().
  nocturnation::dal::DAL::subscribe_button_presses("local", &onButtonEvent);

  // Subscribe to audio frames from the host. The handler runs the
  // beat-detection logic that used to live in detectBeat() and triggers
  // the visible response (flash + IR + redraw) on each detected beat.
  // The mic stays off until setBeatMode(true) calls DAL::start_audio_input.
  nocturnation::dal::DAL::subscribe_audio_frames("local", &onAudioFrame);

  drawIdleUI();
}

void onButtonEvent(const char*, const nocturnation::dal::ButtonPressEvent& ev) {
  using namespace nocturnation::hal;

  // Btn2 (side button): cycle colour mode.
  if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::Pressed) {
    currentMode = static_cast<Mode>((currentMode + 1) % MODE_COUNT);
    if (beatModeActive) drawBeatUI();
    else                drawIdleUI();
    return;
  }

  // Btn1 (front "fire" button): test pulse when idle, mute toggle in beat mode.
  if (ev.id == ButtonId::Btn1 && ev.kind == ButtonEvent::Pressed) {
    if (beatModeActive) {
      beatModePaused = !beatModePaused;
      sendCurrentIR();
    } else {
      sendCurrentIR();
    }
    delay(20);
    return;
  }

  // Btn3 (power button): toggle beat mode on a click (press + release).
  if (ev.id == ButtonId::Btn3 && ev.kind == ButtonEvent::Clicked) {
    setBeatMode(!beatModeActive);
    return;
  }
}

void loop()
{
  M5.update();

  // Advance HAL/DAL state. The HAL polls buttons and the LocalDriver's
  // bridge fires our onButtonEvent() handler from inside this call.
  nocturnation::dal::DAL::loop_tick();

  if (beatModeActive)
  {
    // Beat detection lives in onAudioFrame() now (called from the DAL when
    // a fresh AudioFrameEvent arrives). The loop body just keeps the BPM
    // / meter UI fresh between beats.
    unsigned long now = millis();
    if (now - lastDrawMs > 50)
    {
      drawBeatUI();
      lastDrawMs = now;
    }
  }
  else
  {
    delay(5);
  }
}