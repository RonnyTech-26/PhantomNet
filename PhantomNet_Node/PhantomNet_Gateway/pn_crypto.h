// ============================================================================
//  PHANTOM NET — pn_crypto.h
//  Encrypted telemetry frame: AES-128-GCM, plus replay rejection.
//
//  THIS FILE DEFINES THE OVER-THE-AIR FORMAT. The node and the gateway must
//  both use an identical copy. Change one, change the other, or every packet
//  fails authentication.
//
//  FRAME LAYOUT (39 bytes)
//  -----------------------
//    byte  0     magic     0xA7
//    byte  1     version   0x02        (0x01 was the plaintext format)
//    byte  2     src       originating node id
//    byte  3     dst       1 = gateway, 0xFF = broadcast
//    byte  4     type      0 = DATA
//    byte  5     flags     bit0 ALARM, bit1 RELAY_REQ, bit2 LOW_BATT
//    byte  6-7   boot      power-cycle counter, from flash (little-endian)
//    byte  8-9   msg       per-boot message counter (little-endian)
//    byte  10    plen      plaintext length
//   ---------- the 11 bytes above are AUTHENTICATED, NOT ENCRYPTED ----------
//    byte  11    ttl       hops remaining      <- MUTABLE, not authenticated
//    byte  12    hops      hops taken so far   <- MUTABLE, not authenticated
//    byte  13..  ciphertext[plen]
//    last  8     GCM authentication tag (truncated from 16)
//
//  WHY THE HEADER IS READABLE
//  --------------------------
//  A relay has to see src, dst and ttl to route a packet. If those were
//  encrypted, every relay would need the key and would have to decrypt and
//  re-encrypt - which destroys end-to-end secrecy. Instead they travel in
//  the clear but are fed to GCM as Additional Authenticated Data, so they
//  are READABLE but NOT FORGEABLE. You cannot take node 2's packet and
//  relabel it as node 3 without the tag failing.
//
//  WHY ttl AND hops SIT OUTSIDE THE AUTHENTICATED REGION
//  -----------------------------------------------------
//  A relay must decrement ttl and increment hops. If those bytes were
//  covered by the tag, the relay would have to re-encrypt - which again
//  needs the key. Leaving them mutable means an attacker can meddle with
//  hop counts but not with any actual data. That is the right trade.
// ============================================================================
#pragma once

#include <Arduino.h>

#define PN_MAGIC        0xA7
#define PN_VERSION      0x02
#define PN_TYPE_DATA    0x00
#define PN_DST_GATEWAY  0x01
#define PN_DST_BCAST    0xFF

#define PN_FLAG_ALARM      0x01
#define PN_FLAG_RELAY_REQ  0x02
#define PN_FLAG_LOW_BATT   0x04

#define PN_HDR_LEN      11
#define PN_MUT_LEN      2
#define PN_PLAIN_LEN    18
#define PN_TAG_LEN      8
#define PN_FRAME_LEN    (PN_HDR_LEN + PN_MUT_LEN + PN_PLAIN_LEN + PN_TAG_LEN)  // 39
#define PN_NONCE_LEN    12
#define PN_DEFAULT_TTL  3

// --- authenticated header -------------------------------------------------
typedef struct __attribute__((packed)) {
  uint8_t  magic;
  uint8_t  version;
  uint8_t  src;
  uint8_t  dst;
  uint8_t  type;
  uint8_t  flags;
  uint16_t boot;
  uint16_t msg;
  uint8_t  plen;
} pn_hdr_t;

// --- encrypted payload ----------------------------------------------------
typedef struct __attribute__((packed)) {
  uint8_t  status;
  uint8_t  hr_bpm;
  uint8_t  spo2;
  uint8_t  batt_pct;
  uint16_t amag_mg;      // |a| in milli-g
  uint8_t  sats;
  uint8_t  gps_flags;    // bit0 = fix
  float    lat;
  float    lon;
  uint16_t uptime_s;
} pn_payload_t;

static_assert(sizeof(pn_hdr_t)     == PN_HDR_LEN,   "header layout changed");
static_assert(sizeof(pn_payload_t) == PN_PLAIN_LEN, "payload layout changed");

// ---------------------------------------------------------------------------
//  API
// ---------------------------------------------------------------------------

// Loads the boot counter from flash and increments it. Call once in setup().
// The boot counter is what stops a packet captured yesterday from being
// replayed today: the message counter restarts at 1 every power cycle, so
// without it a replayed packet would look perfectly fresh.
void     pn_crypto_begin();
uint16_t pn_boot_count();

// Build a complete encrypted frame. buf must be at least PN_FRAME_LEN.
// Returns the number of bytes written, or 0 on failure.
uint8_t  pn_frame_build(uint8_t* buf, uint8_t src, uint8_t dst, uint8_t flags,
                        uint16_t msg, uint8_t ttl, const pn_payload_t* pl);

// Authenticate and decrypt a received frame. Returns false if the frame is
// the wrong size, the wrong version, or the tag does not verify - which
// means it was corrupted or forged. There is no partial success.
bool     pn_frame_open(const uint8_t* buf, int len, pn_hdr_t* hdr,
                       pn_payload_t* pl, uint8_t* ttl, uint8_t* hops);

// Replay rejection. Returns true if this (boot, msg) pair is NEWER than
// anything already seen from that node. A captured packet retransmitted
// later returns false and must be discarded.
bool     pn_replay_ok(uint8_t src, uint16_t boot, uint16_t msg);
uint32_t pn_replay_rejected();
void     pn_replay_clear();

// Hex dump, for showing an examiner that the payload really is ciphertext.
void     pn_hexdump(const char* label, const uint8_t* b, size_t n);