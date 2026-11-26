#pragma once
// ============================================================================
//  PHANTOM NET — ml_fall.h
//
//  A Random-Forest fall classifier, running ALONGSIDE fall.cpp as a second,
//  independent opinion — not a replacement for it.
//
//  fall.cpp is the primary, safety-critical detector: a fixed physics state
//  machine (free-fall -> impact -> stillness -> tilt), already tuned and
//  proven on real drops. This module never overrides it and never blocks
//  its alarm. It computes a second number, P(fall) from 0 to 1, from the
//  same IMU stream, using a model trained on real captured falls AND real
//  "looks like a fall but isn't" data (helmet handling, bends, sit-stands,
//  walking, head turns). Log both opinions together: when they agree, that
//  is strong evidence for the report; when they disagree, that is useful
//  information too, not a bug to hide.
// ============================================================================
#include <stdint.h>

// Call once in setup(), after imu_begin().
void ml_fall_begin();

// Call every loop(), right after imu_update(). Cheap - only actually
// evaluates the model once every 250 ms, after the first 1-second window
// has filled.
void ml_fall_update();

// True once the first full 1-second window has been scored.
bool ml_fall_ready();

// The most recent P(fall), 0..1. Updates ~4 times per second.
float ml_fall_score();
