/*
 * SipSyncFinal.ino
 * ==============================================================================
 * SipSync – state machine
 *
 * States:
 *   IDLE     – white pulsing, waiting for the cup
 *   CONFIRM  – green wave (confirm placement); chirpNotify at the same time
 *   WAITING  – "all good" (matrix dark)
 *   YELLOW   – yellow breathing (brightness 50)
 *   ORANGE   – orange breathing (brightness 120),
 *              chirpWarn on entry, then at the configured chirp interval
 *   RED      – red breathing (brightness 180), endless until "far" is detected;
 *              chirpAngry on entry, then at the configured chirp interval
 *   HAPPY    – cup removed from a "cup present" state: radial rainbow + happy
 *              chirp as a "well done, you drank!" acknowledgement, then IDLE
 *
 * Global rule: if "far" is detected while in WAITING/YELLOW/ORANGE/RED
 * (ultrasonic sensor > 5 cm), the system immediately switches to HAPPY and
 * afterwards returns to IDLE.
 *
 * State durations and chirp rhythm are configured centrally below (see the
 * "Configuration" section). CONFIRM is excluded on purpose: its length is
 * governed by the quick green breathing animation, not by a timer.
 *
 * Chirp selection: one of 3 variants chosen pseudo-randomly via (millis() % 3).
 * ==============================================================================
 */

#include "ledMatrix.h"
#include "birdChirp.h"
#include "proximity.h"

// ===== Configuration =====
// Central place to tune all state durations and the chirp rhythm.
//
// State durations (milliseconds). CONFIRM is not listed: its length is set by
// the quick green breathing animation. RED has no duration either – it stays
// active until the cup is removed ("far"). IDLE is event-driven (waits for the
// cup), and HAPPY runs for the rainbow animation length (RAINBOW_DURATION_MS in
// ledMatrix.h).
const unsigned long WAITING_DURATION_MS = 30000UL;  // WAITING -> YELLOW
const unsigned long YELLOW_DURATION_MS  = 30000UL;  // YELLOW  -> ORANGE
const unsigned long ORANGE_DURATION_MS  = 30000UL;  // ORANGE  -> RED

// Chirp rhythm (milliseconds, relative to state entry). The first chirp plays
// on entry; each following chirp is spaced one interval further apart.
// ORANGE plays 2 escalation chirps (at 1x and 2x the interval).
// RED plays 3 escalation chirps (at 1x, 2x and 3x the interval).
const unsigned long ORANGE_CHIRP_INTERVAL_MS = 10000UL;
const unsigned long RED_CHIRP_INTERVAL_MS    = 10000UL;

// ===== State model (C-compatible typedef enum) =====
typedef enum { IDLE, CONFIRM, WAITING, YELLOW, ORANGE, RED, HAPPY } State;

State         state          = IDLE;
unsigned long stateStartTime = 0;     // entry timestamp of the current state
uint8_t       chirpsPlayed   = 0;     // counter for escalation chirps

// ===== Chirp tables + random selection =====
typedef void (*ChirpFn)(void);

ChirpFn notifyChirps[] = { chirpNotify1, chirpNotify2, chirpNotify3 };
ChirpFn warnChirps[]   = { chirpWarn1,   chirpWarn2,   chirpWarn3   };
ChirpFn angryChirps[]  = { chirpAngry1,  chirpAngry2,  chirpAngry3  };
ChirpFn happyChirps[]  = { chirpHappy1,  chirpHappy2,  chirpHappy3  };

/**
 * Picks one of the three given chirps at random (millis()%3) and plays it.
 * Cancels any chirp that may still be playing first so nothing overlaps.
 */
void playRandom(ChirpFn *fns) {
  chirpCancel();
  fns[millis() % 3]();
}

// ===== State transitions ===== 

void enterIDLE() {
  chirpCancel();
  breathStop();              // stop any running breathing
  rainbowStop();             // stop any running rainbow
  pulseStart();              // white pulsing
  state          = IDLE;
  stateStartTime = millis();
}

void enterCONFIRM() {
  pulseStop();
  quickGreenBreathStart();   // 1 s fast green breathing
  playRandom(notifyChirps);  // chirpNotify at the SAME TIME as the breathing start
  state          = CONFIRM;
  stateStartTime = millis();
}

void enterWAITING() {
  // breathing has ended by itself after 1 s -> matrix already dark
  state          = WAITING;
  stateStartTime = millis();
}

/**
 * HAPPY: the cup was removed from a "cup present" state.
 * Played as a "well done, you drank!" feedback:
 * radial rainbow (3 s) plus chirpHappy at its start.
 * Afterwards back to IDLE.
 */
void enterHAPPY() {
  breathStop();              // stop any running breathing
  rainbowStart();            // radial rainbow (RAINBOW_DURATION_MS)
  playRandom(happyChirps);   // happy chirp at the start
  state          = HAPPY;
  stateStartTime = millis();
}

void enterYELLOW() {
  yellowBreathStart();       // yellow, cycle BREATH_YELLOW_MS
  state          = YELLOW;
  stateStartTime = millis();
}

void enterORANGE() {
  orangeBreathStart();       // orange, cycle BREATH_ORANGE_MS
  playRandom(warnChirps);    // chirp #1 (on entry)
  chirpsPlayed = 1;
  state          = ORANGE;
  stateStartTime = millis();
}

void enterRED() {
  redBreathStart();          // red, cycle BREATH_RED_MS
  playRandom(angryChirps);   // chirp #1 (on entry)
  chirpsPlayed = 1;
  state          = RED;
  stateStartTime = millis();
}

// ===== setup / loop =====

void setup() {
  pulseSetup();              // initializes the shared NeoPixel object
  chirpSetup();
  proxSetup();
  pulseStart();              // IDLE visual active immediately
  state          = IDLE;
  stateStartTime = millis();
}

void loop() {
  chirpUpdate();             // buzzer, non-blocking
  proxUpdate();              // sensor, non-blocking
  const bool          isNear     = proxIsNear();
  const unsigned long elapsed = millis() - stateStartTime;

  // Global rule: in the "cup present" states, removing the cup
  // (sensor reports "far") switches to HAPPY – radial rainbow + happy chirp
  // as a "well done!" acknowledgement. Afterwards back to IDLE.
  if (!isNear && (state == WAITING || state == YELLOW
            || state == ORANGE  || state == RED)) {
    enterHAPPY();
    return;
  }

  switch (state) {

    case IDLE:
      pulseUpdate();
      if (isNear) enterCONFIRM();
      break;

    case CONFIRM:
      // 1 s fast green breathing is running; then WAITING
      if (!breathUpdate()) enterWAITING();
      break;

    case HAPPY:
      // radial rainbow is running (RAINBOW_DURATION_MS); then back to IDLE
      if (!rainbowUpdate()) enterIDLE();
      break;

    case WAITING:
      // "all good" – matrix dark, just watch the timer
      if (elapsed >= WAITING_DURATION_MS) enterYELLOW();
      break;

    case YELLOW:
      // breathing loops continuously
      if (!breathUpdate()) yellowBreathStart();
      if (elapsed >= YELLOW_DURATION_MS) enterORANGE();
      break;

    case ORANGE:
      if (!breathUpdate()) orangeBreathStart();
      // chirp #1 was played on entry -> chirpsPlayed = 1
      if (chirpsPlayed == 1 && elapsed >= ORANGE_CHIRP_INTERVAL_MS)     { playRandom(warnChirps); chirpsPlayed = 2; }
      if (chirpsPlayed == 2 && elapsed >= 2 * ORANGE_CHIRP_INTERVAL_MS) { playRandom(warnChirps); chirpsPlayed = 3; }
      if (elapsed >= ORANGE_DURATION_MS) enterRED();
      break;

    case RED:
      if (!breathUpdate()) redBreathStart();
      // chirp #1 was played on entry -> chirpsPlayed = 1
      if (chirpsPlayed == 1 && elapsed >= RED_CHIRP_INTERVAL_MS)     { playRandom(angryChirps); chirpsPlayed = 2; }
      if (chirpsPlayed == 2 && elapsed >= 2 * RED_CHIRP_INTERVAL_MS) { playRandom(angryChirps); chirpsPlayed = 3; }
      if (chirpsPlayed == 3 && elapsed >= 3 * RED_CHIRP_INTERVAL_MS) { playRandom(angryChirps); chirpsPlayed = 4; }
      // breathing keeps running; return to IDLE happens above on "far".
      break;
  }
}
