// ============================================================================
//  PHANTOM NET — gps.h
//  NEO-6M position, parsed from raw NMEA. No external library.
// ============================================================================
#pragma once

#include "config.h"

// Open UART2 and find the module's baud rate by listening at each candidate
// and counting valid NMEA sentences. Writes g_node.gps_ok.
// Returns false if nothing intelligible arrives at any speed.
bool gps_begin();

// Non-blocking. Call every loop. Drains the UART and updates gps_fix, sats,
// lat and lon in the shared node state.
void gps_update();

// Baud rate actually in use, as discovered at boot. 0 if nothing was found.
uint32_t gps_baud();

// Count of complete, checksum-valid NMEA sentences received. If this climbs
// but there is still no fix, the module is working and simply has not seen
// enough sky yet - which is a very different problem from a wiring fault.
uint32_t gps_sentences();

// Sentences rejected for a bad checksum. A handful is normal; a steady
// stream means the baud rate is wrong or the wiring is marginal.
uint32_t gps_errors();

// Seconds since the last valid sentence. 0xFFFF if none has ever arrived.
uint16_t gps_age_s();

// Horizontal dilution of precision from the last GGA. Lower is better:
// under 2 is good, over 5 means the satellite geometry is poor.
float gps_hdop();

// TEMPORARY DIAGNOSTIC. Total satellites currently in view, from the most
// recent GSV group, independent of whether a fix exists. A working module
// under open sky reports this within seconds - if it stays 0 for minutes
// outdoors, the antenna/RF path is the problem, not fix-acquisition time.
uint8_t gps_sats_in_view();