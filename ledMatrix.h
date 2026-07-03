#pragma once

/*
 * ledMatrix.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Contains three animations for the same 8×8 matrix:
 *   1) Pulse     – white ring pulsing (outside → inside), endless until pulseStop()
 *   2) Rainbow   – color rings running radially from the center outward (HAPPY)
 *   3) Breathing – single-color fade in/out (CONFIRM/YELLOW/ORANGE/RED)
 *
 * In all animations only the pixels of the circle mask (_mtxInCircle) light up;
 * the four corners stay dark so the round shape is preserved.
 *
 * IMPORTANT: only ONE animation may run at a time – so per loop() iteration
 * call only the update of the currently active animation.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <Adafruit_NeoPixel.h>

// ── Matrix (shared) ─────────────────────────────────────────────────────────────
#define MATRIX_PIN        11        // data pin of the LED matrix
#define MATRIX_NUM_LEDS   64       // 8 × 8

// ── Pulse animation ─────────────────────────────────────────────────────────────
#define PULSE_MIN_BRIGHT  4        // minimum brightness – LEDs never go fully dark
#define PULSE_MAX_BRIGHT  30       // max brightness per channel (0–255)
#define PULSE_PERIOD_MS   2800UL   // duration of one full pulse cycle
#define PULSE_PHASE_RAD   (PI / 4.0f)  // phase offset per ring (= 45°)
#define PULSE_UPDATE_MS   25       // frame interval (≈ 40 fps)
// ──────────────────────────────────────────────────────────────────────────────

// ── ONE shared matrix instance ──────────────────────────────────────────────────
static Adafruit_NeoPixel _mtx(MATRIX_NUM_LEDS, MATRIX_PIN, NEO_GRB + NEO_KHZ800);
static bool _mtxBegun = false;

// pixel index for a serpentine-wired 8×8 matrix
static uint8_t _mtxXY(uint8_t x, uint8_t y) {
  return (y & 1) ? (y * 8 + 7 - x) : (y * 8 + x);
}

// ring index (0 = outer edge … 3 = center), only for the pulse animation
static uint8_t _mtxRing(uint8_t x, uint8_t y) {
  uint8_t dx = (x < 4) ? x : (uint8_t)(7 - x);
  uint8_t dy = (y < 4) ? y : (uint8_t)(7 - y);
  return min(dx, dy);
}

// ── Circle mask ──────────────────────────────────────────────────────────────────
// Instead of the full 8×8 area, in ALL animations only the pixels of the round
// region light up; the four corners stay dark. One byte per row, bit x = 1
// means "LED on" (x = 0 is bit 0). Shape (symmetric, so left/right does not matter):
//   ..####..
//   .######.
//   ########
//   ########
//   ########
//   ########
//   .######.
//   ..####..
static const uint8_t _MTX_CIRCLE[8] = {
  0b00111100,  // row 0  – columns 2..5
  0b01111110,  // row 1  – columns 1..6
  0b11111111,  // row 2
  0b11111111,  // row 3
  0b11111111,  // row 4
  0b11111111,  // row 5
  0b01111110,  // row 6  – columns 1..6
  0b00111100   // row 7  – columns 2..5
};

// true if pixel (x, y) belongs to the circle (i.e. may light up).
static bool _mtxInCircle(uint8_t x, uint8_t y) {
  return (_MTX_CIRCLE[y] >> x) & 0x01;
}

// shared initialization (calling it multiple times does no harm)
static void matrixSetup() {
  if (_mtxBegun) return;
  _mtx.begin();
  _mtx.clear();
  _mtx.show();
  _mtxBegun = true;
}


// ══════════════════════════════════════════════════════════════════════════════
//  1.  PULSE  (white ring pulsing, endless until pulseStop())
// ══════════════════════════════════════════════════════════════════════════════

static bool          _pmActive = false;
static unsigned long _pmT0     = 0;
static unsigned long _pmLast   = 0;

/** Call once in setup() (initializes the shared matrix). */
static void pulseSetup() { matrixSetup(); }

/** Start pulsing (or restart). */
static void pulseStart() { _pmActive = true; _pmT0 = millis(); _pmLast = 0; }

/** Stop pulsing and turn the matrix off. */
static void pulseStop()  { _pmActive = false; _mtx.clear(); _mtx.show(); }

/** Frame update – non-blocking, call in every loop() while the pulse is active. */
static void pulseUpdate() {
  if (!_pmActive) return;
  const unsigned long now = millis();
  if (now - _pmLast < PULSE_UPDATE_MS) return;
  _pmLast = now;

  const float base = TWO_PI
    * (float)((now - _pmT0) % PULSE_PERIOD_MS) / (float)PULSE_PERIOD_MS;

  for (uint8_t y = 0; y < 8; y++) {
    for (uint8_t x = 0; x < 8; x++) {
      if (!_mtxInCircle(x, y)) {                 // corners stay dark
        _mtx.setPixelColor(_mtxXY(x, y), 0);
        continue;
      }
      const float   a = base - (float)_mtxRing(x, y) * PULSE_PHASE_RAD;
      const uint8_t b = PULSE_MIN_BRIGHT
        + (uint8_t)((sinf(a) + 1.0f) * 0.5f * (PULSE_MAX_BRIGHT - PULSE_MIN_BRIGHT));
      _mtx.setPixelColor(_mtxXY(x, y), _mtx.Color(b, b, b));
    }
  }
  _mtx.show();
}


// ══════════════════════════════════════════════════════════════════════════════
//  2.  RADIAL RAINBOW  (HAPPY)
//
//  A rainbow whose hue depends on the RING number (distance from the center
//  3.5 | 3.5). Over time an offset travels through the spectrum so the colors
//  run from the center OUTWARD and repeat periodically.
//
//  So the individual colors stay clearly visible on the coarse grid,
//  RAINBOW_RING_GROUP neighboring rings share ONE color each (default 2):
//  this makes the color steps wider and easier to tell apart.
//
//  AVR-friendly: each pixel's ring number is geometrically fixed and is
//  therefore precomputed ONCE and stored in flash (_RB_RING, 0 = center .. 6 =
//  edge). Per frame only an offset is added, ColorHSV() yields the color –
//  no float, no sqrt in loop().
// ══════════════════════════════════════════════════════════════════════════════

// ── Configuration (freely adjustable) ──────────────────────────────────────────
#define RAINBOW_DURATION_MS  4000U   // total display duration of the effect (initially 4 s)
#define RAINBOW_PERIOD_MS    800U   // cycle duration until repetition (initially 0.8 s)
#define RAINBOW_RING_GROUP   2       // how many neighboring rings share ONE color
                                     //   2 = two rings share a color (clearly visible)
                                     //   1 = each ring its own color (finer gradient)
#define RAINBOW_HUESTEP      64       // hue distance between color groups (0..255)
                                     //   64 -> 4 clearly separated colors across the radius
#define RAINBOW_VALUE        200     // peak brightness 0..255 (like breathing)
#define RAINBOW_UPDATE_MS    25      // frame interval (~40 fps)

// precomputed ring number per pixel (index = y*8 + x), 0 = center .. 6 = edge.
// 255 = corner pixel (does not light up, value irrelevant).
static const uint8_t _RB_RING[64] PROGMEM = {
  255, 255,   6,   5,   5,   6, 255, 255,   // row 0
  255,   5,   4,   3,   3,   4,   5, 255,   // row 1
    6,   4,   2,   1,   1,   2,   4,   6,   // row 2
    5,   3,   1,   0,   0,   1,   3,   5,   // row 3
    5,   3,   1,   0,   0,   1,   3,   5,   // row 4
    6,   4,   2,   1,   1,   2,   4,   6,   // row 5
  255,   5,   4,   3,   3,   4,   5, 255,   // row 6
  255, 255,   6,   5,   5,   6, 255, 255    // row 7
};

static bool          _rbActive = false;
static unsigned long _rbT0     = 0;
static unsigned long _rbLast   = 0;

/** Start the radial rainbow (or restart). */
static void rainbowStart() { _rbActive = true; _rbT0 = millis(); _rbLast = 0; }

/** Stop early and turn the matrix off. */
static void rainbowStop()  { _rbActive = false; _mtx.clear(); _mtx.show(); }

/**
 * Frame update – non-blocking, call in every loop().
 * Returns true while the animation is running; false on the frame it ends.
 */
static bool rainbowUpdate() {
  if (!_rbActive) return false;

  const unsigned long now     = millis();
  const unsigned long elapsed = now - _rbT0;

  if (elapsed >= RAINBOW_DURATION_MS) { rainbowStop(); return false; }
  if (now - _rbLast < RAINBOW_UPDATE_MS) return true;
  _rbLast = now;

  // phase offset 0..255 for this frame: one full spectrum sweep per
  // RAINBOW_PERIOD_MS (= cycle duration until repetition).
  const uint8_t phase =
    (uint8_t)((elapsed % RAINBOW_PERIOD_MS) * 256UL / RAINBOW_PERIOD_MS);

  for (uint8_t y = 0; y < 8; y++) {
    for (uint8_t x = 0; x < 8; x++) {
      if (!_mtxInCircle(x, y)) {                 // corners stay dark
        _mtx.setPixelColor(_mtxXY(x, y), 0);
        continue;
      }
      const uint8_t ring = pgm_read_byte(&_RB_RING[y * 8 + x]);
      const uint8_t band = ring / RAINBOW_RING_GROUP;   // every GROUP rings = one color
      // hue = color group·step - phase  →  colors run OUTWARD (mod 256).
      const uint8_t  hue8 = (uint8_t)((uint16_t)(band * RAINBOW_HUESTEP) - phase);
      const uint16_t hue  = (uint16_t)hue8 << 8;         // 0..65535 for ColorHSV
      _mtx.setPixelColor(_mtxXY(x, y), _mtx.ColorHSV(hue, 255, RAINBOW_VALUE));
    }
  }
  _mtx.show();
  return true;
}


// ══════════════════════════════════════════════════════════════════════════════
//  3.  BREATHING  –  fade the whole matrix gently in/out in a single color
//
//  Pure integer arithmetic (no cosf, no float) – copes better with the AVR Uno.
//  Triangle wave: MIN -> 255 -> MIN over _brDur milliseconds.
//  breathStartDur(r,g,b,durMs)    -> breathing cycle with any duration in ms
//  yellow/orange/redBreathStart() -> warning states with their cycle durations
// ══════════════════════════════════════════════════════════════════════════════

#define BREATH_DEFAULT_MS  5000U   // initial value for _brDur (before the first start)
#define BREATH_UPDATE_MS   25      // frame interval (about 40 fps)

// ── Minimum brightness ──────────────────────────────────────────────────────────
// Lower bound of the brightness scaling (0..255). When "exhaling", the brightness
// no longer falls all the way to 0, only down to this fraction of the peak.
// This avoids the abrupt light/dark jump. 0 = goes fully off. 30 ≈ 12 % glow.
#define BREATH_MIN_LEVEL   10

// ── Cycle durations of the warning states (freely adjustable for experimenting) ──
#define BREATH_YELLOW_MS   5000U   // yellow : one full breathing cycle = 5.0 s
#define BREATH_ORANGE_MS   3000U   // orange : 3.0 s
#define BREATH_RED_MS      1500U   // red    : 1.5 s (fast)

static bool          _brActive = false;
static unsigned long _brT0     = 0;
static unsigned long _brLast   = 0;
static uint16_t      _brDur    = BREATH_DEFAULT_MS;   // current breathing duration
static uint8_t       _brR = 0, _brG = 0, _brB = 0;    // peak color

/** Breathing cycle with configurable duration (1..65535 ms). */
static void breathStartDur(uint8_t r, uint8_t g, uint8_t b, uint16_t durMs) {
  _brR = r; _brG = g; _brB = b;
  _brDur = (durMs == 0) ? 1 : durMs;
  _brActive = true; _brT0 = millis(); _brLast = 0;
}

/** Yellow breathing – peak brightness 50, cycle BREATH_YELLOW_MS. */
static void yellowBreathStart() { breathStartDur(50, 50, 0, BREATH_YELLOW_MS); }

/** Orange breathing – peak R120/G50, cycle BREATH_ORANGE_MS. */
static void orangeBreathStart() { breathStartDur(120, 50, 0, BREATH_ORANGE_MS); }

/** Red breathing – peak R180, cycle BREATH_RED_MS (fast). */
static void redBreathStart() { breathStartDur(180, 0, 0, BREATH_RED_MS); }

/** Fast green breathing as confirmation when the cup is placed (1 s). */
static void quickGreenBreathStart() { breathStartDur(0, 50, 0, 1000); }

/** Stop early and turn the matrix off. */
static void breathStop() {
  _brActive = false;
  _mtx.clear(); _mtx.show();
}

/**
 * Frame update – non-blocking, call in every loop() iteration.
 * Returns true while the animation is running.
 */
static bool breathUpdate() {
  if (!_brActive) return false;

  const unsigned long now     = millis();
  const unsigned long elapsed = now - _brT0;

  if (elapsed >= _brDur) { breathStop(); return false; }
  if (now - _brLast < BREATH_UPDATE_MS) return true;
  _brLast = now;

  // triangle wave 0 -> 255 -> 0 over _brDur (in integer arithmetic)
  const unsigned long half = (unsigned long)_brDur / 2UL;
  uint16_t k;
  if (elapsed < half) {
    k = (uint16_t)((elapsed * 255UL) / half);              // rising
  } else {
    k = (uint16_t)((((unsigned long)_brDur - elapsed) * 255UL) / half);  // falling
  }

  // pull in a lower bound: lift k from [0..255] to [BREATH_MIN_LEVEL..255],
  // so the breathing never goes fully off (no abrupt jump at the low point).
  k = (uint16_t)BREATH_MIN_LEVEL
    + (uint16_t)(((uint32_t)k * (255UL - BREATH_MIN_LEVEL)) / 255UL);

  // color scaling via bit shift instead of division: (channel * k) / 256
  const uint8_t r = (uint8_t)(((uint16_t)_brR * k) >> 8);
  const uint8_t g = (uint8_t)(((uint16_t)_brG * k) >> 8);
  const uint8_t b = (uint8_t)(((uint16_t)_brB * k) >> 8);
  const uint32_t color = _mtx.Color(r, g, b);

  for (uint8_t y = 0; y < 8; y++) {
    for (uint8_t x = 0; x < 8; x++) {
      // corners stay dark, the rest breathes along in one color
      _mtx.setPixelColor(_mtxXY(x, y), _mtxInCircle(x, y) ? color : 0);
    }
  }
  _mtx.show();
  return true;
}