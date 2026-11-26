// ============================================================================
//  PHANTOM NET — pn_lora.h
//  SX1278 telemetry link. Broadcasts this node's NodeState to every other
//  node in range and absorbs theirs.
//
//  This is the PLAINTEXT stage. The frame carries the same header fields as
//  the encrypted format in 04_PROTOCOL.md, so moving to AES-GCM later means
//  wrapping the payload, not redesigning the packet.
// ============================================================================
#pragma once

#include "config.h"

// Bring up SPI and the radio with the settings from config.h.
// Sets g_node.lora_ok. Returns false if the SX1278 does not respond.
bool lora_begin();

// Non-blocking. Call every loop. Rate-limits transmission internally and
// drains the receive queue. Transmission is asynchronous, so this never
// stalls the loop for the ~120 ms a packet spends on air.
void lora_update();

// --- peer information, for the display and the dashboard -------------------

// Node id of the most recently heard peer, or 0 if none yet.
uint8_t  lora_peer_id();

// RSSI of the last packet from that peer, in dBm.
int16_t  lora_peer_rssi();

// Seconds since we last heard anything from it. 0xFFFF if never.
uint16_t lora_peer_age_s();

// That peer's status field (NodeStatus) as of its last packet.
uint8_t  lora_peer_status();

// Human-readable reason lora_begin() failed, for the self-test screen.
const char* lora_last_error();