// ============================================================================
//  PHANTOM NET — CloudBridge.h
//  WiFi + Supabase over plain HTTPS REST calls (PostgREST, which every
//  Supabase project exposes automatically). No extra library beyond what
//  the ESP32 Arduino core already ships: WiFi.h, WiFiClientSecure.h,
//  HTTPClient.h. Nothing to install via Library Manager for this file.
//
//  WHY THIS IS SYNCHRONOUS - NO INTERRUPTS, JUST A BOUNDED BLOCKING CALL
//  ------------------------------------------------------------------------
//  pn_lora.cpp already documents why LoRa.onReceive() is off the table for
//  this project: it fires from the DIO0 interrupt, that handler has to do
//  SPI reads to drain the radio, and on ESP32 that can collide with SPI
//  already in progress elsewhere and lock the board solid - silently, with
//  no crash message. That is exactly the failure mode an interrupt-driven
//  fix for "don't let a slow cloud push delay LoRa reception" would risk
//  reintroducing, so it is deliberately not used here either.
//
//  Instead: each push is a plain, synchronous HTTPS call, bounded by
//  CLOUD_HTTP_TIMEOUT_MS (2 s default) so a bad network cannot hang loop()
//  indefinitely. This is the same trade-off pn_lora.cpp already made for
//  the node's own ~200 ms transmit-blocking window, applied consistently:
//  a short, bounded, easy-to-reason-about block, in exchange for never
//  touching interrupts. A packet arriving from a second node in the same
//  instant a push is in flight could in principle be missed - if that ever
//  matters in practice, the robust fix is a second FreeRTOS task pinned to
//  the ESP32's other core doing the network I/O, communicating over a
//  thread-safe queue (no interrupts either way) - flagged here as a known,
//  deliberately deferred next step, not implemented now.
//
//  Tables written (see CloudConfig.h for the SQL that creates them):
//    POST /gateway_status  (Prefer: resolution=merge-duplicates) - upserted
//                           on id=1, so there is always exactly one row
//    POST /nodes            (Prefer: resolution=merge-duplicates) - upserted
//                           on node_id; this is "current state", not a
//                           log, so the latest write is all that matters
//    POST /events            plain insert - Postgres' bigserial id makes
//                           this an append-only, naturally ordered log the
//                           dashboard subscribes to via Supabase Realtime
//
//  TLS note: WiFiClientSecure::setInsecure() encrypts the connection but
//  does not verify Supabase's certificate chain. Standard shortcut for an
//  ESP32 talking to a fixed, trusted endpoint - keeping a root CA bundle
//  current on a microcontroller is its own maintenance burden. Adequate
//  here; not something to carry into a product that must resist an
//  on-path attacker.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "CloudConfig.h"

class CloudBridge {
public:
  bool begin() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) delay(250);
    return WiFi.status() == WL_CONNECTED;
  }

  bool connected() const { return WiFi.status() == WL_CONNECTED; }

  // call once every loop() - reconnects WiFi in the background if it ever
  // drops, without ever touching the LoRa side, which keeps working the
  // whole time regardless of whether the cloud link is currently up
  void update() {
    if (connected()) return;
    const uint32_t now = millis();
    if (now - _lastWifiTry < 10000) return;
    _lastWifiTry = now;
    WiFi.reconnect();
  }

  bool putGateway(const char* json) {
    return upsert("/gateway_status?on_conflict=id",
                   String("{\"id\":1,\"payload\":") + json + "}");
  }
  bool putNode(uint8_t node, const char* json) {
    return upsert("/nodes?on_conflict=node_id",
                   String("{\"node_id\":") + node + ",\"payload\":" + json + "}");
  }
  bool postEvent(const char* json) {
    return insert("/events", String("{\"payload\":") + json + "}");
  }

private:
  uint32_t _lastWifiTry = 0;

  String url(const char* path) const {
    return String("https://") + SUPABASE_URL + "/rest/v1" + path;
  }
  void authHeaders(HTTPClient& http) const {
    http.addHeader("apikey", SUPABASE_SERVICE_KEY);
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_SERVICE_KEY);
    http.addHeader("Content-Type", "application/json");
  }

  bool upsert(const char* path, const String& body) {
    if (!connected()) return false;
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http;
    http.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    if (!http.begin(client, url(path))) return false;
    authHeaders(http);
    http.addHeader("Prefer", "resolution=merge-duplicates,return=minimal");
    const int code = http.POST(body);
    http.end();
    return code > 0 && code < 300;
  }
  bool insert(const char* path, const String& body) {
    if (!connected()) return false;
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http;
    http.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    if (!http.begin(client, url(path))) return false;
    authHeaders(http);
    http.addHeader("Prefer", "return=minimal");
    const int code = http.POST(body);
    http.end();
    return code > 0 && code < 300;
  }
};
