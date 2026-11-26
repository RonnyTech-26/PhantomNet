// ============================================================================
//  PHANTOM NET — gps.cpp
//  NEO-6M driver. Raw NMEA parsing, no external library.
//
//  WIRING — the classic mistake here is not crossing the lines.
//      GPS TX  -> ESP32 GPIO16   (the ESP32 RECEIVES here)
//      GPS RX  -> ESP32 GPIO17   (only needed to configure the module)
//      GPS VCC -> 5V             (the module has its own LDO)
//      GPS GND -> GND            (shared)
//  TX-to-TX gets you silence and looks identical to a dead module.
//
//  WHY AUTO-BAUD
//  -------------
//  The module we were shipped came configured for UBX binary at 57600, not
//  the NEO-6M factory default of NMEA at 9600 - a common vendor setting for
//  drone flight controllers. Hard-coding 9600 produced "bytes arriving but
//  no valid sentences", which is a maddening symptom because the wiring is
//  provably fine. So this driver listens at each plausible speed, counts
//  checksum-valid sentences, and picks the winner. It costs about a second
//  at boot and removes the entire class of problem.
//
//  If yours still speaks UBX, run PhantomNet_GPS_Fix.ino once. It switches
//  the module to NMEA at 9600 and saves that to flash.
//
//  WHAT WE PARSE
//  -------------
//  GGA - fix quality, satellite count, position, HDOP.  This is the one
//        that matters.
//  RMC - a validity flag and position, used as a fallback when GGA has not
//        arrived yet.
//  Everything else is ignored.
// ============================================================================

#include "gps.h"
#include <math.h>

// ---------------------------------------------------------------------------
//  TEMPORARY DIAGNOSTIC — set to 1 to dump every raw NMEA sentence to Serial.
//  This shows GSV sentences (satellites IN VIEW, before any fix), which the
//  parser below normally discards since only GGA/RMC are used for position.
//  A module with a working antenna in open sky reports GSV counts within
//  seconds, long before GGA ever shows a fix. If GSV also stays empty for
//  minutes outdoors, the fault is the antenna/RF path, not firmware timing.
//  Set back to 0 once diagnosed - this is verbose and not for normal use.
// ---------------------------------------------------------------------------
#define GPS_DEBUG_DUMP_RAW  1

// ---------------------------------------------------------------------------
//  CONFIG
// ---------------------------------------------------------------------------
#define NMEA_MAX          100        // longest sentence we will buffer
#define GPS_STALE_MS      5000UL     // no sentence for this long -> link down
#define PROBE_MS          1200UL     // listen time per candidate baud

static const uint32_t CANDIDATE_BAUD[] = { 9600, 57600, 38400, 115200, 19200, 4800 };
static const uint8_t  N_CANDIDATES = sizeof(CANDIDATE_BAUD) / sizeof(CANDIDATE_BAUD[0]);

// ---------------------------------------------------------------------------
//  STATE
// ---------------------------------------------------------------------------
static bool     s_ready     = false;
static uint32_t s_baud      = 0;
static char     s_buf[NMEA_MAX];
static uint8_t  s_len       = 0;
static bool     s_inSentence = false;

static uint32_t s_sentences = 0;
static uint32_t s_errors    = 0;
static uint32_t s_lastMs    = 0;
static bool     s_everValid = false;
static float    s_hdop      = 99.0f;

// ---------------------------------------------------------------------------
uint32_t gps_baud()      { return s_baud; }
uint32_t gps_sentences() { return s_sentences; }
uint32_t gps_errors()    { return s_errors; }
float    gps_hdop()      { return s_hdop; }

uint16_t gps_age_s() {
  if (!s_everValid) return 0xFFFF;
  return (uint16_t)((millis() - s_lastMs) / 1000);
}

// ---------------------------------------------------------------------------
//  NMEA helpers
// ---------------------------------------------------------------------------

// A sentence is "$....*HH" where HH is the XOR of everything between.
// Checking it is what separates a real fix from noise that happens to
// look like a number.
static bool checksumOk(const char* s, uint8_t len) {
  if (len < 5 || s[0] != '$') return false;

  int star = -1;
  for (int i = len - 1; i > 0; i--) {
    if (s[i] == '*') { star = i; break; }
  }
  if (star < 1 || star + 2 >= len) return false;

  uint8_t sum = 0;
  for (int i = 1; i < star; i++) sum ^= (uint8_t)s[i];

  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  const int hi = hex(s[star + 1]);
  const int lo = hex(s[star + 2]);
  if (hi < 0 || lo < 0) return false;

  return sum == (uint8_t)((hi << 4) | lo);
}

// Copy comma-separated field n into out. Returns false if it is empty.
static bool field(const char* s, uint8_t len, uint8_t n, char* out, uint8_t outSz) {
  uint8_t f = 0, o = 0;
  out[0] = 0;
  for (uint8_t i = 0; i < len; i++) {
    const char c = s[i];
    if (c == ',' || c == '*') {
      if (f == n) { out[o] = 0; return o > 0; }
      f++; o = 0;
      if (c == '*') break;
      continue;
    }
    if (f == n && o < outSz - 1) out[o++] = c;
  }
  if (f == n) { out[o] = 0; return o > 0; }
  return false;
}

// NMEA gives latitude as ddmm.mmmm and longitude as dddmm.mmmm - degrees and
// minutes glued together, NOT decimal degrees. Feeding the raw number into a
// map puts you in the wrong country.
static double toDegrees(const char* v, char hemi) {
  const double raw = atof(v);
  const int    deg = (int)(raw / 100.0);
  const double min = raw - (deg * 100.0);
  double out = deg + min / 60.0;
  if (hemi == 'S' || hemi == 'W') out = -out;
  return out;
}

// ---------------------------------------------------------------------------
//  Sentence handling
// ---------------------------------------------------------------------------
static void parseGGA(const char* s, uint8_t len) {
  char q[8];
  if (!field(s, len, 6, q, sizeof(q))) return;
  const int quality = atoi(q);            // 0 = no fix, 1 = GPS, 2 = DGPS

  char sats[8];
  if (field(s, len, 7, sats, sizeof(sats))) g_node.sats = (uint8_t)atoi(sats);

  char hd[10];
  if (field(s, len, 8, hd, sizeof(hd))) s_hdop = atof(hd);

  const bool hadFix = g_node.gps_fix;
  g_node.gps_fix = (quality > 0);

  if (g_node.gps_fix) {
    char la[16], ns[4], lo[16], ew[4];
    if (field(s, len, 2, la, sizeof(la)) && field(s, len, 3, ns, sizeof(ns)) &&
        field(s, len, 4, lo, sizeof(lo)) && field(s, len, 5, ew, sizeof(ew))) {
      g_node.lat = toDegrees(la, ns[0]);
      g_node.lon = toDegrees(lo, ew[0]);
    }
  }

  if (g_node.gps_fix != hadFix) {
    if (g_node.gps_fix) {
      Serial.printf("# GPS: FIX acquired  %u sats  hdop %.1f  %.6f, %.6f\n",
                    (unsigned)g_node.sats, s_hdop, g_node.lat, g_node.lon);
    } else {
      Serial.println("# GPS: fix LOST");
    }
  }
}

// GSV reports satellites IN VIEW - it has nothing to do with whether a fix
// exists yet. Field 3 of the FIRST GSV sentence in each group is the total
// count. Several GSV sentences can arrive per update (4 satellites each);
// only sentence #1 carries the total, so that is the only one read here.
static uint8_t s_satsInView = 0;

static void parseGSV(const char* s, uint8_t len) {
  char seq[4];
  if (!field(s, len, 1, seq, sizeof(seq))) return;   // sentence # within group
  if (atoi(seq) != 1) return;                        // only #1 has the total

  char tot[4];
  if (field(s, len, 3, tot, sizeof(tot))) {
    s_satsInView = (uint8_t)atoi(tot);
  }
}

static void parseRMC(const char* s, uint8_t len) {
  char st[4];
  if (!field(s, len, 2, st, sizeof(st))) return;
  if (st[0] != 'A') return;               // V = data not valid

  // Only used before the first GGA arrives; GGA is authoritative.
  if (g_node.gps_fix) return;

  char la[16], ns[4], lo[16], ew[4];
  if (field(s, len, 3, la, sizeof(la)) && field(s, len, 4, ns, sizeof(ns)) &&
      field(s, len, 5, lo, sizeof(lo)) && field(s, len, 6, ew, sizeof(ew))) {
    g_node.lat = toDegrees(la, ns[0]);
    g_node.lon = toDegrees(lo, ew[0]);
    g_node.gps_fix = true;
  }
}

// Returns true if the sentence was well-formed, whether or not we used it.
static bool handleSentence(const char* s, uint8_t len) {
#if GPS_DEBUG_DUMP_RAW
  { char tmp[NMEA_MAX + 1]; memcpy(tmp, s, len); tmp[len] = 0;
    Serial.printf("# GPS RAW: %s\n", tmp); }
#endif

  if (!checksumOk(s, len)) { s_errors++; return false; }

  s_sentences++;
  s_lastMs    = millis();
  s_everValid = true;

  // Talker id varies - GP (GPS), GN (multi-constellation), GL (GLONASS).
  // Match on the last three letters of the type instead of the whole thing.
  if (len < 6) return true;
  const char* t = &s[3];
  if      (!strncmp(t, "GGA", 3)) parseGGA(s, len);
  else if (!strncmp(t, "RMC", 3)) parseRMC(s, len);
  else if (!strncmp(t, "GSV", 3)) parseGSV(s, len);
  return true;
}

// Total satellites currently in view, from the last GSV group - regardless
// of fix status. This is the number that actually diagnoses the antenna/RF
// path: it should be nonzero within seconds of a working module seeing open
// sky, whether or not a fix has been computed yet.
uint8_t gps_sats_in_view() { return s_satsInView; }

// Feed one character in. Returns true when a complete sentence was handled.
static bool feed(char c) {
  if (c == '$') { s_inSentence = true; s_len = 0; }
  if (!s_inSentence) return false;

  if (c == '\r' || c == '\n') {
    const bool ok = (s_len > 0) ? handleSentence(s_buf, s_len) : false;
    s_inSentence = false;
    s_len = 0;
    return ok;
  }

  if (s_len < NMEA_MAX - 1) s_buf[s_len++] = c;
  else { s_inSentence = false; s_len = 0; }   // overlong: drop it
  return false;
}

// ---------------------------------------------------------------------------
//  INIT — find the baud rate rather than assuming one
// ---------------------------------------------------------------------------
bool gps_begin() {
  s_ready      = false;
  g_node.gps_ok = false;
  g_node.gps_fix = false;
  s_baud = 0;

  Serial.println("# GPS: probing for baud rate...");

  uint32_t bestBaud = 0, bestScore = 0;

  for (uint8_t i = 0; i < N_CANDIDATES; i++) {
    const uint32_t baud = CANDIDATE_BAUD[i];

    Serial2.begin(baud, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    delay(80);
    while (Serial2.available()) Serial2.read();   // flush stale bytes

    s_sentences = 0; s_errors = 0;
    s_inSentence = false; s_len = 0;

    uint32_t bytes = 0;
    const uint32_t until = millis() + PROBE_MS;
    while (millis() < until) {
      while (Serial2.available()) { bytes++; feed((char)Serial2.read()); }
    }

    Serial.printf("#   %6lu baud : %4lu bytes, %2lu valid, %2lu bad checksum\n",
                  (unsigned long)baud, (unsigned long)bytes,
                  (unsigned long)s_sentences, (unsigned long)s_errors);

    if (s_sentences > bestScore) { bestScore = s_sentences; bestBaud = baud; }
    if (s_sentences >= 5) break;          // good enough, stop probing
  }

  if (bestBaud == 0 || bestScore == 0) {
    Serial.println("# GPS: no valid NMEA at any baud rate.");
    Serial.printf ("#   wiring: GPS TX -> GPIO%d,  GPS RX -> GPIO%d\n",
                   PIN_GPS_RX, PIN_GPS_TX);
    Serial.println("#   CROSS them - TX to TX is the usual mistake.");
    Serial.println("#   If bytes arrived but none were valid, the module is");
    Serial.println("#   speaking UBX binary. Run PhantomNet_GPS_Fix.ino once.");
    Serial2.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    return false;
  }

  Serial2.begin(bestBaud, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  s_baud        = bestBaud;
  s_ready       = true;
  g_node.gps_ok = true;
  s_sentences = 0; s_errors = 0;

  Serial.printf("# GPS: OK at %lu baud, NMEA\n", (unsigned long)bestBaud);
  Serial.println("#   A cold start needs a clear view of the sky and can take");
  Serial.println("#   5-15 minutes for the first fix. The module's LED blinks");
  Serial.println("#   once per second ONLY after it has one.");
  return true;
}

// ---------------------------------------------------------------------------
//  UPDATE
// ---------------------------------------------------------------------------
void gps_update() {
  if (!s_ready) return;

  // Bounded per call: a burst of sentences must not stall the main loop and
  // hold up the radio.
  uint16_t guard = 0;
  while (Serial2.available() && guard++ < 400) feed((char)Serial2.read());

  // If the module stops talking entirely, say so rather than leaving a stale
  // position on the dashboard as though it were current.
  if (s_everValid && (millis() - s_lastMs > GPS_STALE_MS)) {
    if (g_node.gps_ok) {
      Serial.println("# GPS: UART has gone quiet - check the TX wire");
      g_node.gps_ok  = false;
      g_node.gps_fix = false;
      g_node.sats    = 0;
    }
  } else if (s_everValid) {
    g_node.gps_ok = true;
  }
}