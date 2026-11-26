// ============================================================================
//  PHANTOM NET — fall.h
//  Fall detection state machine. Reads the IMU fields of NodeState and
//  writes g_node.status.
// ============================================================================
#pragma once

#include "config.h"

enum FallState : uint8_t {
  FALL_IDLE      = 0,   // normal activity
  FALL_FREEFALL  = 1,   // weightless - something is happening
  FALL_SETTLING  = 2,   // impact seen, waiting to see if they get up
  FALL_CONFIRMED = 3,   // all four stages passed: MAN DOWN
};

void fall_begin();

// Call every loop, AFTER imu_update(). Non-blocking.
void fall_update();

// Clear a confirmed alarm and return to monitoring. This is what a cancel
// button, or an acknowledgement from the gateway, would call.
void fall_reset();

FallState   fall_state();
const char* fall_state_name();

// Details of the most recent candidate event, for the serial log and the
// project report. These are what you quote as measured evidence.
float    fall_last_peak_g();       // impact magnitude
float    fall_last_min_g();        // how weightless it got
uint16_t fall_last_freefall_ms();  // duration of the weightless phase
float    fall_last_tilt_deg();     // orientation change, before vs after