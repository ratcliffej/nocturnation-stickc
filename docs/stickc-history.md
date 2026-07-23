# StickC design history

Deep rationale for non-obvious decisions in the StickC firmware. In-code
comments carry a one-line hint where a WHY is needed; this file holds the
bench findings, cross-PR context and multi-paragraph reasoning that
would otherwise clutter the source.

Entries are organised by file. Line numbers are approximate — grep for
the identifier if they've drifted.

---

### ESP-NOW redundant-send count set to 2

**File**: `src/dal/drivers/espnow_broadcast_driver.h` (near `kRedundantSends`)
**Context**: Spec §4.3 lets Directors send each frame N times with the same
sequence number, separated by 5-15 ms of pseudo-random jitter. Lume dedup
catches duplicates.
**Rationale**: Bench 2026-06-23 found that bumping 3 → 5 *increased* visible
pulse dropouts on the M5 fleet. Each 5-send burst eats ~40-70 ms of radio
time; at the 7 Hz sparkle rate of sparkle_on_beat the bursts overlap each
other plus wash-refresh traffic, and Lume WiFi receive queues fill before
the main task can drain them. Frames get dropped at the radio IDF layer.
Epic 15 (2026-06-27) then dropped 3 → 2 alongside the move to ESP-NOW
Long Range mode: LR doubles per-frame airtime (500 kbps vs 1 Mbps), so
3 retransmits at 7 Hz would push back into the saturation territory the
5-send finding identified. If RF reliability at a venue needs more, the
answer is the Lume-repeat mesh setting, not louder Director retransmits.

---

### Retransmit buffer raised to 250 bytes

**File**: `src/dal/drivers/espnow_broadcast_driver.h` (near `kRetransmitBufSize`)
**Context**: Originally 32 bytes, sized for LIGHT_PULSE (14 bytes with header).
**Rationale**: Epic 13 raised the wire ceiling to 250 for TextDisplay (~200 bytes
max) and bitmap planes, and the DMX-bridge passthrough routes display
frames through this driver too. Setting the buffer to the transport-level
ceiling means no frame size silently skips retransmit.

---

### Heartbeat gate switched to last_hb_ms_ (not last_tx_ms_)

**File**: `src/dal/drivers/espnow_broadcast_driver.cpp` (`maybe_send_heartbeat`)
**Context**: Pre-v0.6 the gate was `last_tx_ms_` (skip-if-any-recent-TX).
**Rationale**: Skip-if-recent suppressed heartbeats under continuous
DMX-bridge or sparkle-rate traffic, leaving Lumes without the stable
1 Hz anchor §4.3 requires. Now the gate is `last_hb_ms_` only, so
heartbeats fire every `kHeartbeatPeriodMs` regardless of other TX
activity. Phase 1 of the tick-anchor work.

---

### send_passthrough re-stamps source_id and sequence_number

**File**: `src/dal/drivers/espnow_broadcast_driver.cpp` (`send_passthrough`)
**Context**: The orchestrator emits display frames (TextDisplay / Bitmap*/
ClearScreen) with source_id = 0xFF because it doesn't know which Director
it's bridging through. The Director re-stamps.
**Rationale**: When we take ownership of the source_id we MUST also
re-stamp the sequence_number from `next_seq()`, so the passthrough
frame joins this Director's single monotonic seq stream. The
orchestrator emits with its own independent 1..255 wrapping counter;
leaving it in place puts two unrelated seq streams under one source_id.
Every receiver dedups on `(source_id, sequence_number)` over a 16-slot
ring, and SignalQuality derives missed frames from forward seq gaps —
so a collision drops display frames AND native pulses fleet-wide and
inflates loss % to nonsense. The retransmit copies inherit the stamped
buffer so all redundant copies share the one seq and dedup as one
logical frame.

Lumes locked to a *different* Director then reject these display
frames as "from a different source", which is exactly the multi-show
partitioning the EMF work needed.

---

### Rainbow Test step interval bumped 50 → 100 ms and sustain to T_192

**File**: `src/modes/test_mode.cpp` (`tick_rainbow`) and `test_mode.h`
(`kRainbowStepIntervalMs`)
**Context**: Test mode's rainbow sweeps hue at 0.5 Hz, firing pulses that
render across bracelets, LED strips, and Tildagon perimeter.
**Rationale**: The Tildagon perimeter's Full-mode Harding cap sits around
60 ms and its MicroPython render poll has an ~85 ms baseline (with
occasional ~160 ms outliers). At the previous 50 ms step + T_96 sustain,
envelope 1 expired ~96 ms into life; if the next Tildagon render tick
caught that window before pulse 2 drained from the ESP-NOW queue, the
perimeter cleared the envelope and painted black — visible as a strobe
gap in the sweep. 100 ms step + T_192 sustain gives ~92 ms overlap,
comfortably above the poll baseline. Atom LED strip + bracelet effect
unchanged.

---

### Sparkle Test cadence slowed to ~0.9 Hz (1100 ms step)

**File**: `src/modes/test_mode.cpp` (`kSparkleStepMs`)
**Context**: 1 s fade envelope (T_0 + T_480 + T_480 = 960 ms).
**Rationale**: 500 ms (2 Hz) cut the envelope short before it completed.
1100 ms leaves ~140 ms safety gap so the fade lands before the next
step arrives. Reads as "lingering twinkles" against the CHANCE_32
random-chance fire.

---

### Sparkle palette is white only

**File**: `src/modes/test_mode.cpp` (`kSparkleColour`)
**Context**: Test pulse/fade cycles use R/G/B/W; sparkle uses only white.
**Rationale**: Operator spec — the random colour palette felt too busy;
white sparkle reads as crowd-shimmer. Pure red also runs slightly hotter
than green/blue at the same drive level so red-bearing colours read warm
during a fade tail.

---

### Auto-cal dynamic range floored at 5 octaves (32×)

**File**: `src/modes/test_mode.cpp` (`process_audio_frame`)
**Context**: The original constraint was 2 octaves.
**Rationale**: 2 octaves worked on the Plus2's PDM mic because its noise
floor varied enough that `auto_min` and `auto_max` stayed naturally apart.
The StickS3's ES8311 codec has a much flatter noise floor and the bars
compressed to a tiny range then oscillated wildly with any input
variance. 5 octaves comfortably accommodates music dynamics on either
host.

---

### Lume defers renders that block the WiFi task

**File**: `src/modes/lume_mode.cpp` (`on_recv`, `pending_light_`,
`pending_repeat_`, `signal_recovered_needs_repaint_`)
**Context**: The ESP-NOW receive callback runs on the WiFi task.
**Rationale**: `IRsend::sendRaw` is a ~30 ms blocking cli/sei GPIO
bit-bang; calling it from the WiFi task crashes the S3 (watchdog / stack).
`radio->send_broadcast` from the WiFi callback is also unsafe under
arduino-esp32 v2.x. SPI paints from the WiFi task are similarly crash
prone. Copying the payload into a queue is fast and safe; loop_tick
drains from the main task. Newer arrivals replace older ones — dropping
a stale beat is fine when a fresh one is already on the way.

---

### Inline vs deferred LIGHT_PULSE fan-out

**File**: `src/modes/lume_mode.cpp` (`fan_out_light_pulse_inline` vs
`fan_out_light_pulse`)
**Context**: Bindings declare `can_render_in_callback()`.
**Rationale**: Callback-safe bindings (LED strip, LCD) stamp the pulse
envelope IMMEDIATELY in WiFi-task context, so all Lumes anchor to
broadcast-receipt time (µs-synchronised across the fleet). Blocking
renderers (PixMob's `IRsend::sendRaw`) stay on the deferred path via
`pending_light_`. Inter-Lume render unison for sparkle-on-beat goes
from up-to-50 ms loop-tick variance to ~µs radio-propagation variance.
The two paths skip each other by checking the flag so a binding is
dispatched exactly once per pulse.

---

### PixMob IR vs LED strip mutex on hosts with both

**File**: `src/modes/lume_mode.cpp` (LumeMode::enter, `strip_active` gate)
**Context**: On a host with both IRTx and LedStrip, having both bindings
active at once would break inter-Lume sparkle unison.
**Rationale**: `IRsend::sendRaw` is a ~30 ms cli/sei blocking bit-bang.
When the strip is actually enabled in Config, skip activating
PixMobIrBinding. Operator who wants a Stick-as-PixMob-IR-satellite
turns the strip off via Config > LED Strip > Enable; PixMobIrBinding
activates as normal on the next Lume re-entry.

---

### last_rx_ms_ vs millis() race requires saturating subtract

**File**: `src/modes/lume_mode.cpp` (multiple sites — `age_since_rx`,
`signal_bars_from_age`, `draw_no_signal_body`)
**Context**: `last_rx_ms_` is written from the WiFi-task callback, read
from the main task.
**Rationale**: If `on_recv` fires between the caller's `millis()` sample
and the comparison, `last_rx_ms_` can briefly be 1-2 ms ahead of `now`.
Without the saturating subtract, that turns into ~UINT32_MAX, trips the
`> kNoSignalMs` threshold, and produces a spurious NO SIGNAL flicker
every pulse cycle.

---

### Lumes never auto-promote to Director on signal loss

**File**: `src/modes/lume_mode.cpp` (NO SIGNAL edge in `loop_tick`)
**Context**: Three missed heartbeats (3 s) surfaces "NO SIGNAL". A naïve
design would fail over into local Director mode.
**Rationale**: A Lume-promoted-to-Director would compete with the real
Director when it comes back and ruin show coordination. Discipline is
"fail subtle": display NO SIGNAL text only, don't run any visually
distinctive idle effect on this edge. A brief outage must not visually
fragment the show.

---

### hop_count byte offset is at header byte 5 (v2 wire)

**File**: `src/modes/lume_mode.cpp` (`pending_repeat_buf_` bump path)
**Context**: v1 had hop_count at byte 3; v2 shifted it to byte 5 after the
2-byte magic + version prefix landed.
**Rationale**: Pre-2026-06-28 the driver wrote to `buf[3]`, corrupting
source_id and breaking mesh-wide dedup + TOFU lock. Now it goes through
`transport::espnow::set_hop_count` so the offset can't drift from the
header layout again.

---

### Passthrough source_id re-stamp validates magic + version first

**File**: `src/dal/drivers/espnow_broadcast_driver.cpp` (`send_passthrough`)
**Context**: We patch source_id byte in place if it's 0xFF (broadcast).
**Rationale**: Validate the magic + version bytes before mutating, so a
misbehaving upstream sender can't get bad bytes onto the air through the
Director's identity. Drop on anomaly instead.

---

### Btn1 short-press brightness cycle retired

**File**: `src/modes/lume_mode.cpp` (`on_button_event`)
**Context**: The Atom Lite's Btn1 short-press used to cycle strip brightness.
**Rationale**: Retired 2026-07-08. Strip is now hard-wired to 5 % at
`DAL::begin` so no runtime adjustment is meaningful, and Atom Lite needs
Btn1 for GroupID cycling (long-press). Restore via per-host external-PSU
cap raise + build flag if a stage deployment ever needs live brightness
control.

---

### Signal-loss fallback wash (EMF prep)

**File**: `src/modes/lume_mode.cpp` (`emit_fallback_wash_start`,
`emit_fallback_wash_fade`, `emit_fallback_wash_recovery`) and
`src/modes/lume_mode.h` (`kFallbackEnterMs`, `kFallbackFadeStartMs`,
`kFallbackFadeTicks`).
**Context**: EMF artist-stage prep required a graceful degradation when a
Lume loses the Director.
**Rationale**: Anchor three edges to `age_since_rx`:
- 3 s → NO SIGNAL diagnostic text (operator-facing).
- 10 s → synthesise a LIGHT_WASH locally (muted blue↔purple, low
  intensity) dispatched through `fan_out_light_wash`. Every binding's
  existing wash state machine handles it — no parallel render path.
- 40 s → LIGHT_WASH_END with release_time = 255 (100 ms units, ~25.5 s
  cap). u8 max isn't quite the requested 30 s but is functionally
  identical to the eye and avoids staging multiple END frames.
Signal recovery emits a 500 ms fade-out so the synthetic baseline
clears before the returning Director's traffic competes with it.
The fallback flags reset on the recovery edge so the next silence
episode re-triggers cleanly.

---

### TOFU lock partitions multi-show venues

**File**: `src/modes/lume_mode.cpp` (`on_recv`, `tofu_.admit`) and
`src/modes/lume_mode.h` (`tofu_`)
**Context**: At a multi-show venue two Directors can broadcast on the
same channel.
**Rationale**: First-eligible-frame establishes a lock on that Director's
source_id; subsequent frames from any other source are dropped. Display
family broadcasts (source_id = 0xFF) are admitted once a session exists,
without resetting the liveness timer. Lock expires after 10 s of silence
(same window as `kRescanMs` so channel rescan and TOFU relock fire on
the same edge). Port of the Tildagon TofuLock; without it a StickC-based
Lume at EMF would render both shows' content simultaneously.

---

### Director-clock offset tracked with 90/10 smoothing

**File**: `src/modes/lume_mode.cpp` (Heartbeat handling in `on_recv`)
**Context**: Each admitted HEARTBEAT carries the Director's ms tick. Phase 1
of the §4.3 tick-anchor work tracks `(tick - local_ms)` smoothed
exponentially; envelope math rewires to it in Phase 2.
**Rationale**: 90/10 handles the typical ~1 ms/s local `millis()` drift
smoothly and rejects one-off outliers (relay-path arrivals with
abnormal latency). Integer arithmetic to avoid float on the hot RX
path. Duplicates carry the same tick as their originating send, so
post-dedup is the correct gate. First value for a fresh source
(including TOFU relock) seeds the smoother directly to skip the
exponential "climb from zero" transient.

---

### Status pip: full-screen pulse trade-off

**File**: `src/modes/lume_mode.cpp` (`draw_status_pip`) and
`src/modes/lume_mode.h` (`kPip*` constants)
**Context**: Block 13 replaced a full-width 12 px status strip with a
compact 38×12 px pip anchored top-right.
**Rationale**: Frees the pulse rect for full-screen colour impact. The pip
paints OVER the pulse rect on each refresh; pulses may briefly paint
underneath between pip refreshes. 100 ms (10 Hz) pip refresh reads as
steady when full-screen pulses repaint at ~30 Hz, and bounds SPI writes
(~7 fill_rects per refresh batched into one burst via
`begin_buffered_paint`). Pip needs an opaque wipe first because any
colour may have been painted underneath since last refresh; without the
wipe, signal/battery colours composite against arbitrary backgrounds.

---

### PixMob Bench T5 uses square envelope for auto-refresh

**File**: `src/modes/test_mode.cpp` (`tick_pixmob_bench`)
**Context**: T5 auto-fires `SingleColor(255,255,255)` at a selectable
cadence to validate the continuous-wash refresh interval.
**Rationale**: Square envelope (T_0 / T_3840 / T_0) has no decay tail,
so the new refresh pre-empts the previous sustain cleanly and the
bracelet stays at full brightness as long as refresh cadence < sustain
duration. Bench auto-fire also deliberately doesn't update
`pmob_last_fire_ms_` — operator is watching the bracelet, not the
screen, and a "Sent!" flash every cycle would just distract.
