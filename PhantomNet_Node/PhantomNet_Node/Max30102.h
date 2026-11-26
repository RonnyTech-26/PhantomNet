// ============================================================================
//  PHANTOM NET — max30102.h
//  Heart rate and SpO2. Raw register driver, no external library.
// ============================================================================
#pragma once

#include "config.h"

// Probe, identify and configure the sensor. Writes g_node.hr_ok.
// Returns false if the part does not answer or reports the wrong ID.
bool max30102_begin();

// Non-blocking. Call every loop; rate-limits to HR_SAMPLE_MS internally.
// Writes finger, hr_bpm and spo2 into the shared node state.
void max30102_update();

// Latest raw infrared reading. Use it to sanity-check finger detection.
uint32_t max30102_ir();

// Latest raw red reading.
uint32_t max30102_red();

// 0..100. How consistent the recent beat intervals are. Below ~40 the BPM
// figure should not be believed, and SpO2 is suppressed entirely.
uint8_t max30102_quality();

// PART_ID register. 0x15 for a genuine MAX30102.
uint8_t max30102_part_id();

// Amplitude of the pulsating (AC) part of the infrared signal, in raw ADC
// counts. This is the diagnostic that matters when no rate appears:
//   under ~40   no usable pulse - press firmer, or raise the LED current
//   200 - 5000  healthy
// A large steady IR reading with a tiny amplitude means the sensor sees your
// finger but not your pulse.
float max30102_amp();