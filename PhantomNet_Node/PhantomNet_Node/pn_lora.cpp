// ============================================================================
//  PHANTOM NET — pn_lora.cpp
//  SX1278 telemetry link, now carrying AES-128-GCM encrypted frames.
//
//  DESIGN: FULLY SYNCHRONOUS. NO INTERRUPTS.
//  ------------------------------------------------------------------------
//  An earlier version used LoRa.onReceive() and LoRa.onTxDone(). Those fire
//  from the DIO0 interrupt, and the receive one has to do SPI reads to pull
//  the packet out of the radio. On ESP32 that can collide with SPI already
//  in progress in the main loop and lock the board solid - it runs fine for
//  tens of seconds then goes silent with no crash message, which is close
//  to impossible to diagnose.
//
//  This version polls. endPacket() blocks for the ~200 ms a packet spends
//  on air, once every 5 seconds. That is under 5% of the time and removes
//  an entire class of failure.
//
//  The frame format and all cryptography live in pn_crypto.h/.cpp, which
//  the gateway uses too. Neither file knows anything about the radio.
// ============================================================================

#include "pn_lora.h"
#include "pn_crypto.h"
#include <SPI.h>
#include <LoRa.h>

// ---------------------------------------------------------------------------
//  TIMING
// ---------------------------------------------------------------------------
#define TX_PERIOD_NORMAL_MS   LORA_TX_PERIOD_MS   // 5000 from config.h
#define TX_PERIOD_ALARM_MS    2000UL
#define TX_JITTER_MS          400UL               // anti-lockstep
#define PEER_STALE_MS         15000UL             // no packet -> link down

// Set to 1 to hex-dump every frame as it goes out. Useful for showing that
// the payload really is ciphertext - leave it at 0 for normal running.
#define PN_DUMP_FRAMES        0

// ---------------------------------------------------------------------------
//  STATE
// ---------------------------------------------------------------------------
static bool     s_ready      = false;
static uint32_t s_nextTxMs   = 0;
static uint16_t s_msgCounter = 0;
static const char* s_error   = "";
static bool     s_dumpedOnce = false;

static uint8_t  s_peerId     = 0;
static int16_t  s_peerRssi   = 0;
static uint8_t  s_peerStatus = 0;
static uint32_t s_peerLastMs = 0;
static bool     s_peerEver   = false;

// ---------------------------------------------------------------------------
uint8_t  lora_peer_id()       { return s_peerId; }
int16_t  lora_peer_rssi()     { return s_peerRssi; }
uint8_t  lora_peer_status()   { return s_peerStatus; }
const char* lora_last_error() { return s_error; }

uint16_t lora_peer_age_s() {
  if (!s_peerEver) return 0xFFFF;
  return (uint16_t)((millis() - s_peerLastMs) / 1000);
}

// ---------------------------------------------------------------------------
//  INIT
// ---------------------------------------------------------------------------
bool lora_begin() {
  s_ready            = false;
  g_node.lora_ok     = false;
  g_node.lora_linked = false;

  randomSeed(esp_random());
  pn_crypto_begin();          // loads and bumps the boot counter from flash

  // NOTE: no SPI.begin() here. The sketch owns the bus and initialises it
  // once, because the SD card sits on the same three wires. Two drivers
  // each re-initialising a shared bus is the exact fault that cost us hours
  // on I2C - a library "helpfully" reconfiguring it behind our back.
  LoRa.setPins(PIN_LORA_CS, PIN_LORA_RST, PIN_LORA_DIO0);

  if (!LoRa.begin(LORA_FREQ)) {
    s_error = "SX1278 no reply";
    Serial.println("# LoRa: init FAILED");
    Serial.println("#   VCC must be 3.3 V - 5 V destroys the Ra-02");
    Serial.println("#   SCK=18 MISO=19 MOSI=23 NSS=5 RST=27 DIO0=26");
    Serial.println("#   antenna MUST be attached before powering up");
    return false;
  }

  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setPreambleLength(8);
  LoRa.setSyncWord(LORA_SYNCWORD);
  LoRa.setTxPower(LORA_TXPOWER, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.enableCrc();
  LoRa.receive();             // no callbacks registered - we poll

  Serial.printf("# LoRa: OK  %.3f MHz  SF%d  BW%ld  CR4/%d  sync 0x%02X  %d dBm\n",
                LORA_FREQ / 1e6, LORA_SF, (long)LORA_BW, LORA_CR,
                LORA_SYNCWORD, LORA_TXPOWER);
  Serial.printf("# LoRa: node %u, %u-byte ENCRYPTED frame (AES-128-GCM), polled\n",
                (unsigned)NODE_ID, (unsigned)PN_FRAME_LEN);

  s_ready        = true;
  s_error        = "";
  g_node.lora_ok = true;

  s_nextTxMs = millis() + 1000 + (uint32_t)random(0, TX_JITTER_MS * 2);
  return true;
}

// ---------------------------------------------------------------------------
//  TRANSMIT
// ---------------------------------------------------------------------------
static void buildPayload(pn_payload_t& p) {
  p.status   = (uint8_t)g_node.status;
  p.hr_bpm   = g_node.hr_bpm;
  p.spo2     = g_node.spo2;
  p.batt_pct = g_node.batt_pct;

  float mg = g_node.amag * 1000.0f;
  if (mg < 0)        mg = 0;
  if (mg > 65535.0f) mg = 65535.0f;    // clamp so a clipped impact cannot wrap
  p.amag_mg = (uint16_t)mg;

  p.sats      = g_node.sats;
  p.gps_flags = g_node.gps_fix ? 0x01 : 0x00;
  p.lat       = (float)g_node.lat;
  p.lon       = (float)g_node.lon;
  p.uptime_s  = (uint16_t)(g_node.uptime_s & 0xFFFF);
}

static void transmit() {
  uint8_t flags = 0;
  if (g_node.status == STATUS_MANDOWN || g_node.status == STATUS_SOS)
    flags |= PN_FLAG_ALARM;
  if (!g_node.on_usb && g_node.batt_pct < 20)
    flags |= PN_FLAG_LOW_BATT;

  pn_payload_t pl;
  buildPayload(pl);

  uint8_t frame[PN_FRAME_LEN];
  const uint8_t n = pn_frame_build(frame, NODE_ID, PN_DST_BCAST, flags,
                                   ++s_msgCounter, PN_DEFAULT_TTL, &pl);
  if (n == 0) {
    Serial.println("# LoRa: encryption FAILED - packet not sent");
    return;
  }

  // Dump the first frame so it is on the serial log as evidence that what
  // goes on air is ciphertext, not readable telemetry.
  if (!s_dumpedOnce || PN_DUMP_FRAMES) {
    s_dumpedOnce = true;
    Serial.println("# --- first frame on air ---");
    pn_hexdump("plaintext payload", (const uint8_t*)&pl, PN_PLAIN_LEN);
    pn_hexdump("encrypted frame",   frame, n);
    Serial.println("#   bytes 0-10 are the readable header (authenticated),");
    Serial.println("#   13-30 are ciphertext, last 8 are the GCM tag.");
  }

  LoRa.beginPacket();
  LoRa.write(frame, n);
  LoRa.endPacket();          // BLOCKING - returns when the packet is sent
  LoRa.receive();            // straight back to listening

  g_node.tx_count++;
}

// ---------------------------------------------------------------------------
//  RECEIVE
// ---------------------------------------------------------------------------
static void handlePacket(int len) {
  uint8_t buf[PN_FRAME_LEN];
  if (len != PN_FRAME_LEN) {
    while (LoRa.available()) LoRa.read();
    return;
  }
  for (int i = 0; i < len; i++) {
    if (!LoRa.available()) return;
    buf[i] = (uint8_t)LoRa.read();
  }

  pn_hdr_t     h;
  pn_payload_t pl;
  uint8_t      ttl, hops;

  // Fails on a corrupted packet, a forged one, or one encrypted with a
  // different key. There is no partial success and nothing to salvage.
  if (!pn_frame_open(buf, len, &h, &pl, &ttl, &hops)) return;
  if (h.src == NODE_ID) return;                       // our own echo
  if (!pn_replay_ok(h.src, h.boot, h.msg)) return;    // already seen

  s_peerId     = h.src;
  s_peerStatus = pl.status;
  s_peerRssi   = (int16_t)LoRa.packetRssi();
  s_peerLastMs = millis();
  s_peerEver   = true;

  g_node.rx_count++;
  g_node.rssi = s_peerRssi;

  Serial.printf("# LoRa RX  node %u  rssi=%d  status=%u  |a|=%.3fg  hr=%u  "
                "up=%us  (boot %u msg %u)\n",
                (unsigned)h.src, (int)s_peerRssi, (unsigned)pl.status,
                pl.amag_mg / 1000.0f, (unsigned)pl.hr_bpm,
                (unsigned)pl.uptime_s, (unsigned)h.boot, (unsigned)h.msg);

  if (h.flags & PN_FLAG_ALARM) {
    Serial.printf("# LoRa RX  *** ALARM from node %u ***\n", (unsigned)h.src);
  }
}

// ---------------------------------------------------------------------------
//  UPDATE
// ---------------------------------------------------------------------------
void lora_update() {
  if (!s_ready) return;

  const int sz = LoRa.parsePacket();
  if (sz > 0) handlePacket(sz);

  g_node.lora_linked = s_peerEver &&
                       (millis() - s_peerLastMs < PEER_STALE_MS);

  if ((int32_t)(millis() - s_nextTxMs) >= 0) {
    transmit();

    const uint32_t base = (g_node.status == STATUS_MANDOWN ||
                           g_node.status == STATUS_SOS)
                          ? TX_PERIOD_ALARM_MS
                          : TX_PERIOD_NORMAL_MS;

    const int32_t jit = (int32_t)random(0, TX_JITTER_MS * 2) - (int32_t)TX_JITTER_MS;
    s_nextTxMs = millis() + base + jit;
  }
}