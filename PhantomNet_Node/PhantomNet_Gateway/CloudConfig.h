// ============================================================================
//  PHANTOM NET — CloudConfig.h
//  Fill in your own WiFi and Supabase details below - nothing else in the
//  gateway needs to change to bring the cloud link up.
//
//  -------------------------------------------------------------- SETUP -----
//  1. Create a free Supabase project: https://supabase.com/dashboard
//     -> New project -> (name it, pick a region, set a DB password you
//     will not need again) -> Create. Wait ~2 min for it to provision.
//  2. Project Settings (gear icon) -> Data API -> copy the "Project URL"
//     (looks like https://xxxxxxxx.supabase.co) into SUPABASE_URL below,
//     HOST ONLY - no "https://", no trailing slash.
//  3. Project Settings -> API Keys -> copy the "service_role" key (NOT the
//     "anon" key) into SUPABASE_SERVICE_KEY below. Treat this exactly like
//     a database password: it bypasses every access rule and must only
//     ever live here, in the gateway. The dashboard uses the separate
//     "anon" key instead (see its own SUPA config block near the top of
//     the "CLOUD LINK" script) - that one is safe to embed in a web page,
//     this one is not.
//  4. SQL Editor -> New query -> paste this -> Run:
//
//       create table nodes (
//         node_id    int primary key,
//         payload    jsonb not null,
//         updated_at timestamptz not null default now()
//       );
//       create table events (
//         id         bigserial primary key,
//         payload    jsonb not null,
//         created_at timestamptz not null default now()
//       );
//       create table gateway_status (
//         id         int primary key default 1,
//         payload    jsonb not null,
//         updated_at timestamptz not null default now()
//       );
//
//       alter table nodes           enable row level security;
//       alter table events          enable row level security;
//       alter table gateway_status  enable row level security;
//
//       create policy "public read nodes"           on nodes           for select using (true);
//       create policy "public read events"          on events          for select using (true);
//       create policy "public read gateway_status"  on gateway_status  for select using (true);
//
//       alter publication supabase_realtime add table nodes, events, gateway_status;
//
//     This makes live data publicly READABLE (the dashboard's anon key
//     needs no extra grant to display it) and leaves every table WRITE-
//     PROTECTED - no insert/update policy exists for the public role, so
//     only a request carrying the service_role key (only ever this
//     gateway) can post real data. Nobody can inject a fake alert.
// ============================================================================
#pragma once

// ---------------------------------------------------------------- WiFi --
// Only THIS gateway needs WiFi - the wearable nodes stay LoRa-only, on
// purpose, so their battery life and the mesh itself are completely
// unaffected by any of this; only the gateway (mains/bank powered) gained
// a network connection.
#define WIFI_SSID       "Narzo30"
#define WIFI_PASSWORD   "87654321"
#define WIFI_TIMEOUT_MS 15000

// ------------------------------------------------------------- Supabase --
#define SUPABASE_URL          "lhxxxslemoaqqmambamt.supabase.co"
#define SUPABASE_KEY "YOUR_SUPABASE_KEY"

// bounds how long a single cloud push may block loop() - see the note in
// CloudBridge.h ("WHY THIS IS SYNCHRONOUS") for why a short bounded block
// was chosen deliberately over LoRa.onReceive(), which this project has
// already tried once (see pn_lora.cpp) and found causes silent SPI lockups
// on ESP32.
#define CLOUD_HTTP_TIMEOUT_MS  2000
