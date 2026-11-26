// ============================================================================
//  PHANTOM NET — button.h
//  Manual SOS / acknowledge button. One GPIO, internal pull-up, no external
//  resistor needed. Reads and writes g_node.status; calls fall_reset() when
//  clearing - the exact use fall.h's own comment names for that function.
//
//    NORMAL / ALERT   --[DOUBLE-CLICK]-->  SOS      "I need help"
//    MANDOWN / SOS    --[HOLD]        -->  NORMAL   "I'm OK"
//
//  WHY THESE TWO GESTURES, NOT ONE
//  --------------------------------
//  Declaring an emergency and standing one down are opposite-stakes actions
//  and should not share a trigger:
//
//    DOUBLE-CLICK to declare - two deliberate presses inside a short window
//    is very unlikely from a bump, a snag on clothing, or brushing the case
//    against something. A single accidental contact can never fire this.
//
//    HOLD to clear - a sustained, unhurried press is the right gesture for
//    "I am conscious, capable, and choosing to end this alert" - the person
//    doing it is, by definition, in a state to deliberately hold a button
//    down. A brief accidental contact cannot cancel a real alarm either.
//
//  Both gestures are read from ONE pin with no press left ambiguous: a
//  press that turns into a hold is never later also counted as a click,
//  and a click that never gets a second click within the window quietly
//  does nothing rather than firing anything by half-measure.
// ============================================================================
#pragma once

#include "config.h"

// Configures PIN_SOS_BUTTON as INPUT_PULLUP. Call once in setup(), any time
// after Serial is up.
void button_begin();

// Non-blocking. Call every loop. Debounces internally; classifies each
// press as part of a double-click or a hold, never both.
void button_update();

// Each true exactly once, on the loop where that action just fired. Use
// these to log the event (SD black box, dashboard) without duplicating the
// click/hold logic anywhere else.
bool button_event_sos();      // double-click seen, NORMAL/ALERT -> SOS
bool button_event_clear();    // hold seen, MANDOWN/SOS -> NORMAL