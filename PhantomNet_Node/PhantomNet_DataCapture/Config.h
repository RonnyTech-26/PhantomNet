// ============================================================================
//  PHANTOM NET — config.h
//
//  One place for every pin and setting. If a number appears anywhere else
//  in the firmware, that is a bug.
// ============================================================================
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
//  NODE IDENTITY   <<< CHANGE THIS PER BOARD >>>
// ---------------------------------------------------------------------------
#define NODE_ID            2        // 1 on the first board, 2 on the second
#define FIRMWARE_VERSION   "0.6"

// ---------------------------------------------------------------------------
//  I2C PINS   <<< THESE MUST MATCH WHERE THE WIRES PHYSICALLY ARE >>>
//
//  This caused hours of confusion once already. A #define that disagrees
//  with the wiring behaves EXACTLY like a dead sensor: the scan finds
//  nothing and every driver reports "not present".
//
//  The firmware prints these pin numbers at boot. If the printed values are
//  not the pins your jumpers are plugged into, nothing else will work and
//  no amount of debugging elsewhere will help.
// ---------------------------------------------------------------------------
#define PIN_I2C_SDA        21
#define PIN_I2C_SCL        22

// 100 kHz. Slower is more tolerant of long dupont wiring. Raise to 400000
// only once everything is short and soldered.
#define I2C_CLOCK_HZ       100000UL

// ---------------------------------------------------------------------------
//  MODULE ENABLE FLAGS
//
//  IMPORTANT: setting one of these to 0 only stops the SOFTWARE talking to
//  that device. The module is still electrically on the bus and can still
//  drag it down. To rule a device out, physically unplug it.
// ---------------------------------------------------------------------------
#define ENABLE_MAX30102    1
#define ENABLE_LORA        1
#define ENABLE_OLED        0        // parked
#define ENABLE_IMU         1
#define ENABLE_GPS         1
#define ENABLE_BATTERY     0        // divider not fitted

// Cycles status NORMAL -> ALERT -> MAN DOWN so the alarm path can be shown
// without dropping the board. Also FAKES a heart rate when no sensor is
// present - which is why readings appeared with the sensor unplugged.
#define DEMO_MODE          0

// ---------------------------------------------------------------------------
//  LORA PINS (SX1278) — SPI, unrelated to the I2C bus
// ---------------------------------------------------------------------------
#define PIN_LORA_SCK       18
#define PIN_LORA_MISO      19
#define PIN_LORA_MOSI      23
#define PIN_LORA_CS         5
#define PIN_LORA_RST       27
#define PIN_LORA_DIO0      26

#define LORA_FREQ          433775000UL
#define LORA_SF            9
#define LORA_BW            125000L
#define LORA_CR            5
#define LORA_SYNCWORD      0x2A
#define LORA_TXPOWER       17
#define LORA_TX_PERIOD_MS  5000UL

// ---------------------------------------------------------------------------
//  GPS (NEO-6M) on UART2
//
//  CROSS THE LINES. The GPS module's TX goes to the ESP32's RX. Wiring
//  TX-to-TX produces total silence and looks exactly like a dead module.
//
//  GPS_BAUD is only a fallback - gps_begin() probes for the real rate,
//  because the module we were shipped came set to UBX binary at 57600
//  rather than the NEO-6M factory default of NMEA at 9600.
// ---------------------------------------------------------------------------
#define PIN_GPS_RX         16       // ESP32 RECEIVES here  <- GPS TX
#define PIN_GPS_TX         17       // ESP32 transmits here -> GPS RX
#define GPS_BAUD           9600UL

// ---------------------------------------------------------------------------
//  I2C ADDRESSES
// ---------------------------------------------------------------------------
#define ADDR_MAX30102      0x57
#define ADDR_OLED          0x3C
#define ADDR_IMU           0x68

// ---------------------------------------------------------------------------
//  TIMING
// ---------------------------------------------------------------------------
#define HR_SAMPLE_MS       20       // 50 Hz poll of the MAX30102 FIFO
#define IMU_SAMPLE_MS      5        // 200 Hz - an impact lasts only 5-15 ms,
                                    // so 50 Hz can step right over it
#define HEARTBEAT_MS       2000     // serial status line

// ---------------------------------------------------------------------------
//  FALL DETECTION
//
//  MEASURED ON THIS HARDWARE, not copied from a paper. Drop tests gave a
//  clean impact peak of 7.9 g, pre-impact free-fall down to 0.38 g, and a
//  real topple produced 33 degrees of orientation change. These thresholds
//  sit comfortably inside those figures so marginal falls still trip.
//
//  Always tune to the WORST case you measured, never the average - tuning
//  to the average guarantees you miss half your falls.
// ---------------------------------------------------------------------------
#define FALL_FREEFALL_G       0.50f    // below this = weightless
#define FALL_FREEFALL_MIN_MS  30       // must stay weightless this long
#define FALL_IMPACT_G         2.50f    // above this = hit something
#define FALL_WINDOW_MS        800      // free-fall must be followed by
                                       // impact within this, else ignore
#define FALL_STILL_MS         2000     // motionless this long = did not get up
#define FALL_STILL_TOL_G      0.30f    // |a| within 1g +/- this counts as still
#define FALL_STILL_GYRO_DPS   50.0f    // and rotating slower than this
#define FALL_TILT_DEG         25.0f    // orientation must have changed by this
#define FALL_LATCH_MS         60000UL  // auto-clear, so a demo can repeat

// ---------------------------------------------------------------------------
//  STATUS
// ---------------------------------------------------------------------------
enum NodeStatus : uint8_t {
  STATUS_NORMAL   = 0,
  STATUS_ALERT    = 1,
  STATUS_MANDOWN  = 2,
  STATUS_SOS      = 3,
};

// ---------------------------------------------------------------------------
//  SHARED NODE STATE
//
//  Every module writes into this; the radio reads it. Field list is
//  unchanged from v0.5 so the 26-byte LoRa frame, the gateway and the
//  dashboard all keep working without modification.
// ---------------------------------------------------------------------------
struct NodeState {
  uint8_t     node_id       = NODE_ID;
  uint32_t    uptime_s      = 0;

  // --- IMU (parked) ---
  bool        imu_ok        = false;
  const char* imu_name      = "none";
  float       ax = 0, ay = 0, az = 0;
  float       amag          = 0;
  float       gx = 0, gy = 0, gz = 0;
  float       temp_c        = 0;

  // --- heart rate ---
  bool        hr_ok         = false;   // sensor present and configured
  bool        finger        = false;   // something on the window
  uint8_t     hr_bpm        = 0;
  uint8_t     spo2          = 0;

  // --- GPS (not wired) ---
  bool        gps_ok        = false;
  bool        gps_fix       = false;
  uint8_t     sats          = 0;
  double      lat           = 0.0;
  double      lon           = 0.0;

  // --- LoRa ---
  bool        lora_ok       = false;
  bool        lora_linked   = false;
  int16_t     rssi          = 0;
  uint32_t    tx_count      = 0;
  uint32_t    rx_count      = 0;

  // --- power ---
  bool        on_usb        = true;
  uint16_t    batt_mv       = 0;
  uint8_t     batt_pct      = 0;

  // --- status ---
  NodeStatus  status        = STATUS_NORMAL;
};

extern NodeState g_node;