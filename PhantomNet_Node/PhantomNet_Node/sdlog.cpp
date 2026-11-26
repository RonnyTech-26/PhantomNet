// ============================================================================
//  PHANTOM NET — sdlog.cpp
//  MicroSD black-box recorder.
//
//  SHARING THE SPI BUS WITH THE RADIO
//  ----------------------------------
//  The SD card and the SX1278 sit on the same three SPI wires (SCK, MISO,
//  MOSI) and are told apart by their own chip-select lines. That works, but
//  only if one rule is obeyed:
//
//      THE SKETCH OWNS THE BUS. Drivers use it, they do not configure it.
//
//  SPI.begin() is called exactly once, in the .ino, before either driver
//  starts. Neither this file nor pn_lora.cpp calls it. We learned this the
//  hard way on I2C, where two libraries each "helpfully" re-initialising
//  the bus produced corrupted reads that looked like dead sensors.
//
//  Both libraries wrap their transfers in SPI.beginTransaction() /
//  endTransaction(), so they can run at different clock speeds without
//  interfering - the settings are swapped per transaction.
//
//  POWER-SAFE WRITING
//  ------------------
//  A file write goes into a RAM cache first. If the battery dies before
//  that cache reaches flash, those rows never existed. flush() forces the
//  cache out to physical memory. Doing it on every row would wear the card
//  and stall the loop, so we flush every few rows - and always immediately
//  after an event, because that is exactly when power loss is likely.
// ============================================================================

#include "sdlog.h"
#include "pn_crypto.h"      // for pn_boot_count() - names the log file
#include <SPI.h>
#include <SD.h>

// ---------------------------------------------------------------------------
//  STATE
// ---------------------------------------------------------------------------
static bool     s_ready   = false;
static File     s_file;
static char     s_name[20] = "";
static uint32_t s_rows    = 0;
static uint32_t s_bytes   = 0;
static uint32_t s_errors  = 0;
static uint16_t s_sinceFlush = 0;
static uint32_t s_cardMB  = 0;

// ---------------------------------------------------------------------------
const char* sdlog_filename() { return s_name; }
uint32_t    sdlog_rows()     { return s_rows; }
uint32_t    sdlog_bytes()    { return s_bytes; }
uint32_t    sdlog_errors()   { return s_errors; }
uint32_t    sdlog_card_mb()  { return s_cardMB; }

uint32_t sdlog_free_mb() {
  if (!s_ready) return 0;
  const uint64_t used = SD.usedBytes();
  const uint64_t tot  = SD.totalBytes();
  return (uint32_t)((tot - used) / (1024ULL * 1024ULL));
}

// ---------------------------------------------------------------------------
static const char* cardTypeName(uint8_t t) {
  switch (t) {
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC";
    default:        return "unknown";
  }
}

// ---------------------------------------------------------------------------
//  INIT
// ---------------------------------------------------------------------------
bool sdlog_begin() {
  s_ready      = false;
  g_node.sd_ok = false;
  s_rows = s_bytes = s_errors = 0;

  // Park BOTH chip selects high before touching the bus. A slave that is
  // left selected will answer transfers meant for the other one, and the
  // symptom is a card that "sometimes" mounts.
  pinMode(PIN_SD_CS, OUTPUT);   digitalWrite(PIN_SD_CS, HIGH);
#if ENABLE_LORA
  pinMode(PIN_LORA_CS, OUTPUT); digitalWrite(PIN_LORA_CS, HIGH);
#endif
  delay(10);

  // 4 MHz deliberately. The card will happily do 25 MHz on a short, clean
  // PCB trace; on dupont jumpers sharing a bus with a radio it will not,
  // and the failure looks like random corruption rather than a clean error.
  if (!SD.begin(PIN_SD_CS, SPI, 4000000)) {
    Serial.println("# SD: no card / mount FAILED");
    Serial.printf ("#   CS=GPIO%d  shares SCK=%d MISO=%d MOSI=%d with the radio\n",
                   PIN_SD_CS, PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI);
    Serial.println("#   check the card is FAT32 formatted and pushed fully in");
    return false;
  }

  const uint8_t type = SD.cardType();
  if (type == CARD_NONE) {
    Serial.println("# SD: card slot empty");
    return false;
  }

  s_cardMB = (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL));

  // One file per power cycle, numbered from the boot counter already kept
  // in flash for the crypto nonce. That gives every run its own file with
  // no risk of overwriting a previous incident, and no clock needed.
  snprintf(s_name, sizeof(s_name), "/PN_%04u.CSV", (unsigned)pn_boot_count());

  s_file = SD.open(s_name, FILE_WRITE);
  if (!s_file) {
    Serial.printf("# SD: cannot create %s\n", s_name);
    return false;
  }

  // node and boot go in EVERY row, not just the filename. A card read six
  // months later, or a row copied into a report, must identify itself
  // without relying on someone remembering which device it came out of.
  s_file.println(F("node,boot,uptime_s,event,detail,status,hr_bpm,spo2,"
                   "amag_g,imu,sats,fix,lat,lon,batt_pct,rssi,tx,rx"));
  s_file.flush();

  s_ready      = true;
  g_node.sd_ok = true;

  Serial.printf("# SD: OK  %s card, %lu MB, %lu MB free\n",
                cardTypeName(type), (unsigned long)s_cardMB,
                (unsigned long)sdlog_free_mb());
  Serial.printf("# SD: logging to %s every %u ms\n",
                s_name, (unsigned)SDLOG_PERIOD_MS);

  if (sdlog_free_mb() < 5) {
    Serial.println("# SD: WARNING under 5 MB free - delete old logs");
  }

  sdlog_event("BOOT", "logging started");
  return true;
}

// ---------------------------------------------------------------------------
//  ROW WRITER
// ---------------------------------------------------------------------------
static void writeRow(const char* event, const char* detail) {
  if (!s_ready) return;

  char line[220];
  const int n = snprintf(line, sizeof(line),
      "%u,%u,%lu,%s,%s,%u,%u,%u,%.3f,%u,%u,%u,%.6f,%.6f,%u,%d,%lu,%lu",
      (unsigned)NODE_ID,
      (unsigned)pn_boot_count(),
      (unsigned long)g_node.uptime_s,
      event ? event : "",
      detail ? detail : "",
      (unsigned)g_node.status,
      (unsigned)g_node.hr_bpm,
      (unsigned)g_node.spo2,
      g_node.amag,
      (unsigned)(g_node.imu_ok ? 1 : 0),
      (unsigned)g_node.sats,
      (unsigned)(g_node.gps_fix ? 1 : 0),
      g_node.lat, g_node.lon,
      (unsigned)g_node.batt_pct,
      (int)g_node.rssi,
      (unsigned long)g_node.tx_count,
      (unsigned long)g_node.rx_count);

  if (n <= 0) { s_errors++; return; }

  const size_t w = s_file.println(line);
  if (w == 0) {
    // A card pulled out mid-run, or a worn-out sector. Stop pretending we
    // are recording - a black box that silently stopped is worse than one
    // that says it failed.
    s_errors++;
    if (s_errors == 1)
      Serial.println("# SD: write FAILED - card removed or full?");
    if (s_errors > 10) {
      Serial.println("# SD: too many write errors, logging disabled");
      s_file.close();
      s_ready      = false;
      g_node.sd_ok = false;
    }
    return;
  }

  s_rows++;
  s_bytes += w;
  s_sinceFlush++;
}

// ---------------------------------------------------------------------------
void sdlog_flush() {
  if (!s_ready) return;
  s_file.flush();
  s_sinceFlush = 0;
}

// ---------------------------------------------------------------------------
void sdlog_event(const char* tag, const char* detail) {
  if (!s_ready) return;
  writeRow(tag, detail);
  sdlog_flush();          // events are exactly when power is about to go
  Serial.printf("# SD: logged event %s (%s)\n", tag, detail ? detail : "");
}

// ---------------------------------------------------------------------------
void sdlog_update() {
  if (!s_ready) return;

  static uint32_t nextMs = 0;
  if ((int32_t)(millis() - nextMs) < 0) return;
  nextMs = millis() + SDLOG_PERIOD_MS;

  writeRow("", "");

  if (s_sinceFlush >= SDLOG_FLUSH_EVERY) sdlog_flush();
}
