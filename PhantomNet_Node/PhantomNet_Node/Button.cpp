// ============================================================================
//  PHANTOM NET — button.cpp
//  See button.h for the design rationale (double-click to declare, hold to
//  clear).
//
//  ONE PIN, THREE PHASES
//  -----------------------
//  IDLE          nothing pending
//  DOWN          currently pressed - timing whether this becomes a HOLD
//  WAIT_SECOND   a completed short press just released - waiting to see if
//                a second press starts within BUTTON_DOUBLECLICK_MS
//
//  A press classified as a HOLD fires immediately, the instant the hold
//  threshold is crossed - it does not wait for release. A press that is
//  released before the hold threshold is a completed CLICK, which either
//  becomes the first half of a double-click or, if no second press follows
//  in time, is silently discarded. There is no single-click action.
// ============================================================================

#include "button.h"
#include "fall.h"

// ---------------------------------------------------------------------------
//  TEMPORARY DIAGNOSTIC — set to 1 to print every raw press/release and
//  every phase transition. Turns "nothing happened" into a visible trace of
//  exactly what the pin did and how the state machine read it. Set back to
//  0 once the button is confirmed working - this is verbose.
// ---------------------------------------------------------------------------
#define BUTTON_DEBUG_VERBOSE  1

enum BtnPhase : uint8_t { BTN_IDLE, BTN_DOWN, BTN_WAIT_SECOND };

// ---------------------------------------------------------------------------
//  STATE
// ---------------------------------------------------------------------------
static bool     s_lastRaw    = true;    // idle = HIGH with pull-up
static bool     s_debounced  = true;
static uint32_t s_lastEdgeMs = 0;

static BtnPhase s_phase       = BTN_IDLE;
static uint32_t s_downSinceMs = 0;
static uint32_t s_waitSinceMs = 0;
static bool     s_firedAsHold = false;   // this DOWN already acted as a hold

static bool s_evSos   = false;
static bool s_evClear = false;

// ---------------------------------------------------------------------------
bool button_event_sos()   { const bool v = s_evSos;   s_evSos   = false; return v; }
bool button_event_clear() { const bool v = s_evClear; s_evClear = false; return v; }

// ---------------------------------------------------------------------------
void button_begin() {
  pinMode(PIN_SOS_BUTTON, INPUT_PULLUP);
  s_lastRaw = s_debounced = digitalRead(PIN_SOS_BUTTON);

  Serial.printf("# BUTTON: armed on GPIO%d - double-click (within %ums) "
                "declares SOS, hold %ums clears an active alarm\n",
                PIN_SOS_BUTTON, (unsigned)BUTTON_DOUBLECLICK_MS,
                (unsigned)BUTTON_HOLD_MS);
  Serial.printf("# BUTTON: idle pin reads %s (expect HIGH - if this says LOW "
                "at boot with nothing pressed, check the wiring: the far "
                "side of the button must go to GND, not 3V3)\n",
                s_lastRaw ? "HIGH" : "LOW");
}

// ---------------------------------------------------------------------------
static void fireSos() {
  if (g_node.status == STATUS_NORMAL || g_node.status == STATUS_ALERT) {
    g_node.status = STATUS_SOS;
    s_evSos = true;
    Serial.println("# BUTTON: DOUBLE-CLICK - manual SOS declared (\"need help\")");
  } else {
    Serial.println("# BUTTON: double-click ignored - already in an alarm state");
  }
}

static void fireClear() {
  if (g_node.status == STATUS_MANDOWN || g_node.status == STATUS_SOS) {
    const bool wasSos = (g_node.status == STATUS_SOS);
    fall_reset();                                // clears STATUS_MANDOWN, if active
    if (wasSos) g_node.status = STATUS_NORMAL;   // fall_reset() only clears MANDOWN
    s_evClear = true;
    Serial.println("# BUTTON: HELD - alarm acknowledged and cleared (\"I'm OK\")");
  } else {
    Serial.println("# BUTTON: hold ignored - nothing active to clear");
  }
}

// ---------------------------------------------------------------------------
void button_update() {
  const uint32_t now = millis();
  const bool     raw = digitalRead(PIN_SOS_BUTTON);   // LOW = pressed

  // --- debounce: only accept a level once it has been stable this long ---
  if (raw != s_lastRaw) { s_lastEdgeMs = now; s_lastRaw = raw; }
  const bool edge = (now - s_lastEdgeMs >= BUTTON_DEBOUNCE_MS) &&
                    (s_debounced != raw);
  if (edge) s_debounced = raw;

#if BUTTON_DEBUG_VERBOSE
  if (edge) {
    Serial.printf("# BUTTON: edge -> %s  (phase was %d)\n",
                  s_debounced == LOW ? "PRESSED" : "released",
                  (int)s_phase);
  }
#endif

  switch (s_phase) {

    // -----------------------------------------------------------------
    case BTN_IDLE:
      if (edge && s_debounced == LOW) {          // fresh press
        s_phase       = BTN_DOWN;
        s_downSinceMs = now;
        s_firedAsHold = false;
#if BUTTON_DEBUG_VERBOSE
        Serial.println("# BUTTON: -> DOWN (timing for hold vs click)");
#endif
      }
      break;

    // -----------------------------------------------------------------
    case BTN_DOWN:
      // Still held past the hold threshold: this press IS a hold. Fires
      // right here, the instant it's earned - not on release - so an "I'm
      // OK" is acted on as soon as it is genuinely meant, not delayed.
      if (!s_firedAsHold && (now - s_downSinceMs >= BUTTON_HOLD_MS)) {
        s_firedAsHold = true;
        fireClear();
      }

      if (edge && s_debounced == HIGH) {         // released
        if (s_firedAsHold) {
          s_phase = BTN_IDLE;                    // already handled, nothing more to do
        } else {
          // A completed short press - the first half of a possible
          // double-click. Open the window for a second one.
          s_phase       = BTN_WAIT_SECOND;
          s_waitSinceMs = now;
#if BUTTON_DEBUG_VERBOSE
          Serial.printf("# BUTTON: click #1 done (held %ums) -> waiting up to "
                        "%ums for a second click\n",
                        (unsigned)(now - s_downSinceMs),
                        (unsigned)BUTTON_DOUBLECLICK_MS);
#endif
        }
      }
      break;

    // -----------------------------------------------------------------
    case BTN_WAIT_SECOND:
      if (edge && s_debounced == LOW) {          // second press arrived in time
#if BUTTON_DEBUG_VERBOSE
        Serial.printf("# BUTTON: click #2 arrived %ums after click #1 released "
                      "-> DOUBLE-CLICK\n", (unsigned)(now - s_waitSinceMs));
#endif
        s_phase = BTN_IDLE;
        fireSos();
        break;
      }
      if (now - s_waitSinceMs >= BUTTON_DOUBLECLICK_MS) {
        // Window closed with only one click. A single click does nothing
        // on purpose - there is no single-click action defined, precisely
        // so an accidental single knock cannot declare or clear anything.
#if BUTTON_DEBUG_VERBOSE
        Serial.println("# BUTTON: double-click window expired - only one "
                        "click seen, no action (were the two presses too "
                        "far apart?)");
#endif
        s_phase = BTN_IDLE;
      }
      break;
  }
}