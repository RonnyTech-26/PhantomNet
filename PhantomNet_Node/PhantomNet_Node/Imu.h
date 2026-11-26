// ============================================================================
//  PHANTOM NET — imu.h
//  Motion sensing: MPU6050 / MPU6500 / MPU9250, auto-detected at boot.
// ============================================================================
#pragma once

#include "config.h"

// Probe 0x68 then 0x69, identify the die, configure it. Writes imu_ok and
// imu_name into the shared node state. Returns false if nothing usable.
bool imu_begin();

// Non-blocking. Call every loop; rate-limits to IMU_SAMPLE_MS internally.
void imu_update();

// Measure and store the gyro zero-rate offset. The board MUST be still.
// Called automatically by imu_begin(); exposed for re-calibration later.
void imu_calibrate_gyro(uint16_t samples = 400);

// Human-readable part name, e.g. "MPU6500".
const char* imu_part_name();

// Raw WHO_AM_I byte. A REAL chip returns the same value every boot - if this
// changes between resets, the I2C bus is corrupting reads and nothing else
// this driver reports can be trusted.
uint8_t imu_who_am_i();

// Largest |a| seen since the last call, in milli-g. Reset by reading.
uint16_t imu_peak_mg();

// True once at least one sample has been read successfully.
bool imu_has_data();