// ============================================================================
//  PHANTOM NET — Wearable Node
//  v0.6 — clean rebuild: heart rate + LoRa telemetry
//
//  FILES IN THIS SKETCH FOLDER (6):
//     PhantomNet_Node.ino   this file
//     config.h              every pin and setting, and NodeState
//     max30102.h/.cpp       heart rate + SpO2, raw register driver
//     pn_lora.h/.cpp        SX1278 telemetry, polled (no interrupts)
//
//  OLED and IMU are deliberately absent. They are parked, and their flags
//  in config.h are 0. Nothing here depends on them.
//
//  ARCHITECTURE
//  ------------
//  Every module writes into one shared NodeState. The radio reads from it.
//  No module calls another module. That is what lets any one of them be
//  switched off without touching the rest.
//
//  TWO RULES LEARNED THE HARD WAY
//  ------------------------------
//  1. THE SKETCH OWNS THE I2C BUS. Wire.begin() and setClock() are called
//     here, exactly once, and nowhere else. Arduino libraries habitually
//     call them "to be helpful", which is fine with one device and actively
//     destructive with three. Both drivers here only read and write
//     registers.
//
//  2. AN ENABLE_ FLAG IS NOT AN UNPLUG. Setting ENABLE_OLED 0 stops the
//     software talking to it; the module is still on the bus, still loading
//     it, and can still drag every other device down. To rule a device out,
//     physically remove it.
// ============================================================================

#include "config.h"
#include "imu.h"
#include "fall.h"
#include "max30102.h"
#include "gps.h"
#include "pn_lora.h"
#include "sdlog.h"
#include "button.h"
#include <Wire.h>
#include <SPI.h>

NodeState g_node;

static uint32_t s_bootMs = 0;

// ---------------------------------------------------------------------------
//  I2C bus recovery. Must run BEFORE Wire.begin().
//
//  If the ESP32 resets mid-transaction — which happens on every upload — a
//  slave can be left holding SDA low, waiting for clocks that never arrive.
//  The bus stays jammed until that chip loses power. Nine manual clock
//  pulses let it finish and release the line.
// ---------------------------------------------------------------------------
static void i2cRecover() {
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
  delay(5);

  const bool sda = digitalRead(PIN_I2C_SDA);
  const bool scl = digitalRead(PIN_I2C_SCL);
  Serial.printf("  I2C idle : SDA=%s  SCL=%s\n",
                sda ? "HIGH" : "LOW", scl ? "HIGH" : "LOW");

  if (sda && scl) return;                 // bus is free

  if (!scl) {
    Serial.println("  !! SCL held LOW - a device is clamping the clock, or");
    Serial.println("     SCL is shorted to GND. Software cannot fix this.");
    return;
  }

  Serial.println("  !! SDA held LOW - clocking 9 pulses to free the bus");
  pinMode(PIN_I2C_SCL, OUTPUT);
  for (uint8_t i = 0; i < 9 && digitalRead(PIN_I2C_SDA) == LOW; i++) {
    digitalWrite(PIN_I2C_SCL, LOW);  delayMicroseconds(10);
    digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(10);
  }
  pinMode(PIN_I2C_SDA, OUTPUT);           // manual STOP
  digitalWrite(PIN_I2C_SDA, LOW);  delayMicroseconds(10);
  digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_I2C_SDA, HIGH); delayMicroseconds(10);

  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
  delay(5);
  Serial.printf("  recovery : SDA=%s\n",
                digitalRead(PIN_I2C_SDA) ? "HIGH (freed)" : "LOW (still stuck)");
}

// ---------------------------------------------------------------------------
//  Bus scan. Always prints the pins actually in use — a #define that
//  disagrees with the wiring behaves exactly like a dead sensor.
// ---------------------------------------------------------------------------
static void busScan(const char* when) {
  Serial.printf("  I2C scan (%-10s) SDA=GPIO%-2d SCL=GPIO%-2d :",
                when, PIN_I2C_SDA, PIN_I2C_SCL);
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      found++;
      Serial.printf("  0x%02X", a);
      if      (a == ADDR_MAX30102) Serial.print("(hr)");
      else if (a == ADDR_OLED)     Serial.print("(oled)");
      else if (a == 0x68 || a == 0x69) Serial.print("(imu)");
    }
    delay(1);
  }
  if (!found) Serial.print("  none");
  Serial.printf("   [%u]\n", found);

  if (found > 8) {
    Serial.println("  !! Dozens of devices means SDA is floating or held low.");
    Serial.println("     That is electrical, not software. Check power, ground");
    Serial.println("     and that SDA/SCL are not swapped.");
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(400);
  s_bootMs = millis();

  Serial.println();
  Serial.println("==================================================");
  Serial.println("  PHANTOM NET  -  WEARABLE NODE");
  Serial.printf ("  node %02u    firmware %s\n", (unsigned)NODE_ID, FIRMWARE_VERSION);
  Serial.println("==================================================");
  Serial.printf ("  chip     : %s rev %d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf ("  free RAM : %u bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.printf ("  I2C pins : SDA=GPIO%d  SCL=GPIO%d  @ %lu Hz\n",
                 PIN_I2C_SDA, PIN_I2C_SCL, (unsigned long)I2C_CLOCK_HZ);
  Serial.printf ("  modules  : IMU=%s  HR=%s  GPS=%s  LoRa=%s  SD=%s  "
                 "OLED=%s  Batt=%s  Btn=%s\n",
                 ENABLE_IMU      ? "on" : "off",
                 ENABLE_MAX30102 ? "on" : "off",
                 ENABLE_GPS      ? "on" : "off",
                 ENABLE_LORA     ? "on" : "off",
                 ENABLE_SDLOG    ? "on" : "off",
                 ENABLE_OLED     ? "on" : "off",
                 ENABLE_BATTERY  ? "on" : "off",
                 ENABLE_BUTTON   ? "on" : "off");
  Serial.println();

  // ---- the sketch owns the bus. Set up once, here, and nowhere else ----
  i2cRecover();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(I2C_CLOCK_HZ);
  busScan("boot");

  // ---- drivers ----
#if ENABLE_IMU
  if (imu_begin()) fall_begin();
#endif

#if ENABLE_BUTTON
  button_begin();
#endif

#if ENABLE_MAX30102
  max30102_begin();          // sets g_node.hr_ok itself
#endif

#if ENABLE_GPS
  gps_begin();               // probes for baud, sets g_node.gps_ok itself
#endif

  // ---- the sketch owns the SPI bus too --------------------------------
  // The radio and the SD card share SCK, MISO and MOSI, and are told apart
  // by their own chip-select lines. Initialise it ONCE here; neither driver
  // calls SPI.begin(). Same rule as I2C above, learned the same way.
#if ENABLE_LORA || ENABLE_SDLOG
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, -1);
  Serial.printf("  SPI pins : SCK=GPIO%d MISO=GPIO%d MOSI=GPIO%d "
                "(LoRa CS=%d, SD CS=%d)\n",
                PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI,
                PIN_LORA_CS, PIN_SD_CS);
#endif

#if ENABLE_LORA
  lora_begin();              // sets g_node.lora_ok itself
#endif

  // AFTER lora_begin(): the log file is named from the boot counter, which
  // pn_crypto_begin() loads during radio init.
#if ENABLE_SDLOG
  sdlog_begin();             // sets g_node.sd_ok itself
#endif

  // If this differs from the boot scan, a driver reconfigured the bus
  // during its own init - which would be the bug, not the wiring.
  busScan("after init");

  Serial.println();
  Serial.println("  entering main loop");
  Serial.println();
}

// ---------------------------------------------------------------------------
//  Per-module stall timing.
//
//  A node that "goes offline" with no crash message has something blocking
//  inside the loop. Guessing which is a waste of time, so measure it. The
//  worst case since the last heartbeat is printed on every status line.
// ---------------------------------------------------------------------------
static uint16_t mImu = 0, mHr = 0, mGps = 0, mLora = 0, mSd = 0, mBtn = 0, mLoop = 0;
static inline uint16_t lap(uint32_t& mark) {
  const uint32_t n = millis();
  const uint16_t d = (uint16_t)(n - mark);
  mark = n;
  return d;
}
#define KEEPMAX(s, v) do { const uint16_t v_ = (v); if (v_ > (s)) (s) = v_; } while (0)

// ---------------------------------------------------------------------------
void loop() {
  uint32_t mark = millis();
  const uint32_t loopStart = mark;

#if ENABLE_IMU
  imu_update();
  fall_update();             // must run AFTER imu_update
  KEEPMAX(mImu, lap(mark));
#endif

#if ENABLE_MAX30102
  max30102_update();
  KEEPMAX(mHr, lap(mark));
#endif

#if ENABLE_BUTTON
  button_update();
  KEEPMAX(mBtn, lap(mark));
#endif

#if ENABLE_GPS
  gps_update();
  KEEPMAX(mGps, lap(mark));
#endif

#if ENABLE_LORA
  lora_update();
  KEEPMAX(mLora, lap(mark));
#endif

#if ENABLE_SDLOG
  // Anything worth alarming about is worth recording immediately, before
  // the battery has a chance to die. Routine rows are written on a timer
  // inside sdlog_update().
  static NodeStatus lastStatus = STATUS_NORMAL;
  if (g_node.status != lastStatus) {
    static const char* STNAME[] = { "NORMAL", "ALERT", "MANDOWN", "SOS" };
    sdlog_event("STATUS", STNAME[g_node.status & 0x03]);
    lastStatus = g_node.status;
  }
  #if ENABLE_IMU
  static FallState lastFall = FALL_IDLE;
  if (fall_state() != lastFall) {
    sdlog_event("FALL", fall_state_name());
    lastFall = fall_state();
  }
  #endif

  #if ENABLE_BUTTON
  if (button_event_sos())   sdlog_event("BUTTON", "double-click - SOS declared");
  if (button_event_clear()) sdlog_event("BUTTON", "hold - alarm cleared, I'm OK");
  #endif

  sdlog_update();
  KEEPMAX(mSd, lap(mark));
#endif

  g_node.uptime_s = (millis() - s_bootMs) / 1000;
  g_node.on_usb   = true;

#if DEMO_MODE
  // Cycles the status field so the alarm path can be demonstrated without
  // dropping the board. Leave this at 0 for real readings - it also fakes
  // a heart rate when no sensor is fitted.
  const uint32_t phase = ((millis() - s_bootMs) / 6000) % 3;
  g_node.status = (phase == 0) ? STATUS_NORMAL
                : (phase == 1) ? STATUS_ALERT
                               : STATUS_MANDOWN;
#endif

  KEEPMAX(mLoop, (uint16_t)(millis() - loopStart));

  // ---- status line ----
  static uint32_t lastLog = 0;
  if (millis() - lastLog >= HEARTBEAT_MS) {
    lastLog = millis();

    Serial.printf("up=%4lus  st=%d", (unsigned long)g_node.uptime_s,
                  (int)g_node.status);

#if ENABLE_IMU
    if (!g_node.imu_ok) Serial.print("  | IMU absent");
    else Serial.printf("  | %s |a|=%.3fg %.0fC  fall:%s",
                       g_node.imu_name, g_node.amag, g_node.temp_c,
                       fall_state_name());
#endif

#if ENABLE_MAX30102
    if (!g_node.hr_ok)        Serial.print("  | HR sensor absent");
    else if (!g_node.finger)  Serial.printf("  | HR no finger (ir=%lu)",
                                            (unsigned long)max30102_ir());
    else                      Serial.printf("  | HR %3u bpm  SpO2 %3u%%  q=%3u%%  ir=%lu amp=%.0f",
                                            (unsigned)g_node.hr_bpm,
                                            (unsigned)g_node.spo2,
                                            (unsigned)max30102_quality(),
                                            (unsigned long)max30102_ir(),
                                            max30102_amp());
#endif

#if ENABLE_GPS
    if (!g_node.gps_ok)        Serial.printf("  | GPS no data");
    else if (g_node.gps_fix)   Serial.printf("  | GPS FIX %u sats  %.5f,%.5f hdop=%.1f",
                                             (unsigned)g_node.sats,
                                             g_node.lat, g_node.lon, gps_hdop());
    else                       Serial.printf("  | GPS searching (%u sats, %lu sent)",
                                             (unsigned)g_node.sats,
                                             (unsigned long)gps_sentences());
#endif

#if ENABLE_LORA
    if (!g_node.lora_ok)          Serial.printf("  | LoRa FAIL (%s)", lora_last_error());
    else if (g_node.lora_linked)  Serial.printf("  | LoRa tx=%lu rx=%lu peer%u@%ddBm",
                                                (unsigned long)g_node.tx_count,
                                                (unsigned long)g_node.rx_count,
                                                (unsigned)lora_peer_id(),
                                                (int)lora_peer_rssi());
    else                          Serial.printf("  | LoRa tx=%lu rx=%lu no peer",
                                                (unsigned long)g_node.tx_count,
                                                (unsigned long)g_node.rx_count);
#endif

    // Everything should be single-digit ms except LoRa, which spikes to
    // ~200 ms on the loop where it actually transmits. That one is normal.
#if ENABLE_SDLOG
    if (!g_node.sd_ok) Serial.print("  | SD absent");
    else Serial.printf("  | SD %s %lu rows %luKB free=%luMB%s",
                       sdlog_filename(), (unsigned long)sdlog_rows(),
                       (unsigned long)(sdlog_bytes() / 1024),
                       (unsigned long)sdlog_free_mb(),
                       sdlog_errors() ? " ERR" : "");
#endif

    Serial.printf("  | ms imu=%u hr=%u gps=%u lora=%u sd=%u btn=%u loop=%u",
                  mImu, mHr, mGps, mLora, mSd, mBtn, mLoop);
    mImu = mHr = mGps = mLora = mSd = mBtn = mLoop = 0;

    Serial.println();
  }

  delay(2);
}