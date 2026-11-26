// ============================================================================
//  PHANTOM NET — GATEWAY  (encrypted + cloud-linked)
//
//  STANDALONE SKETCH. Own folder, FIVE files:
//      PhantomNet_Gateway\PhantomNet_Gateway.ino
//      PhantomNet_Gateway\pn_crypto.h      <- identical copy from the node
//      PhantomNet_Gateway\pn_crypto.cpp    <- identical copy from the node
//      PhantomNet_Gateway\CloudConfig.h    <- fill in WiFi + Supabase creds
//      PhantomNet_Gateway\CloudBridge.h    <- WiFi + Supabase REST push
//
//  Flash this to the THIRD ESP32. It is not a wearable node: no OLED, no
//  sensors. It listens to every frame in range, authenticates and decrypts
//  it, prints one line of JSON per packet to Serial, and pushes the same
//  JSON to Supabase over its own WiFi - PhantomNet_Dashboard.html reads it
//  from there, on ANY device (laptop or phone) at once, not just whichever
//  machine happens to be plugged into this board over USB.
//
//  WIRING: only the Ra-02.
//      3V3->VCC  GND->GND  18->SCK  19->MISO  23->MOSI  5->NSS  27->RST  26->DIO0
//      100 nF across VCC/GND at the module. Antenna attached BEFORE power.
//
//  SETUP: fill in CloudConfig.h (WiFi SSID/password, Supabase project URL
//  + service_role key) before flashing - see the comment block at the top
//  of that file for the exact Supabase project/table setup steps.
//
//  SERIAL: 115200.
//    Lines starting with '{' are data - the dashboard can still read these
//    directly if you ever reconnect it over USB, but normally reads the
//    same lines from Supabase instead.
//    Lines starting with '#' are commentary - the dashboard ignores them.
//  So you can watch the same port in a plain Serial Monitor while the
//  dashboard (over USB or cloud) is running, and neither confuses the other.
// ============================================================================

#include <SPI.h>
#include <LoRa.h>
#include "pn_crypto.h"
#include "CloudConfig.h"
#include "CloudBridge.h"

static CloudBridge cloud;

// ---------------------------------------------------------------------------
//  RADIO — must match config.h on the nodes exactly
// ---------------------------------------------------------------------------
#define PIN_LORA_SCK    18
#define PIN_LORA_MISO   19
#define PIN_LORA_MOSI   23
#define PIN_LORA_CS      5
#define PIN_LORA_RST    27
#define PIN_LORA_DIO0   26

#define LORA_FREQ       433775000UL
#define LORA_SF         9
#define LORA_BW         125000L
#define LORA_CR         5
#define LORA_SYNCWORD   0x2A
#define LORA_TXPOWER    17

#define GATEWAY_ID      99

// ---------------------------------------------------------------------------
//  NODE TABLE
// ---------------------------------------------------------------------------
#define MAX_NODES        8
#define NODE_STALE_MS    30000UL

struct NodeRec {
  uint8_t  id;
  uint32_t lastMs;
  uint32_t packets;
  uint16_t lastMsg;
  uint16_t lastBoot;
  uint32_t missed;
  int16_t  rssi;
  bool     alive;
};

static NodeRec  s_nodes[MAX_NODES];
static uint8_t  s_nodeCount = 0;
static uint32_t s_totalRx   = 0;
static uint32_t s_badAuth   = 0;   // failed the GCM tag: corrupt or forged
static uint32_t s_badSize   = 0;   // not our frame at all

// ---------------------------------------------------------------------------
static NodeRec* findOrAdd(uint8_t id) {
  for (uint8_t i = 0; i < s_nodeCount; i++)
    if (s_nodes[i].id == id) return &s_nodes[i];
  if (s_nodeCount >= MAX_NODES) return nullptr;

  NodeRec* n = &s_nodes[s_nodeCount++];
  n->id = id; n->packets = 0; n->missed = 0;
  n->lastMsg = 0; n->lastBoot = 0; n->alive = false;
  return n;
}

// ---------------------------------------------------------------------------
//  JSON OUTPUT — one line, no embedded newlines
// ---------------------------------------------------------------------------
static void emitData(const pn_hdr_t& h, const pn_payload_t& p,
                     uint8_t hops, int16_t rssi, float snr, uint32_t missed) {
  char json[420];
  snprintf(json, sizeof(json),
    "{\"t\":\"data\",\"node\":%u,\"boot\":%u,\"msg\":%u,\"hops\":%u,"
    "\"enc\":true,\"status\":%u,\"alarm\":%s,\"lowbatt\":%s,"
    "\"hr\":%u,\"spo2\":%u,\"amag\":%.3f,\"fix\":%s,\"sats\":%u,"
    "\"lat\":%.6f,\"lon\":%.6f,\"batt\":%u,\"rssi\":%d,\"snr\":%.1f,"
    "\"uptime\":%u,\"missed\":%lu,\"gw_ms\":%lu}",
    (unsigned)h.src, (unsigned)h.boot, (unsigned)h.msg, (unsigned)hops,
    (unsigned)p.status, (h.flags & PN_FLAG_ALARM) ? "true" : "false",
    (h.flags & PN_FLAG_LOW_BATT) ? "true" : "false",
    (unsigned)p.hr_bpm, (unsigned)p.spo2, p.amag_mg / 1000.0f,
    (p.gps_flags & 0x01) ? "true" : "false", (unsigned)p.sats,
    p.lat, p.lon, (unsigned)p.batt_pct, (int)rssi, snr,
    (unsigned)p.uptime_s, (unsigned long)missed, (unsigned long)millis());
  Serial.println(json);
  cloud.putNode(h.src, json);
}

static void emitEvent(uint8_t node, const char* ev) {
  char json[100];
  snprintf(json, sizeof(json),
    "{\"t\":\"event\",\"node\":%u,\"event\":\"%s\",\"gw_ms\":%lu}",
    (unsigned)node, ev, (unsigned long)millis());
  Serial.println(json);
  cloud.postEvent(json);
}

static void emitSecurity(const char* ev, uint32_t count) {
  char json[120];
  snprintf(json, sizeof(json),
    "{\"t\":\"security\",\"event\":\"%s\",\"count\":%lu,\"gw_ms\":%lu}",
    ev, (unsigned long)count, (unsigned long)millis());
  Serial.println(json);
  cloud.postEvent(json);
}

// ---------------------------------------------------------------------------
static void handlePacket(int len) {
  uint8_t buf[PN_FRAME_LEN];

  if (len != PN_FRAME_LEN) {
    while (LoRa.available()) LoRa.read();
    s_badSize++;
    return;
  }
  for (int i = 0; i < len; i++) {
    if (!LoRa.available()) return;
    buf[i] = (uint8_t)LoRa.read();
  }

  const int16_t rssi = (int16_t)LoRa.packetRssi();
  const float   snr  = LoRa.packetSnr();

  pn_hdr_t     h;
  pn_payload_t p;
  uint8_t      ttl, hops;

  // Authentication and decryption are one step. A packet whose tag does not
  // verify was either corrupted in flight or forged by someone without the
  // key - either way there is nothing usable inside it.
  if (!pn_frame_open(buf, len, &h, &p, &ttl, &hops)) {
    s_badAuth++;
    emitSecurity("auth_failed", s_badAuth);
    return;
  }

  // A valid packet can still be a REPLAY - a recording of a real one, sent
  // again. The tag verifies because it is genuine. What catches it is the
  // (boot, msg) pair being one we have already accepted.
  const uint32_t before = pn_replay_rejected();
  if (!pn_replay_ok(h.src, h.boot, h.msg)) {
    emitSecurity("replay_rejected", pn_replay_rejected());
    return;
  }
  (void)before;

  s_totalRx++;

  NodeRec* n = findOrAdd(h.src);
  uint32_t missedTotal = 0;

  if (n) {
    // A jump in the message counter means packets we never heard. This is
    // the gateway's own measure of link quality, independent of RSSI.
    if (n->packets > 0 && h.boot == n->lastBoot && h.msg > n->lastMsg + 1)
      n->missed += (h.msg - n->lastMsg - 1);

    if (h.boot != n->lastBoot && n->packets > 0)
      Serial.printf("# GW  node %u restarted (boot %u -> %u)\n",
                    (unsigned)h.src, (unsigned)n->lastBoot, (unsigned)h.boot);

    if (!n->alive) { n->alive = true; emitEvent(h.src, "online"); }

    n->lastBoot = h.boot;
    n->lastMsg  = h.msg;
    n->lastMs   = millis();
    n->rssi     = rssi;
    n->packets++;
    missedTotal = n->missed;
  }

  emitData(h, p, hops, rssi, snr, missedTotal);
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println();
  Serial.println(F("# ==================================================="));
  Serial.println(F("#  PHANTOM NET - GATEWAY  (AES-128-GCM)"));
  Serial.printf ("#  listening on %.3f MHz  SF%d  sync 0x%02X\n",
                 LORA_FREQ / 1e6, LORA_SF, LORA_SYNCWORD);
  Serial.printf ("#  frame %u bytes: %u header + %u mutable + %u ciphertext "
                 "+ %u tag\n", (unsigned)PN_FRAME_LEN, (unsigned)PN_HDR_LEN,
                 (unsigned)PN_MUT_LEN, (unsigned)PN_PLAIN_LEN,
                 (unsigned)PN_TAG_LEN);
  Serial.println(F("#  JSON lines start with { - everything else is comment"));
  Serial.println(F("# ==================================================="));

  pn_crypto_begin();

  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);
  LoRa.setPins(PIN_LORA_CS, PIN_LORA_RST, PIN_LORA_DIO0);

  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println(F("# GATEWAY: LoRa init FAILED"));
    Serial.println(F("#   VCC must be 3.3 V - 5 V destroys the Ra-02"));
    Serial.println(F("#   SCK=18 MISO=19 MOSI=23 NSS=5 RST=27 DIO0=26"));
    Serial.println(F("#   antenna MUST be attached before powering up"));
    while (true) delay(1000);
  }

  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setPreambleLength(8);
  LoRa.setSyncWord(LORA_SYNCWORD);
  LoRa.setTxPower(LORA_TXPOWER, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.enableCrc();
  LoRa.receive();             // polled, no callbacks - see pn_lora.cpp notes

  Serial.println(F("# GATEWAY: radio up, waiting for nodes"));

  // WiFi/cloud comes up AFTER the radio, and its failure is never fatal -
  // LoRa reception works identically whether or not the cloud link is up.
  Serial.printf("# Connecting to WiFi \"%s\"...\n", WIFI_SSID);
  if (cloud.begin()) {
    Serial.printf("# WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println(F("# WiFi connect failed - will keep retrying in the "
                      "background; LoRa reception is unaffected either way"));
  }

  char rdy[100];
  snprintf(rdy, sizeof(rdy),
    "{\"t\":\"gateway\",\"event\":\"ready\",\"id\":%u,\"enc\":\"AES-128-GCM\"}",
    (unsigned)GATEWAY_ID);
  Serial.println(rdy);
  cloud.putGateway(rdy);
}

// ---------------------------------------------------------------------------
void loop() {
  const int sz = LoRa.parsePacket();
  if (sz > 0) handlePacket(sz);

  const uint32_t now = millis();
  for (uint8_t i = 0; i < s_nodeCount; i++) {
    if (s_nodes[i].alive && (now - s_nodes[i].lastMs > NODE_STALE_MS)) {
      s_nodes[i].alive = false;
      emitEvent(s_nodes[i].id, "offline");
    }
  }

  static uint32_t lastSummary = 0;
  if (now - lastSummary > 15000) {
    lastSummary = now;
    Serial.printf("# GW  rx=%lu  authfail=%lu  replay=%lu  badsize=%lu  nodes=%u |",
                  (unsigned long)s_totalRx, (unsigned long)s_badAuth,
                  (unsigned long)pn_replay_rejected(), (unsigned long)s_badSize,
                  (unsigned)s_nodeCount);
    for (uint8_t i = 0; i < s_nodeCount; i++) {
      Serial.printf("  N%02u %s %ddBm pkts=%lu missed=%lu",
                    (unsigned)s_nodes[i].id,
                    s_nodes[i].alive ? "UP  " : "DOWN",
                    (int)s_nodes[i].rssi,
                    (unsigned long)s_nodes[i].packets,
                    (unsigned long)s_nodes[i].missed);
    }
    Serial.println();
  }

  cloud.update();   // reconnects WiFi in the background only - never blocks
                     // for long, never touches the radio; see CloudBridge.h

  delay(2);
}