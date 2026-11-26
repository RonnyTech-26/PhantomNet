// ============================================================================
//  PHANTOM NET — fall.cpp
//  Fall detection.
//
//  A fall is not one threshold. It is a STORY WITH FOUR CHAPTERS, and all
//  four must happen, in order, inside a time window:
//
//    1. NORMAL      |a| around 1 g - walking about
//    2. FREE-FALL   |a| collapses below ~0.5 g for 30 ms or more
//    3. IMPACT      |a| spikes above ~2.5 g
//    4. MOTIONLESS  |a| back near 1 g, gyro quiet, for 2 s
//                   AND the device is now at a different angle than before
//
//  Chapter 4 is what makes it trustworthy. Anyone who falls and gets
//  straight back up is fine. Someone who falls and STAYS DOWN is the actual
//  emergency. The orientation change catches the difference between a
//  person who ended up horizontal and a device that was set on a table.
//
//  Requiring the whole sequence is what rejects the things that would
//  otherwise cause constant false alarms:
//    - sitting down heavily          no free-fall phase
//    - putting the device on a desk  no orientation change of a person
//    - jogging, stairs, clapping     never goes motionless afterwards
//
//  THRESHOLDS ARE MEASURED, NOT COPIED
//  -----------------------------------
//  The defaults in config.h come from drop tests on this exact hardware:
//  a clean impact peaked at 7.9 g, pre-impact free-fall bottomed at 0.38 g,
//  and a real topple produced 33 degrees of orientation change. The
//  thresholds sit well inside those numbers so marginal falls still trip.
// ============================================================================

#include "fall.h"
#include <math.h>

// ---------------------------------------------------------------------------
//  STATE
// ---------------------------------------------------------------------------
static FallState s_state = FALL_IDLE;

static uint32_t s_ffStartMs   = 0;    // when weightlessness began
static uint32_t s_ffMs        = 0;    // how long it lasted
static uint32_t s_impactMs    = 0;    // when the impact landed
static uint32_t s_stillSinceMs = 0;   // when the current stillness began
static uint32_t s_confirmedMs = 0;    // when MAN DOWN latched

// Orientation before the fall: a slowly-updated average of the gravity
// vector, maintained only while idle so a fall cannot contaminate it.
static float s_refX = 0, s_refY = 0, s_refZ = 1;
static bool  s_refValid = false;

// Orientation after: averaged during the stillness window.
static float s_postX = 0, s_postY = 0, s_postZ = 0;
static uint16_t s_postN = 0;

// Event details
static float    s_peakG = 0;
static float    s_minG  = 9.99f;
static uint16_t s_lastFfMs = 0;
static float    s_tiltDeg  = 0;

// ---------------------------------------------------------------------------
FallState fall_state()            { return s_state; }
float     fall_last_peak_g()      { return s_peakG; }
float     fall_last_min_g()       { return s_minG; }
uint16_t  fall_last_freefall_ms() { return s_lastFfMs; }
float     fall_last_tilt_deg()    { return s_tiltDeg; }

const char* fall_state_name() {
  switch (s_state) {
    case FALL_IDLE:      return "idle";
    case FALL_FREEFALL:  return "FREE-FALL";
    case FALL_SETTLING:  return "settling";
    case FALL_CONFIRMED: return "MAN DOWN";
    default:             return "?";
  }
}

// ---------------------------------------------------------------------------
static float angleBetween(float ax, float ay, float az,
                          float bx, float by, float bz) {
  const float na = sqrtf(ax*ax + ay*ay + az*az);
  const float nb = sqrtf(bx*bx + by*by + bz*bz);
  if (na < 0.1f || nb < 0.1f) return 0.0f;

  float d = (ax*bx + ay*by + az*bz) / (na * nb);
  if (d >  1.0f) d =  1.0f;
  if (d < -1.0f) d = -1.0f;
  return acosf(d) * 57.29578f;
}

// ---------------------------------------------------------------------------
void fall_begin() {
  s_state    = FALL_IDLE;
  s_refValid = false;
  s_peakG    = 0;
  s_minG     = 9.99f;
  s_lastFfMs = 0;
  s_tiltDeg  = 0;

  Serial.printf("# FALL: armed  free-fall<%.2fg for >%ums, impact>%.2fg, "
                "still %ums, tilt>%.0f deg\n",
                FALL_FREEFALL_G, (unsigned)FALL_FREEFALL_MIN_MS,
                FALL_IMPACT_G, (unsigned)FALL_STILL_MS, FALL_TILT_DEG);
}

void fall_reset() {
  if (s_state == FALL_CONFIRMED) Serial.println("# FALL: alarm cleared");
  s_state    = FALL_IDLE;
  s_refValid = false;
  if (g_node.status == STATUS_MANDOWN) g_node.status = STATUS_NORMAL;
}

// ---------------------------------------------------------------------------
void fall_update() {
  if (!g_node.imu_ok) return;

  const uint32_t now  = millis();
  const float    a    = g_node.amag;
  const float    gmag = sqrtf(g_node.gx * g_node.gx +
                              g_node.gy * g_node.gy +
                              g_node.gz * g_node.gz);
  const bool still = (fabsf(a - 1.0f) < FALL_STILL_TOL_G) &&
                     (gmag < FALL_STILL_GYRO_DPS);

  switch (s_state) {

    // -----------------------------------------------------------------
    case FALL_IDLE: {
      // Keep a slow running reference of which way is down. Only updated
      // while still, so a fall in progress cannot poison it.
      if (still) {
        if (!s_refValid) {
          s_refX = g_node.ax; s_refY = g_node.ay; s_refZ = g_node.az;
          s_refValid = true;
        } else {
          s_refX += (g_node.ax - s_refX) * 0.02f;
          s_refY += (g_node.ay - s_refY) * 0.02f;
          s_refZ += (g_node.az - s_refZ) * 0.02f;
        }
      }

      // CHAPTER 2: weightlessness.
      if (a < FALL_FREEFALL_G) {
        s_state     = FALL_FREEFALL;
        s_ffStartMs = now;
        s_ffMs      = 0;
        s_peakG     = 0;
        s_minG      = a;
        Serial.printf("# FALL: free-fall started (|a|=%.2fg)\n", a);
      }
      break;
    }

    // -----------------------------------------------------------------
    case FALL_FREEFALL: {
      if (a < s_minG) s_minG = a;
      if (a < FALL_FREEFALL_G) s_ffMs = now - s_ffStartMs;

      // CHAPTER 3: impact.
      if (a > FALL_IMPACT_G) {
        s_lastFfMs = (uint16_t)s_ffMs;

        if (s_ffMs < FALL_FREEFALL_MIN_MS) {
          // Impact with no real weightless phase before it - a knock, a
          // slam, setting the device down hard. Not a fall.
          Serial.printf("# FALL: rejected - impact %.2fg but only %ums "
                        "of free-fall\n", a, (unsigned)s_ffMs);
          s_state = FALL_IDLE;
          break;
        }

        s_peakG        = a;
        s_impactMs     = now;
        s_stillSinceMs = now;
        s_postX = s_postY = s_postZ = 0;
        s_postN = 0;
        s_state = FALL_SETTLING;
        Serial.printf("# FALL: IMPACT %.2fg after %ums free-fall (min %.2fg)\n",
                      s_peakG, (unsigned)s_ffMs, s_minG);
        break;
      }

      // Nothing hit anything in time. Probably a fast arm movement.
      if (now - s_ffStartMs > FALL_WINDOW_MS) {
        Serial.println("# FALL: free-fall ended with no impact - ignoring");
        s_state = FALL_IDLE;
      }
      break;
    }

    // -----------------------------------------------------------------
    case FALL_SETTLING: {
      if (a > s_peakG) s_peakG = a;      // catch a bigger secondary impact

      if (!still) {
        // They are still moving. Restart the clock - someone who gets up
        // is not a confirmed fall.
        s_stillSinceMs = now;
        s_postX = s_postY = s_postZ = 0;
        s_postN = 0;

        // Moving for a long time after the impact means they recovered.
        if (now - s_impactMs > FALL_STILL_MS * 3) {
          Serial.println("# FALL: movement after impact - they got up, ignoring");
          s_state = FALL_IDLE;
        }
        break;
      }

      // Accumulate the resting orientation while they stay still.
      s_postX += g_node.ax; s_postY += g_node.ay; s_postZ += g_node.az;
      s_postN++;

      // CHAPTER 4: motionless for long enough.
      if (now - s_stillSinceMs >= FALL_STILL_MS && s_postN > 10) {
        const float px = s_postX / s_postN;
        const float py = s_postY / s_postN;
        const float pz = s_postZ / s_postN;

        s_tiltDeg = s_refValid
                  ? angleBetween(s_refX, s_refY, s_refZ, px, py, pz)
                  : 0.0f;

        if (!s_refValid) {
          Serial.println("# FALL: no pre-fall reference - accepting on impact alone");
        }

        if (!s_refValid || s_tiltDeg >= FALL_TILT_DEG) {
          s_state       = FALL_CONFIRMED;
          s_confirmedMs = now;
          if (g_node.status != STATUS_SOS) g_node.status = STATUS_MANDOWN;

          Serial.println("# ============================================");
          Serial.printf ("# FALL CONFIRMED - MAN DOWN\n");
          Serial.printf ("#   impact      %.2f g\n", s_peakG);
          Serial.printf ("#   free-fall   %.2f g for %u ms\n", s_minG, s_lastFfMs);
          Serial.printf ("#   orientation changed %.0f deg\n", s_tiltDeg);
          Serial.println("# ============================================");
        } else {
          Serial.printf("# FALL: rejected - still, but only %.0f deg of "
                        "orientation change (need %.0f)\n",
                        s_tiltDeg, FALL_TILT_DEG);
          s_state = FALL_IDLE;
        }
      }
      break;
    }

    // -----------------------------------------------------------------
    case FALL_CONFIRMED: {
      // Latch. A real deployment would hold this until a carer
      // acknowledges; for demo and test purposes it self-clears so the
      // sequence can be repeated without a reboot.
      if (now - s_confirmedMs > FALL_LATCH_MS) fall_reset();
      break;
    }
  }
}