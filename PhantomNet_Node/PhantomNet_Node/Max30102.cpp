// ============================================================================
//  PHANTOM NET — max30102.cpp
//  Heart rate and SpO2. Raw register driver — NO external library.
//
//  Why raw instead of the SparkFun library
//  ---------------------------------------
//  MAX30105::begin() calls Wire.begin() with no arguments and forces the bus
//  clock to whatever it was handed. On ESP32 that re-initialises the I2C
//  peripheral on the DEFAULT pins, throwing away the Wire.begin(sda, scl) the
//  sketch already did — and it does it to a bus shared with other devices.
//  Adafruit_SSD1306 does the same thing. Two libraries each believing they
//  own the bus is how you get corrupted reads that look like dead sensors.
//
//  This driver never touches the bus configuration. The sketch owns Wire;
//  this file only reads and writes registers.
//
//  How the sensor works
//  --------------------
//  Red and infrared LEDs shine into the skin; a photodiode measures what
//  comes back. Blood absorbs light, and blood volume rises and falls with
//  every heartbeat, so the reading is a large steady (DC) level with a small
//  pulsing (AC) component on top.
//
//    HEART RATE — time between AC peaks.
//    SpO2       — compare how much RED pulses vs how much INFRARED pulses.
//                 Oxygenated and deoxygenated haemoglobin absorb the two
//                 wavelengths differently, so that ratio tracks saturation.
//
//  Honest limitation: this is a reflectance sensor. On a fingertip held
//  still it is decent. On a wrist it is approximate, because the signal is
//  weaker and motion swamps it. We publish a quality figure rather than
//  pretend every reading is trustworthy.
// ============================================================================

#include "max30102.h"
#include <Wire.h>
#include <math.h>

// ---------------------------------------------------------------------------
//  REGISTERS (MAX30102 datasheet)
// ---------------------------------------------------------------------------
#define REG_INT_STATUS_1   0x00
#define REG_INT_STATUS_2   0x01
#define REG_INT_ENABLE_1   0x02
#define REG_INT_ENABLE_2   0x03
#define REG_FIFO_WR_PTR    0x04
#define REG_OVF_COUNTER    0x05
#define REG_FIFO_RD_PTR    0x06
#define REG_FIFO_DATA      0x07
#define REG_FIFO_CONFIG    0x08
#define REG_MODE_CONFIG    0x09
#define REG_SPO2_CONFIG    0x0A
#define REG_LED1_PA        0x0C     // RED
#define REG_LED2_PA        0x0D     // IR
#define REG_DIE_TEMP_INT   0x1F
#define REG_DIE_TEMP_FRAC  0x20
#define REG_DIE_TEMP_CFG   0x21
#define REG_REV_ID         0xFE
#define REG_PART_ID        0xFF

#define PART_ID_MAX30102   0x15

// --- configuration values -------------------------------------------------
// FIFO_CONFIG: SMP_AVE=1 (000<<5), ROLLOVER on (1<<4), A_FULL=15
//
// Averaging was set to 4, which quietly divided the FIFO output rate by four
// - 100 Hz sampled became 25 Hz delivered. That is only ~15 samples per
// heartbeat, too coarse to time a peak accurately. No averaging gives the
// full 100 Hz.
#define CFG_FIFO           0x1F
// MODE_CONFIG: 011 = SpO2 mode, red + IR. Mode 010 would be IR only and
// would give no oxygen reading at all.
#define CFG_MODE_SPO2      0x03
#define CFG_MODE_RESET     0x40
// SPO2_CONFIG: ADC range 4096 nA (01<<5), 100 Hz (001<<2), 411 us pulse (11)
#define CFG_SPO2           0x27
// LED drive current, 0.2 mA per step. 0x32 = ~10 mA, a good starting point
// for reflectance off a fingertip. Raise toward 0x7F if IR stays low.
#define CFG_LED_PA         0x32

// ---------------------------------------------------------------------------
//  TUNING
// ---------------------------------------------------------------------------
#define FINGER_IR_ON       30000UL   // IR above this = something on the window
#define FINGER_IR_OFF      20000UL   // hysteresis, so it does not chatter
#define BPM_MIN            40.0f
#define BPM_MAX            200.0f
#define BEAT_REFRACTORY_MS 300       // 200 bpm ceiling
#define BEAT_TIMEOUT_MS    2000      // 30 bpm floor
#define BPM_HISTORY        6
#define SPO2_WINDOW        100       // ~1 s at 100 Hz
#define MIN_PULSE_AMP      40.0f     // below this there is no real pulse

// ---------------------------------------------------------------------------
//  STATE
// ---------------------------------------------------------------------------
static bool     s_ready   = false;
static uint8_t  s_partId  = 0;
static uint32_t s_ir = 0, s_red = 0;

// --- pulse extraction ---
static bool     s_primed  = false;
static float    s_dc      = 0;      // slow baseline (the DC component)
static float    s_lpf     = 0;      // smoothed AC component
static float    s_envHi   = 0;      // decaying envelope of the AC signal
static float    s_envLo   = 0;
static bool     s_above   = false;  // currently above the beat threshold
static uint16_t s_settle  = 0;      // samples to ignore after a finger lands

// --- rate ---
static uint32_t s_lastBeatMs = 0;
static float    s_rates[BPM_HISTORY] = {0};
static uint8_t  s_rateIdx = 0, s_rateCount = 0;
static uint8_t  s_quality = 0;

// --- SpO2 accumulation ---
static uint16_t s_spN   = 0;
static uint32_t s_irSum = 0, s_redSum = 0;
static uint32_t s_irMin = 0xFFFFFFFF, s_irMax = 0;
static uint32_t s_redMin = 0xFFFFFFFF, s_redMax = 0;

// ---------------------------------------------------------------------------
//  ACCESSORS
// ---------------------------------------------------------------------------
uint32_t max30102_ir()      { return s_ir; }
uint32_t max30102_red()     { return s_red; }
uint8_t  max30102_quality() { return s_quality; }
uint8_t  max30102_part_id() { return s_partId; }

// Size of the pulsating part of the signal, in raw ADC counts. This is the
// number that decides whether a beat can be found at all: it must clear
// MIN_PULSE_AMP. Printing it turns "no reading" from a mystery into a
// measurement - too small means press more firmly or raise the LED current.
float    max30102_amp()     { return s_envHi - s_envLo; }

// ---------------------------------------------------------------------------
//  LOW-LEVEL I2C
//  Note: no Wire.begin() and no setClock() anywhere in this file.
// ---------------------------------------------------------------------------
static bool wrReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ADDR_MAX30102);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool rdBytes(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(ADDR_MAX30102);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)ADDR_MAX30102, (int)len, (int)true) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static uint8_t rdReg(uint8_t reg) {
  uint8_t v = 0;
  if (!rdBytes(reg, &v, 1)) return 0xFF;
  return v;
}

// ---------------------------------------------------------------------------
static void resetReadings() {
  s_primed = false;
  s_dc = s_lpf = s_envHi = s_envLo = 0;
  s_above = false;

  s_rateIdx = s_rateCount = s_quality = 0;
  for (uint8_t i = 0; i < BPM_HISTORY; i++) s_rates[i] = 0;

  s_spN = 0;
  s_irSum = s_redSum = 0;
  s_irMin = s_redMin = 0xFFFFFFFF;
  s_irMax = s_redMax = 0;

  g_node.hr_bpm = 0;
  g_node.spo2   = 0;
}

// ---------------------------------------------------------------------------
//  INIT
// ---------------------------------------------------------------------------
bool max30102_begin() {
  s_ready       = false;
  g_node.hr_ok  = false;
  g_node.finger = false;

  // --- is anything there at all? ---
  Wire.beginTransmission(ADDR_MAX30102);
  if (Wire.endTransmission() != 0) {
    Serial.printf("# MAX30102: no reply at 0x%02X\n", ADDR_MAX30102);
    Serial.printf("#   bus in use is SDA=GPIO%d  SCL=GPIO%d\n",
                  PIN_I2C_SDA, PIN_I2C_SCL);
    Serial.println("#   confirm the jumpers are on THOSE pins, and that the");
    Serial.println("#   module has power and a shared ground.");
    return false;
  }

  // --- is it really a MAX30102? ---
  s_partId = rdReg(REG_PART_ID);
  Serial.printf("# MAX30102: PART_ID 0x%02X (expect 0x%02X), REV 0x%02X\n",
                s_partId, PART_ID_MAX30102, rdReg(REG_REV_ID));

  if (s_partId != PART_ID_MAX30102) {
    Serial.println("# MAX30102: wrong part id.");
    if (s_partId == 0xFF || s_partId == 0x00) {
      Serial.println("#   0x00/0xFF means the read itself failed - the chip");
      Serial.println("#   ACKs its address but data is corrupt. That is a bus");
      Serial.println("#   problem: shorten the wires or lower I2C_CLOCK_HZ.");
    }
    return false;
  }

  // --- reset, then wait for the RESET bit to self-clear ---
  wrReg(REG_MODE_CONFIG, CFG_MODE_RESET);
  uint32_t until = millis() + 200;
  while (millis() < until) {
    if ((rdReg(REG_MODE_CONFIG) & CFG_MODE_RESET) == 0) break;
    delay(2);
  }

  // --- configure ---
  wrReg(REG_INT_ENABLE_1, 0x00);      // we poll; no interrupts
  wrReg(REG_INT_ENABLE_2, 0x00);
  wrReg(REG_FIFO_WR_PTR,  0x00);
  wrReg(REG_OVF_COUNTER,  0x00);
  wrReg(REG_FIFO_RD_PTR,  0x00);
  wrReg(REG_FIFO_CONFIG,  CFG_FIFO);
  wrReg(REG_MODE_CONFIG,  CFG_MODE_SPO2);
  wrReg(REG_SPO2_CONFIG,  CFG_SPO2);
  wrReg(REG_LED1_PA,      CFG_LED_PA);   // red
  wrReg(REG_LED2_PA,      CFG_LED_PA);   // infrared
  delay(20);

  // --- read the config back. Never trust a write you have not verified ---
  const uint8_t m = rdReg(REG_MODE_CONFIG);
  const uint8_t s = rdReg(REG_SPO2_CONFIG);
  if (m != CFG_MODE_SPO2 || s != CFG_SPO2) {
    Serial.printf("# MAX30102: config readback FAILED (mode 0x%02X spo2 0x%02X)\n", m, s);
    return false;
  }

  resetReadings();
  s_ready      = true;
  g_node.hr_ok = true;

  Serial.println("# MAX30102: OK  red+IR @100Hz, place a FINGERTIP on the window");
  Serial.println("#   hold still - it needs ~10 s of steady beats before it");
  Serial.println("#   publishes a rate.");
  return true;
}

// ---------------------------------------------------------------------------
//  PULSE PROCESSING
// ---------------------------------------------------------------------------
static void onSample(uint32_t ir, uint32_t red) {
  s_ir  = ir;
  s_red = red;

  // --- finger present? hysteresis stops it chattering at the boundary ---
  const bool was = g_node.finger;
  bool now = was;
  if      (!was && ir > FINGER_IR_ON)  now = true;
  else if ( was && ir < FINGER_IR_OFF) now = false;

  if (now != was) {
    g_node.finger = now;
    // Reset on BOTH transitions. A finger landing takes IR from ~1000 to
    // ~180000 in a few samples. That step is 100x bigger than any pulse, and
    // it used to blow the envelope up so far that the beat threshold sat
    // above anything a real heartbeat could reach - so no beats were ever
    // detected. Start clean and ignore the settling period.
    resetReadings();
    s_settle = 150;                            // ~1.5 s at 100 Hz
    Serial.printf("# MAX30102: finger %s (ir=%lu)\n",
                  now ? "ON" : "OFF", (unsigned long)ir);
  }
  if (!now) return;

  // --- split into DC baseline and AC pulse ---
  if (!s_primed) { s_dc = (float)ir; s_primed = true; return; }

  // 0.008 gives a ~1.2 s time constant at 100 Hz, i.e. a 0.13 Hz corner.
  // A heartbeat is 1-3 Hz, so the pulse survives and only the slow baseline
  // drift is removed. The old 0.02 was fast enough to start cancelling the
  // very signal we are trying to measure.
  s_dc += ((float)ir - s_dc) * 0.008f;
  const float ac = (float)ir - s_dc;
  s_lpf = s_lpf * 0.75f + ac * 0.25f;          // smooths sample noise

  // Let the DC filter converge before trusting anything.
  if (s_settle) { s_settle--; s_envHi = s_envLo = 0; return; }

  // --- decaying envelope, so the threshold adapts to signal strength ---
  // 0.98 per sample at 100 Hz halves the envelope in ~0.35 s, so a one-off
  // transient is forgotten quickly instead of poisoning the threshold.
  s_envHi *= 0.98f;
  s_envLo *= 0.98f;
  if (s_lpf > s_envHi) s_envHi = s_lpf;
  if (s_lpf < s_envLo) s_envLo = s_lpf;

  const float amp = s_envHi - s_envLo;
  if (amp < MIN_PULSE_AMP) return;             // nothing pulsing yet

  const float thr  = s_envLo + amp * 0.55f;
  const float hyst = amp * 0.10f;

  // --- beat = rising crossing of the threshold ---
  if (!s_above && s_lpf > thr) {
    s_above = true;
    const uint32_t t  = millis();
    const uint32_t dt = t - s_lastBeatMs;
    s_lastBeatMs = t;

    if (dt >= BEAT_REFRACTORY_MS && dt <= BEAT_TIMEOUT_MS) {
      const float bpm = 60000.0f / (float)dt;
      if (bpm >= BPM_MIN && bpm <= BPM_MAX) {
        s_rates[s_rateIdx] = bpm;
        s_rateIdx = (s_rateIdx + 1) % BPM_HISTORY;
        if (s_rateCount < BPM_HISTORY) s_rateCount++;

        float sum = 0;
        for (uint8_t i = 0; i < s_rateCount; i++) sum += s_rates[i];
        const float avg = sum / s_rateCount;
        g_node.hr_bpm = (uint8_t)(avg + 0.5f);

        // Quality: how tightly the recent beats agree. A still finger gives
        // near-identical intervals; motion scatters them.
        float var = 0;
        for (uint8_t i = 0; i < s_rateCount; i++) {
          const float d = s_rates[i] - avg;
          var += d * d;
        }
        float q = 100.0f - sqrtf(var / s_rateCount) * 8.0f;
        if (q < 0) q = 0;
        if (q > 100) q = 100;
        if (s_rateCount < 3) q = q * s_rateCount / 3.0f;
        s_quality = (uint8_t)q;
      }
    }
  } else if (s_above && s_lpf < (thr - hyst)) {
    s_above = false;
  }

  // --- SpO2: ratio of ratios over a one-second window ---
  s_irSum  += ir;   s_redSum += red;
  if (ir  < s_irMin)  s_irMin  = ir;
  if (ir  > s_irMax)  s_irMax  = ir;
  if (red < s_redMin) s_redMin = red;
  if (red > s_redMax) s_redMax = red;

  if (++s_spN >= SPO2_WINDOW) {
    const float irDC  = (float)s_irSum  / s_spN;
    const float redDC = (float)s_redSum / s_spN;
    const float irAC  = (float)(s_irMax  - s_irMin);
    const float redAC = (float)(s_redMax - s_redMin);

    if (irDC > 1000.0f && redDC > 1000.0f && irAC > 50.0f) {
      const float r = (redAC / redDC) / (irAC / irDC);
      float spo2 = 104.0f - 17.0f * r;          // standard empirical curve
      if (spo2 > 100.0f) spo2 = 100.0f;
      if (spo2 <  70.0f) spo2 =  70.0f;
      // Only publish when the pulse itself is believable.
      g_node.spo2 = (s_quality >= 40) ? (uint8_t)(spo2 + 0.5f) : 0;
    } else {
      g_node.spo2 = 0;
    }

    s_spN = 0;
    s_irSum = s_redSum = 0;
    s_irMin = s_redMin = 0xFFFFFFFF;
    s_irMax = s_redMax = 0;
  }
}

// ---------------------------------------------------------------------------
//  UPDATE
// ---------------------------------------------------------------------------
void max30102_update() {
  if (!s_ready) return;

  static uint32_t nextMs = 0;
  if ((int32_t)(millis() - nextMs) < 0) return;
  nextMs = millis() + HR_SAMPLE_MS;

  // How many samples are waiting? The pointers are 5-bit and wrap at 32.
  const uint8_t wr = rdReg(REG_FIFO_WR_PTR);
  const uint8_t rd = rdReg(REG_FIFO_RD_PTR);
  if (wr == 0xFF || rd == 0xFF) return;

  uint8_t n = (uint8_t)((wr - rd) & 0x1F);
  if (n == 0) return;
  if (n > 8) n = 8;                 // 8 x 6 = 48 bytes, well inside the
                                    // Wire buffer, and keeps this call short

  uint8_t buf[48];
  if (!rdBytes(REG_FIFO_DATA, buf, n * 6)) return;

  for (uint8_t i = 0; i < n; i++) {
    const uint8_t* p = &buf[i * 6];
    // 18-bit samples, left-aligned in three bytes. RED first, then IR.
    const uint32_t red = (((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]) & 0x03FFFF;
    const uint32_t ir  = (((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 8) | p[5]) & 0x03FFFF;
    onSample(ir, red);
  }

  // Beats stop the instant a finger lifts. Age out a stale rate rather than
  // leaving the last value frozen on the dashboard.
  if (g_node.finger && g_node.hr_bpm &&
      (millis() - s_lastBeatMs > BEAT_TIMEOUT_MS * 2)) {
    resetReadings();
  }
}