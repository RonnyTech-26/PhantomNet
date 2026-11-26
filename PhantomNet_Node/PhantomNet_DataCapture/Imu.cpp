// ============================================================================
//  PHANTOM NET — imu.cpp
//  Raw register driver for MPU6050 / MPU6500 / MPU9250.
//
//  Why raw instead of a library
//  ---------------------------
//  The modules we were shipped are labelled MPU6050 but read back
//  WHO_AM_I = 0x70, which is an MPU6500 die. Adafruit's library checks for
//  0x68 and refuses anything else - one byte, and it will not run. The
//  register maps are compatible for everything this project needs, so we
//  detect the part at boot and configure whichever is present.
//
//  It also means this file never calls Wire.begin() or Wire.setClock().
//  The sketch owns the bus; drivers only read and write registers.
//
//  TWO RANGE SETTINGS THAT MATTER
//  ------------------------------
//  ACCEL +/-16 g. At the +/-2 g default an impact CLIPS, and a clipped
//  impact reads SMALLER than it really was - so the fall detector quietly
//  under-reads exactly when it matters. This is the single most common
//  invisible bug in DIY fall detectors.
//
//  GYRO +/-2000 dps. We measured 1544 dps during a real drop test. At the
//  +/-500 dps setting that would have clipped to 500 and reported a third
//  of the truth.
// ============================================================================

#include "imu.h"
#include <Wire.h>
#include <math.h>

// ---------------------------------------------------------------------------
//  REGISTERS
// ---------------------------------------------------------------------------
#define REG_SMPLRT_DIV     0x19
#define REG_CONFIG         0x1A
#define REG_GYRO_CONFIG    0x1B
#define REG_ACCEL_CONFIG   0x1C
#define REG_ACCEL_CONFIG2  0x1D     // MPU6500 family only
#define REG_ACCEL_XOUT_H   0x3B
#define REG_PWR_MGMT_1     0x6B
#define REG_WHO_AM_I       0x75

#define ACCEL_FS_SEL       0x18     // bits 4:3 = 11 -> +/-16 g
#define GYRO_FS_SEL        0x18     // bits 4:3 = 11 -> +/-2000 dps

static const float ACCEL_SCALE = 1.0f / 2048.0f;   // g per LSB at +/-16 g
static const float GYRO_SCALE  = 1.0f / 16.4f;     // dps per LSB at +/-2000

// ---------------------------------------------------------------------------
//  STATE
// ---------------------------------------------------------------------------
static uint8_t  s_addr    = 0;
static uint8_t  s_who     = 0;
static bool     s_is6500  = false;
static bool     s_ready   = false;
static bool     s_hasData = false;

static float    s_gxOff = 0, s_gyOff = 0, s_gzOff = 0;
static uint16_t s_peakMg = 0;

// ---------------------------------------------------------------------------
//  I2C HELPERS  (no bus configuration anywhere in this file)
// ---------------------------------------------------------------------------
static bool rdBytes(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)s_addr, (int)len, (int)true) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static uint8_t rdReg(uint8_t reg) {
  uint8_t v = 0;
  if (!rdBytes(reg, &v, 1)) return 0xFF;
  return v;
}

static bool wrReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool present(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// ---------------------------------------------------------------------------
const char* imu_part_name() {
  switch (s_who) {
    case 0x68: return "MPU6050";
    case 0x70: return "MPU6500";
    case 0x71: return "MPU9250";
    case 0x73: return "MPU9255";
    case 0x74: return "MPU6515";
    case 0x7C: return "MPU6555";
    default:   return "unknown";
  }
}

uint8_t imu_who_am_i() { return s_who; }
bool    imu_has_data() { return s_hasData; }

uint16_t imu_peak_mg() {
  const uint16_t p = s_peakMg;
  s_peakMg = 0;
  return p;
}

// ---------------------------------------------------------------------------
//  GYRO ZERO-RATE CALIBRATION
//
//  Every MPU has a small constant bias. Left uncorrected it looks like the
//  node is permanently rotating, which trips the "is it still?" test in the
//  fall detector and stops an alarm from ever confirming.
// ---------------------------------------------------------------------------
void imu_calibrate_gyro(uint16_t samples) {
  if (!s_ready) return;

  Serial.printf("# IMU: calibrating gyro (%u samples) - keep the board STILL\n", samples);

  double sx = 0, sy = 0, sz = 0;
  uint16_t got = 0;
  uint8_t b[14];

  for (uint16_t i = 0; i < samples; i++) {
    if (rdBytes(REG_ACCEL_XOUT_H, b, 14)) {
      sx += (int16_t)((b[8]  << 8) | b[9]);
      sy += (int16_t)((b[10] << 8) | b[11]);
      sz += (int16_t)((b[12] << 8) | b[13]);
      got++;
    }
    delay(2);
  }

  if (got == 0) { Serial.println("# IMU: calibration failed, no samples"); return; }

  s_gxOff = (float)(sx / got) * GYRO_SCALE;
  s_gyOff = (float)(sy / got) * GYRO_SCALE;
  s_gzOff = (float)(sz / got) * GYRO_SCALE;

  Serial.printf("# IMU: gyro bias  x=%+.2f  y=%+.2f  z=%+.2f dps\n",
                s_gxOff, s_gyOff, s_gzOff);
}

// ---------------------------------------------------------------------------
//  INIT
// ---------------------------------------------------------------------------
bool imu_begin() {
  s_ready         = false;
  s_hasData       = false;
  g_node.imu_ok   = false;
  g_node.imu_name = "none";

  // AD0 low gives 0x68, AD0 high OR FLOATING gives 0x69. Never assume.
  if      (present(ADDR_IMU))     s_addr = ADDR_IMU;
  else if (present(ADDR_IMU + 1)) s_addr = ADDR_IMU + 1;
  else {
    Serial.println("# IMU: nothing at 0x68 or 0x69");
    return false;
  }

  s_who = rdReg(REG_WHO_AM_I);
  Serial.printf("# IMU: addr 0x%02X  WHO_AM_I 0x%02X -> %s\n",
                s_addr, s_who, imu_part_name());

  s_is6500 = (s_who == 0x70 || s_who == 0x71 || s_who == 0x73 ||
              s_who == 0x74 || s_who == 0x7C);
  const bool is6050 = (s_who == 0x68);

  if (!s_is6500 && !is6050) {
    Serial.println("# IMU: unrecognised die id, refusing to guess.");
    Serial.println("#   If this value CHANGES between resets, the I2C bus is");
    Serial.println("#   corrupting reads - fix the bus, not the sensor.");
    g_node.imu_name = "unknown";
    return false;
  }

  wrReg(REG_PWR_MGMT_1, 0x80);          // reset
  delay(100);
  wrReg(REG_PWR_MGMT_1, 0x01);          // wake, clock = gyro X PLL
  delay(50);

  // DLPF ~92 Hz on both. We sample at 200 Hz, so Nyquist is 100 Hz - a
  // wider filter would alias impact energy back down into the signal, a
  // narrower one would round the top off a 10 ms impact spike.
  wrReg(REG_CONFIG,       0x02);
  wrReg(REG_SMPLRT_DIV,   0x04);        // 1 kHz / (1 + 4) = 200 Hz
  wrReg(REG_GYRO_CONFIG,  GYRO_FS_SEL);
  wrReg(REG_ACCEL_CONFIG, ACCEL_FS_SEL);
  if (s_is6500) wrReg(REG_ACCEL_CONFIG2, 0x02);
  delay(50);

  // Never trust a write you have not read back. A silent failure here means
  // clipped impacts and a fall detector that quietly does not work.
  const uint8_t ac = rdReg(REG_ACCEL_CONFIG);
  const uint8_t gc = rdReg(REG_GYRO_CONFIG);
  const int accelRanges[] = { 2, 4, 8, 16 };
  const int gyroRanges[]  = { 250, 500, 1000, 2000 };
  const int aR = accelRanges[(ac >> 3) & 0x03];
  const int gR = gyroRanges[(gc >> 3) & 0x03];

  Serial.printf("# IMU: range +/-%d g, +/-%d dps, 200 Hz\n", aR, gR);
  if (aR != 16) Serial.println("# IMU: WARNING not +/-16 g - impacts WILL clip");
  if (gR != 2000) Serial.println("# IMU: WARNING not +/-2000 dps - spins will clip");

  s_ready         = true;
  g_node.imu_ok   = true;
  g_node.imu_name = imu_part_name();

  imu_calibrate_gyro();
  return true;
}

// ---------------------------------------------------------------------------
//  UPDATE
// ---------------------------------------------------------------------------
void imu_update() {
  if (!s_ready) return;

  static uint32_t nextMs = 0;
  if ((int32_t)(millis() - nextMs) < 0) return;
  nextMs = millis() + IMU_SAMPLE_MS;

  // One burst read of all 14 bytes, so every axis comes from the same
  // instant. Reading them separately would smear a fast impact.
  uint8_t b[14];
  if (!rdBytes(REG_ACCEL_XOUT_H, b, 14)) return;

  const int16_t axr = (int16_t)((b[0]  << 8) | b[1]);
  const int16_t ayr = (int16_t)((b[2]  << 8) | b[3]);
  const int16_t azr = (int16_t)((b[4]  << 8) | b[5]);
  const int16_t tr  = (int16_t)((b[6]  << 8) | b[7]);
  const int16_t gxr = (int16_t)((b[8]  << 8) | b[9]);
  const int16_t gyr = (int16_t)((b[10] << 8) | b[11]);
  const int16_t gzr = (int16_t)((b[12] << 8) | b[13]);

  g_node.ax = axr * ACCEL_SCALE;
  g_node.ay = ayr * ACCEL_SCALE;
  g_node.az = azr * ACCEL_SCALE;

  // |a| is the length of the acceleration vector. It does not care which
  // way the device is worn, which is exactly why the fall detector uses it
  // instead of any single axis.
  g_node.amag = sqrtf(g_node.ax * g_node.ax +
                      g_node.ay * g_node.ay +
                      g_node.az * g_node.az);

  g_node.gx = gxr * GYRO_SCALE - s_gxOff;
  g_node.gy = gyr * GYRO_SCALE - s_gyOff;
  g_node.gz = gzr * GYRO_SCALE - s_gzOff;

  g_node.temp_c = s_is6500 ? (tr / 333.87f + 21.0f)
                           : (tr / 340.0f  + 36.53f);

  const uint16_t mg = (uint16_t)(g_node.amag * 1000.0f);
  if (mg > s_peakMg) s_peakMg = mg;

  s_hasData = true;
}