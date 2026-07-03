#pragma once

/*
 * birdChirp.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Bird-like chirp sequences for a piezo buzzer that run in parallel with other
 * code (e.g. the LED pulse animation) without blocking the Arduino.
 *
 * Principle:
 *   Instead of playing the tone directly with delay(), each sequence describes a
 *   LIST OF SEGMENTS (sweep, pause, trill, churr, …). When a chirpXxx() function
 *   is called, this list is built and playback starts – the function returns
 *   IMMEDIATELY. chirpUpdate() (called in every loop()) uses millis() to compute
 *   which frequency must be playing right now.
 *
 * Usage:
 *   void setup() {
 *     chirpSetup();
 *   }
 *
 *   void loop() {
 *     pulseUpdate();              // LED animation (non-blocking)
 *     chirpUpdate();              // play sound (non-blocking)
 *
 *     if (eventA) chirpHappy1();      // starts immediately, does not block
 *     if (eventB) chirpWarn2();       // interrupts a running chirp if needed
 *
 *     // ... any other code keeps running unhindered ...
 *   }
 *
 * Cancel:
 *   chirpCancel();                       // abort a running chirp immediately, buzzer off
 *
 * Note: a new chirpXxx() call overwrites a chirp that is still playing.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>

// ── Configuration ──────────────────────────────────────────────────────────────
#define CHIRP_PIN           12    // digital pin of the piezo buzzer
#define CHIRP_MAX_SEGMENTS  48   // buffer size (longest sequence = chirpWarn1 ≈ 43)
// ──────────────────────────────────────────────────────────────────────────────


// ══════════════════════════════════════════════════════════════════════════════
//  SEGMENT DATA MODEL
// ══════════════════════════════════════════════════════════════════════════════

// segment types (plain enum instead of a C++-typed enum; 'type' holds the
// value as a uint8_t)
enum {
  SEG_SWEEP,      // glide a→b
  SEG_PAUSE,      // silence
  SEG_TRILL,      // alternate a↔b, p1 = half-cycle ms
  SEG_DRIFTTRILL, // alternate a↔b with drift, p1 = half-cycle ms, p2 = drift Hz/cycle
  SEG_CHURR,      // noise band, center a→b, p1 = grain ms, p2 = bandwidth
  SEG_SCOLD       // glide a→b with graininess, p1 = grain ms, p2 = graininess ±Hz
};

// typedef struct so the type name 'ChirpSeg' is usable without the 'struct'
// keyword (otherwise required in C)
typedef struct {
  uint8_t  type;
  uint16_t a, b;   // frequency pair (start/end or low/high or center1/center2)
  uint16_t dur;    // duration in ms
  uint16_t p1;     // secondary timing (half-cycle or grain ms)
  int16_t  p2;     // extra (bandwidth / drift Hz / graininess)
} ChirpSeg;

// sequence buffer (refilled on every chirpXxx())
static ChirpSeg _cseq[CHIRP_MAX_SEGMENTS];
static uint8_t  _cLen = 0;

// playback state
static bool          _cPlaying = false;
static uint8_t       _cIdx     = 0;
static unsigned long _cT0      = 0;        // start time of the current segment
static uint16_t      _cLastF   = 0xFFFF;   // last frequency set (0 = silence)
static long          _cWin     = -1;       // current grain/half-cycle window
static int32_t       _cCur     = 0;        // current target frequency in the window


// ══════════════════════════════════════════════════════════════════════════════
//  BUILD HELPERS  (append segments – static, not needed directly)
// ══════════════════════════════════════════════════════════════════════════════

static void _cAdd(uint8_t t, uint16_t a, uint16_t b, uint16_t dur,
                  uint16_t p1, int16_t p2) {
  if (_cLen < CHIRP_MAX_SEGMENTS) {
    // assign field by field instead of brace initialization (in C,
    // 'x = { ... }' is not allowed on an already-existing variable)
    ChirpSeg *s = &_cseq[_cLen++];
    s->type = t;
    s->a    = a;
    s->b    = b;
    s->dur  = dur;
    s->p1   = p1;
    s->p2   = p2;
  }
}

// These six map 1:1 to the building blocks of the blocking version:
static void _qSweep(uint16_t f1, uint16_t f2, uint16_t ms) { _cAdd(SEG_SWEEP, f1, f2, ms, 0, 0); }
static void _qPause(uint16_t ms)                           { _cAdd(SEG_PAUSE, 0, 0, ms, 0, 0); }
static void _qTrill(uint16_t f1, uint16_t f2, uint8_t halfMs, uint16_t ms)
                                                           { _cAdd(SEG_TRILL, f1, f2, ms, halfMs, 0); }
static void _qDriftTrill(uint16_t f1, uint16_t f2, uint8_t halfMs, uint16_t ms, int16_t driftHz)
                                                           { _cAdd(SEG_DRIFTTRILL, f1, f2, ms, halfMs, driftHz); }
static void _qChurr(uint16_t c1, uint16_t c2, uint16_t spread, uint8_t grainMs, uint16_t ms)
                                                           { _cAdd(SEG_CHURR, c1, c2, ms, grainMs, (int16_t)spread); }
static void _qScold(uint16_t fHigh, uint16_t fLow, uint16_t ms)
                                                           { _cAdd(SEG_SCOLD, fHigh, fLow, ms, 4, 170); }

// Start playback of the list just built.
static void _cLaunch() {
  _cIdx = 0;
  _cT0 = millis();
  _cLastF = 0xFFFF;
  _cWin = -1;
  _cPlaying = (_cLen > 0);
}


// ══════════════════════════════════════════════════════════════════════════════
//  PLAYER  –  call in every loop()
// ══════════════════════════════════════════════════════════════════════════════

/**
 * chirpUpdate()
 * Plays the active sequence non-blocking. Returns true while something is
 * still running. Must be called in every loop() iteration.
 */
static bool chirpUpdate() {
  if (!_cPlaying) return false;

  const unsigned long now = millis();
  if (_cIdx >= _cLen) { noTone(CHIRP_PIN); _cPlaying = false; _cLastF = 0; return false; }

  ChirpSeg *s = &_cseq[_cIdx];              // pointer instead of a C++ reference
  const unsigned long t = now - _cT0;       // elapsed time in the current segment

  // segment finished? → advance to the next
  if (t >= s->dur) {
    _cIdx++;
    _cT0 = now;
    _cWin = -1;
    if (_cIdx >= _cLen) { noTone(CHIRP_PIN); _cPlaying = false; _cLastF = 0; return false; }
    return true;
  }

  // determine the current target frequency for this segment
  uint16_t f;
  const uint16_t dur = (s->dur == 0) ? 1 : s->dur;

  switch (s->type) {
    case SEG_PAUSE:
      if (_cLastF != 0) { noTone(CHIRP_PIN); _cLastF = 0; }
      return true;

    case SEG_SWEEP:
      f = (uint16_t)((int32_t)s->a + ((int32_t)s->b - (int32_t)s->a) * (int32_t)t / dur);
      break;

    case SEG_TRILL: {
      long win = t / s->p1;                      // half-cycle window
      if (win != _cWin) { _cWin = win; _cCur = (win & 1) ? s->a : s->b; }
      f = (uint16_t)_cCur;
      break;
    }

    case SEG_DRIFTTRILL: {
      long win = t / s->p1;
      if (win != _cWin) {
        _cWin = win;
        long cyc = win / 2;                      // drift per full cycle
        _cCur = ((win & 1) ? (int32_t)s->a : (int32_t)s->b) + cyc * s->p2;
      }
      f = (uint16_t)_cCur;
      break;
    }

    case SEG_CHURR: {
      long win = t / s->p1;                      // grain window
      if (win != _cWin) {
        _cWin = win;
        int32_t center = (int32_t)s->a + ((int32_t)s->b - (int32_t)s->a) * (int32_t)t / dur;
        _cCur = center - s->p2 / 2 + random(s->p2 + 1);
      }
      f = (uint16_t)_cCur;
      break;
    }

    case SEG_SCOLD: {
      long win = t / s->p1;                      // grain window
      if (win != _cWin) {
        _cWin = win;
        int32_t base = (int32_t)s->a + ((int32_t)s->b - (int32_t)s->a) * (int32_t)t / dur;
        // single-argument random() instead of the C++ overload random(min,max):
        // random(2*p2+1) yields 0..2*p2, minus p2 → -p2..p2
        _cCur = base + (random(2L * s->p2 + 1) - s->p2);
      }
      f = (uint16_t)_cCur;
      break;
    }

    default:
      return true;
  }

  if (f != _cLastF) { tone(CHIRP_PIN, f); _cLastF = f; }
  return true;
}

/** Abort a running chirp immediately and turn the buzzer off. */
static void chirpCancel() { _cPlaying = false; noTone(CHIRP_PIN); _cLastF = 0; }


// ══════════════════════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════════════════════

/** Initialize the buzzer pin – call once in setup(). */
static void chirpSetup() {
  pinMode(CHIRP_PIN, OUTPUT);
  noTone(CHIRP_PIN);
  randomSeed(micros());   // slightly varying rattle texture each start
}


// ══════════════════════════════════════════════════════════════════════════════
//  1.  HAPPY CHIRPING  (≈ 1.4 – 1.7 s)
// ══════════════════════════════════════════════════════════════════════════════

/** chirpHappy1 – "spring sparrow": two melodic verses with trill (≈ 1.6 s) */
static void chirpHappy1() {
  _cLen = 0;
  _qSweep(2500, 4800, 62);  _qPause(35);
  _qSweep(2700, 5200, 68);  _qPause(38);
  _qSweep(2900, 5700, 66);  _qPause(32);
  _qSweep(3200, 6100, 72);  _qPause(45);
  _qTrill(5000, 6600, 18, 280);   _qPause(55);
  _qSweep(2800, 5400, 66);  _qPause(35);
  _qSweep(3000, 5900, 70);  _qPause(32);
  _qSweep(3300, 6300, 72);  _qPause(42);
  _qTrill(5200, 6900, 17, 260);   _qPause(50);
  _qSweep(3400, 6000, 70);  _qPause(38);
  _qSweep(3900, 7000, 90);
  _cLaunch();
}

/** chirpHappy2 – "blackbird melody": up/down arcs with two trills (≈ 1.7 s) */
static void chirpHappy2() {
  _cLen = 0;
  _qSweep(3400, 6100, 68);  _qPause(38);
  _qSweep(5900, 3100, 58);  _qPause(42);
  _qSweep(3200, 6500, 72);  _qPause(38);
  _qSweep(6200, 3400, 60);  _qPause(32);
  _qSweep(3600, 6800, 76);  _qPause(40);
  _qTrill(4800, 6700, 20, 260);   _qPause(50);
  _qSweep(3500, 6600, 70);  _qPause(40);
  _qSweep(6400, 3300, 62);  _qPause(38);
  _qSweep(3700, 6900, 74);  _qPause(36);
  _qSweep(6600, 3600, 58);  _qPause(34);
  _qTrill(5000, 7000, 18, 240);   _qPause(46);
  _qSweep(3800, 7100, 82);  _qPause(36);
  _qSweep(6900, 4200, 65);
  _cLaunch();
}

/** chirpHappy3 – "canary cascade": two chip cascades with drift trill (≈ 1.4 s) */
static void chirpHappy3() {
  _cLen = 0;
  _qSweep(2800, 5500, 58);  _qPause(30);
  _qSweep(3100, 6000, 54);  _qPause(26);
  _qSweep(3400, 6500, 50);  _qPause(22);
  _qSweep(3700, 7000, 46);  _qPause(20);
  _qDriftTrill(5000, 6900, 16, 300, 38);  _qPause(45);
  _qSweep(3000, 5800, 56);  _qPause(28);
  _qSweep(3300, 6300, 52);  _qPause(24);
  _qSweep(3600, 6800, 48);  _qPause(20);
  _qSweep(3900, 7300, 44);  _qPause(18);
  _qDriftTrill(5400, 7300, 15, 280, 35);  _qPause(42);
  _qSweep(5600, 8100, 100);
  _cLaunch();
}


// ══════════════════════════════════════════════════════════════════════════════
//  2.  WARNING CHIRPING  (≈ 2.1 – 2.2 s)
// ══════════════════════════════════════════════════════════════════════════════

/** chirpWarn1 – "agitated tit series": accelerating whistle-chip bursts (≈ 2.2 s) */
static void chirpWarn1() {
  _cLen = 0;
  for (uint8_t i = 0; i < 4; i++) { _qSweep(3200, 5400, 40); _qPause(85); }
  _qPause(125);
  for (uint8_t i = 0; i < 5; i++) { _qSweep(3400, 5700, 36); _qPause(68); }
  _qPause(110);
  for (uint8_t i = 0; i < 6; i++) { _qSweep(3600, 6000, 32); _qPause(54); }
  _qPause(100);
  for (uint8_t i = 0; i < 5; i++) { _qSweep(3800, 6300, 30); _qPause(42); }
  _cLaunch();
}

/** chirpWarn2 – "thrush alarm": groups of falling sweep calls (≈ 2.1 s) */
static void chirpWarn2() {
  _cLen = 0;
  for (uint8_t i = 0; i < 4; i++) { _qSweep(5500 - i*80, 2900 - i*55, 130); _qPause(90); }
  _qPause(155);
  for (uint8_t i = 0; i < 4; i++) { _qSweep(5700 - i*60, 3100 - i*45, 112); _qPause(76); }
  _qPause(132);
  _qSweep(5100, 2500, 215);
  _cLaunch();
}

/** chirpWarn3 – "wheeoo alarm whistle": sharply tapering whistles, rising (≈ 2.2 s) */
static void chirpWarn3() {
  _cLen = 0;
  _qSweep(2600, 6000, 140); _qSweep(6000, 3400, 95); _qPause(165);
  _qSweep(2800, 6300, 128); _qSweep(6300, 3600, 88); _qPause(140);
  _qSweep(3000, 6600, 118); _qSweep(6600, 3800, 82); _qPause(118);
  _qSweep(3200, 6900, 108); _qSweep(6900, 4000, 76); _qPause(98);
  _qSweep(3400, 7200, 100); _qSweep(7200, 4200, 70); _qPause(80);
  _qSweep(3600, 7500, 92);  _qSweep(7500, 4400, 64); _qPause(64);
  _qSweep(3800, 7800, 86);  _qSweep(7800, 4600, 58); _qPause(50);
  _qSweep(4000, 8000, 80);  _qSweep(8000, 4800, 54);
  _cLaunch();
}


// ══════════════════════════════════════════════════════════════════════════════
//  3.  ANGRY CHIRPING  (≈ 2.0 – 2.2 s)
// ══════════════════════════════════════════════════════════════════════════════

/** chirpAngry1 – "wren rattle": hard rolling churr sounds (≈ 2.1 s) */
static void chirpAngry1() {
  _cLen = 0;
  _qChurr(2800, 3400,  900, 4, 480);  _qPause(70);
  _qChurr(3200, 2600, 1000, 4, 420);  _qPause(80);
  _qChurr(2600, 3600, 1100, 5, 520);  _qPause(90);
  _qChurr(3000, 2400,  950, 4, 400);
  _cLaunch();
}

/** chirpAngry2 – "blackbird chatter": accelerating scolds + rattle (≈ 2.1 s) */
static void chirpAngry2() {
  _cLen = 0;
  uint16_t gap = 110;
  for (uint8_t i = 0; i < 5; i++) { _qScold(4200, 2400, 75); _qPause(gap); gap -= 14; }
  _qPause(60);
  for (uint8_t i = 0; i < 8; i++) { _qScold(4500, 2600, 60); _qPause(42); }
  _qPause(80);
  _qChurr(3800, 2800, 1200, 4, 360);
  _cLaunch();
}

/** chirpAngry3 – "magpie scolding": erratic alternation of rattle/scold (≈ 2.1 s) */
static void chirpAngry3() {
  _cLen = 0;
  _qChurr(2400, 3000, 1000, 4, 340);  _qPause(55);
  _qScold(4800, 2200, 70);            _qPause(40);
  _qScold(5000, 2400, 65);            _qPause(70);
  _qChurr(2800, 2200, 1100, 5, 380);  _qPause(60);
  _qScold(4600, 2000, 75);            _qPause(35);
  _qScold(5200, 2600, 60);            _qPause(45);
  _qScold(4400, 1900, 80);            _qPause(75);
  _qChurr(3000, 3600, 1200, 4, 420);  _qPause(55);
  _qScold(5000, 2200, 70);            _qPause(38);
  _qScold(4800, 2000, 65);
  _cLaunch();
}


// ══════════════════════════════════════════════════════════════════════════════
//  4.  NOTIFY CHIRPING  (max. 1 s)
// ══════════════════════════════════════════════════════════════════════════════

/** chirpNotify1 – "double call": two rising arcs (≈ 0.2 s) */
static void chirpNotify1() {
  _cLen = 0;
  _qSweep(3000, 5500, 92);  _qPause(38);
  _qSweep(3800, 6500, 85);
  _cLaunch();
}

/** chirpNotify2 – "upward triad": three rising notes (≈ 0.3 s) */
static void chirpNotify2() {
  _cLen = 0;
  _qSweep(2800, 4500, 82);  _qPause(38);
  _qSweep(3500, 5500, 78);  _qPause(35);
  _qSweep(4400, 6500, 88);
  _cLaunch();
}

/** chirpNotify3 – "whistle with trailing trill" (≈ 0.25 s) */
static void chirpNotify3() {
  _cLen = 0;
  _qSweep(3500, 7000, 100);
  _qTrill(5500, 7000, 14, 130);
  _cLaunch();
}