// ============================================================================
//  PHANTOM NET — IMU DATA CAPTURE  (for training the fall classifier)
//
//  SKETCH FOLDER — four files, three copied from the node UNCHANGED:
//      PhantomNet_DataCapture\PhantomNet_DataCapture.ino
//      PhantomNet_DataCapture\config.h     <- exact copy
//      PhantomNet_DataCapture\imu.h        <- exact copy
//      PhantomNet_DataCapture\imu.cpp      <- exact copy
//
//  WHY THE COPIES MUST BE EXACT
//  ----------------------------
//  A model is only valid for data that looks like what it was trained on.
//  imu.cpp sets the accelerometer to +/-16 g, the gyroscope to +/-2000 dps,
//  a 92 Hz filter and a 200 Hz sample rate. Capture with ANY of those
//  different and the numbers shift, the features shift, and the trained
//  model quietly performs worse on the real device than it did in testing.
//
//  ---------------------------------------------------------------------
//  HOW TO USE — keyboard only, no button
//  ---------------------------------------------------------------------
//  1. Flash this, open the recorder:  python capture.py COM5 session1
//  2. Send a NUMBER KEY to say what you are about to do.
//  3. Send a KEY to start/stop recording:
//
//       b   -> records a 5 second burst, then stops on its own.
//              Press it, then fall. You do not have to touch the
//              keyboard again while you are on the floor.
//
//       s   -> starts/stops CONTINUOUS recording, for the things
//              that go on for minutes: walking, running, still.
//
//  capture.py forwards every key you type straight to the board, so
//  these are typed in the same terminal capture.py is running in.
//
//  The onboard LED is lit whenever data is actually being written, so you
//  can tell at a glance whether you are recording.
// ============================================================================

#include "config.h"
#include "imu.h"
#include <Wire.h>

NodeState g_node;          // imu.cpp writes into this

#define CAPTURE_HZ      200
#define SAMPLE_US       (1000000UL / CAPTURE_HZ)

#define BURST_MS        5000     // length of a 'b' burst recording

#define PIN_LED         2        // onboard LED on most ESP32 DevKits

// ---------------------------------------------------------------------------
//  ACTIVITY LABELS
//
//  0-6 are NOT falls. 7-9 are falls. Getting plenty of 3, 4 and 6 is what
//  stops the finished device crying wolf - those are the movements that
//  look most like a fall when the sensor is on a helmet.
// ---------------------------------------------------------------------------
static const char* LABEL_NAME[10] = {
  "0 still",            // standing or sitting, not moving
  "1 walking",
  "2 running",
  "3 head_turn",        // looking around, nodding, shaking head
  "4 bend_pickup",      // bending down to pick something up
  "5 sit_stand",        // sitting down and getting up
  "6 helmet_on_off",    // THE critical false positive - do plenty
  "7 FALL_forward",
  "8 FALL_backward",
  "9 FALL_side",
};

static uint8_t  s_label      = 0;
static bool     s_recording  = false;
static bool     s_continuous = false;   // true = 's' mode, no timeout
static uint32_t s_burstEnd   = 0;
static uint32_t s_rows       = 0;
static uint32_t s_bursts     = 0;

// ---------------------------------------------------------------------------
static void printMenu() {
  Serial.println("#");
  Serial.println("# ---- send a digit to set the label ----");
  for (uint8_t i = 0; i < 10; i++) Serial.printf("#   %s\n", LABEL_NAME[i]);
  Serial.println("#");
  Serial.println("# ---- keys ----");
  Serial.println("#   b : record a 5 s burst, stops itself");
  Serial.println("#   s : start / stop continuous recording");
  Serial.println("#   LED lit : data is being written");
  Serial.println("#");
}

static void startBurst() {
  s_recording  = true;
  s_continuous = false;
  s_burstEnd   = millis() + BURST_MS;
  s_bursts++;
  Serial.printf("# REC burst #%lu  [%s]  %d s\n",
                (unsigned long)s_bursts, LABEL_NAME[s_label], BURST_MS / 1000);
}

static void stopRecording(const char* why) {
  if (!s_recording) return;
  s_recording  = false;
  s_continuous = false;
  Serial.printf("# stop (%s)  total rows %lu\n", why, (unsigned long)s_rows);
}

static void toggleContinuous() {
  if (s_recording) { stopRecording("'s' pressed again"); return; }
  s_recording  = true;
  s_continuous = true;
  Serial.printf("# REC continuous  [%s]\n", LABEL_NAME[s_label]);
}

// ---------------------------------------------------------------------------
static void handleSerial() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c >= '0' && c <= '9') {
      s_label = (uint8_t)(c - '0');
      Serial.printf("# LABEL -> %s\n", LABEL_NAME[s_label]);
    } else if (c == 's' || c == 'S') {
      toggleContinuous();
    } else if (c == 'b' || c == 'B') {
      startBurst();
    } else if (c == '?') {
      printMenu();
    }
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(921600);
  delay(500);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial.println();
  Serial.println("# =========================================");
  Serial.println("#  PHANTOM NET - IMU CAPTURE FOR TRAINING");
  Serial.printf ("#  %d Hz, same sensor config as the node\n", CAPTURE_HZ);
  Serial.println("# =========================================");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(I2C_CLOCK_HZ);

  if (!imu_begin()) {
    Serial.println("# IMU not found - nothing to capture. Check wiring.");
    while (true) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(200); }
  }

  printMenu();
  Serial.println("# Set a label, then send b or s. CSV header follows.");
  Serial.println("t_ms,label,ax,ay,az,gx,gy,gz,amag");
}

// ---------------------------------------------------------------------------
void loop() {
  handleSerial();

  // A burst ends itself, so you never have to reach the keyboard while you
  // are lying on a mattress.
  if (s_recording && !s_continuous && (int32_t)(millis() - s_burstEnd) >= 0)
    stopRecording("burst complete");

  digitalWrite(PIN_LED, s_recording ? HIGH : LOW);

  static uint32_t nextUs = 0;
  const uint32_t now = micros();
  if ((int32_t)(now - nextUs) < 0) return;
  nextUs = now + SAMPLE_US;

  imu_update();               // self-limits to IMU_SAMPLE_MS, i.e. 200 Hz
  if (!s_recording) return;   // <-- nothing is written unless you asked

  // Six raw axes plus the magnitude. Python could compute amag itself, but
  // writing it here means the training features and the on-device features
  // are derived from the exact same number.
  Serial.printf("%lu,%u,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.4f\n",
                (unsigned long)millis(),
                (unsigned)s_label,
                g_node.ax, g_node.ay, g_node.az,
                g_node.gx, g_node.gy, g_node.gz,
                g_node.amag);
  s_rows++;
}