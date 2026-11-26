// ============================================================================
//  PHANTOM NET — sdlog.h
//  On-board "black box" recorder. MicroSD over SPI, CSV files.
//
//  WHY THIS EXISTS
//  ---------------
//  Radio coverage is not guaranteed. Underground, inside a steel structure,
//  or simply out of range, telemetry sent over LoRa is gone the moment it
//  is not received - there is nobody to hear it and nothing stores it.
//
//  This module writes the same telemetry to physical flash on the device
//  itself, with no dependence on the network at all. If a node goes out of
//  range for ten minutes and something happens in that window, the radio
//  record has a ten-minute hole in it. The card does not.
//
//  After an incident the card is removed and read directly, giving a
//  second-by-second account of the minutes leading up to it.
// ============================================================================
#pragma once

#include "config.h"

// Mount the card and open this boot's log file. MUST be called AFTER
// lora_begin(), because both share the SPI bus and the sketch initialises
// that bus once, before either driver runs.
// Writes g_node.sd_ok. Returns false if no card is present or unreadable.
bool sdlog_begin();

// Non-blocking. Call every loop; rate-limits to SDLOG_PERIOD_MS internally.
// Appends one CSV row of the current NodeState.
void sdlog_update();

// Write an event row immediately and force it to physical flash. Use for
// anything that must survive a power loss in the next millisecond - a
// confirmed fall, a rejected fall, a node going offline.
void sdlog_event(const char* tag, const char* detail);

// Force cached bytes out to the card. sdlog_update() does this
// periodically; call it directly before a deliberate shutdown.
void sdlog_flush();

// --- status, for the display and the serial heartbeat ---------------------
const char* sdlog_filename();    // e.g. "/PN_0007.CSV"
uint32_t    sdlog_rows();        // data rows written this boot
uint32_t    sdlog_bytes();       // bytes written this boot
uint32_t    sdlog_errors();      // failed writes
uint32_t    sdlog_card_mb();     // total card size, MB
uint32_t    sdlog_free_mb();     // free space, MB
