#pragma once

/*
 * proximity.h  –  I²C variant (RCWL-9200 / HC-SR04)
 * ─────────────────────────────────────────────────────────────────────────────
 * Binary proximity sensor: reports "something near" (< threshold) or "nothing near".
 * Uses the module's I²C mode – the sensor measures internally and returns a
 * finished distance, so there is no edge timing and no warm-up issues anymore.
 *
 * I²C sequence (RCWL-9200, address 0x57):
 *   1) write one byte 0x01      → start measurement
 *   2) wait ~120–150 ms         → sensor measures internally
 *   3) read 3 bytes (H,M,L)     → assemble into a 24-bit value in µm
 *   4) distance [cm] = value / 10000
 *
 * Non-blocking: the wait from step 2 is NOT bridged with delay().
 * Instead, one proxUpdate() call starts the measurement and a later one reads
 * it out (small internal state machine). Since the polling interval (seconds)
 * is far longer than the measurement time, the result is long finished by the
 * time it is read.
 *
 * API (unchanged from the GPIO version):
 *   proxSetup(), proxUpdate() (true on state change), proxIsNear()
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include <Wire.h>

// ── Configuration ──────────────────────────────────────────────────────────────
#define PROX_I2C_ADDR      0x57   // fixed address of the RCWL-9200
#define PROX_THRESHOLD_CM  5      // threshold in cm: below this counts as "near"
#define PROX_POLL_MS       1000   // polling interval in ms (1000–5000 is sensible)
#define PROX_MEASURE_MS    150    // minimum wait after measurement start before reading
// ──────────────────────────────────────────────────────────────────────────────

// threshold in µm (sensor returns µm; cm = µm / 10000)
#define PROX_THRESH_UM     ((uint32_t)PROX_THRESHOLD_CM * 10000UL)

// ── Internal state ──────────────────────────────────────────────────────────────
static bool          _proxNear      = false;  // current binary state
static bool          _proxMeasuring = false;  // is a measurement currently running?
static bool          _proxInit      = false;  // has the first measurement been evaluated?
static unsigned long _proxLastPoll  = 0;       // timestamp of the last measurement start


// ══════════════════════════════════════════════════════════════════════════════
//  INTERNAL FUNCTIONS
// ══════════════════════════════════════════════════════════════════════════════

/** Start a measurement: write one byte 0x01 to the sensor. */
static void _proxTrigger() {
  Wire.beginTransmission(PROX_I2C_ADDR);
  Wire.write((uint8_t)0x01);
  Wire.endTransmission();
}

/**
 * Fetch the result: read 3 bytes and assemble them into µm.
 * Returns true if 3 valid bytes were read.
 * (Pointer instead of a C++ reference, to stay C-compatible.)
 */
static bool _proxReadResult(uint32_t *um) {
  if (Wire.requestFrom(PROX_I2C_ADDR, 3) != 3) return false;
  uint32_t h = Wire.read();
  uint32_t m = Wire.read();
  uint32_t l = Wire.read();
  *um = (h << 16) | (m << 8) | l;
  return true;
}


// ══════════════════════════════════════════════════════════════════════════════
//  PUBLIC API
// ══════════════════════════════════════════════════════════════════════════════

/** Initialize I²C – call once in setup(). */
static void proxSetup() {
  Wire.begin();
}

/**
 * Call on every loop() iteration. Non-blocking:
 *   - If no measurement is active and the poll interval has elapsed → start one.
 *   - If a measurement is active and the measurement time is over → read + evaluate.
 * Returns: true if the near/far state changed on THIS call.
 */
static bool proxUpdate() {
  const unsigned long now = millis();

  if (!_proxMeasuring) {
    // Phase A: start a new measurement if due
    if (_proxInit && (now - _proxLastPoll < PROX_POLL_MS)) return false;
    _proxTrigger();
    _proxMeasuring = true;
    _proxLastPoll  = now;
    return false;                       // result arrives on the next update
  }

  // Phase B: measurement running – only read out after the measurement time has passed
  if (now - _proxLastPoll < PROX_MEASURE_MS) return false;

  _proxMeasuring = false;
  _proxInit      = true;

  uint32_t um;
  if (!_proxReadResult(&um)) return false;  // read error → state unchanged

  const bool wasNear = _proxNear;
  // um == 0 happens when nothing / something too close is detected → treat as "far".
  _proxNear = (um > 0 && um <= PROX_THRESH_UM);
  return (_proxNear != wasNear);
}

/** Current binary state: true = something is near (< threshold). */
static bool proxIsNear() { return _proxNear; }