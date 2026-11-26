// ============================================================================
//  PHANTOM NET — pn_crypto.cpp
//  AES-128-GCM using mbedtls, which ships inside the ESP32 Arduino core.
//  No library to install.
//
//  WHY GCM AND NOT PLAIN AES
//  -------------------------
//  Plain AES gives secrecy but no integrity: an attacker can flip bits in
//  the ciphertext and you would decrypt the corrupted result without ever
//  knowing. GCM produces an authentication tag over both the ciphertext and
//  the readable header, so any tampering makes decryption fail outright.
//  For a safety device, silently accepting a modified alarm packet would be
//  far worse than dropping it.
//
//  THE NONCE MUST NEVER REPEAT
//  ---------------------------
//  Reusing a nonce with the same key breaks GCM completely - it leaks the
//  XOR of the two plaintexts and lets an attacker forge tags. Ours is built
//  from (src, boot, msg):
//     src   differs between nodes
//     boot  increments every power cycle, stored in flash
//     msg   increments on every packet within a boot
//  so the same nonce cannot occur twice unless flash is wiped.
// ============================================================================

#include "pn_crypto.h"
#include "mbedtls/gcm.h"
#include <Preferences.h>
#include <string.h>

// ---------------------------------------------------------------------------
//  SHARED KEY
//
//  All nodes and the gateway share one 128-bit key. This is a pre-shared-key
//  design, chosen because the nodes have no way to run a key exchange over a
//  113 ms LoRa packet and no certificate infrastructure to trust.
//
//  BE HONEST ABOUT THIS IN THE REPORT: a single shared key means compromising
//  one node compromises the whole network. Per-node keys with a gateway-held
//  table would fix that, and it is the right next step - but it is a key
//  MANAGEMENT problem, not a cryptography one.
// ---------------------------------------------------------------------------
static const uint8_t PN_KEY[16] = {
  0x50, 0x68, 0x61, 0x6E, 0x74, 0x6F, 0x6D, 0x4E,
  0x65, 0x74, 0x2D, 0x44, 0x65, 0x6D, 0x6F, 0x31
};

static uint16_t s_boot = 0;

// ---------------------------------------------------------------------------
//  BOOT COUNTER
// ---------------------------------------------------------------------------
void pn_crypto_begin() {
  Preferences prefs;
  prefs.begin("phantomnet", false);
  s_boot = prefs.getUShort("boot", 0) + 1;
  prefs.putUShort("boot", s_boot);
  prefs.end();
  Serial.printf("# CRYPTO: AES-128-GCM ready, boot #%u, frame %u bytes\n",
                (unsigned)s_boot, (unsigned)PN_FRAME_LEN);
}

uint16_t pn_boot_count() { return s_boot; }

// ---------------------------------------------------------------------------
static void buildNonce(uint8_t src, uint16_t boot, uint16_t msg,
                       uint8_t nonce[PN_NONCE_LEN]) {
  memset(nonce, 0, PN_NONCE_LEN);
  nonce[0] = src;
  nonce[2] = (uint8_t)(boot & 0xFF);
  nonce[3] = (uint8_t)(boot >> 8);
  nonce[4] = (uint8_t)(msg  & 0xFF);
  nonce[5] = (uint8_t)(msg  >> 8);
}

// ---------------------------------------------------------------------------
//  BUILD
// ---------------------------------------------------------------------------
uint8_t pn_frame_build(uint8_t* buf, uint8_t src, uint8_t dst, uint8_t flags,
                       uint16_t msg, uint8_t ttl, const pn_payload_t* pl) {
  pn_hdr_t h;
  h.magic   = PN_MAGIC;
  h.version = PN_VERSION;
  h.src     = src;
  h.dst     = dst;
  h.type    = PN_TYPE_DATA;
  h.flags   = flags;
  h.boot    = s_boot;
  h.msg     = msg;
  h.plen    = PN_PLAIN_LEN;

  memcpy(buf, &h, PN_HDR_LEN);
  buf[PN_HDR_LEN]     = ttl;
  buf[PN_HDR_LEN + 1] = 0;                 // hops starts at zero

  uint8_t nonce[PN_NONCE_LEN];
  buildNonce(src, s_boot, msg, nonce);

  uint8_t tag[16];
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);

  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, PN_KEY, 128);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(
           &ctx, MBEDTLS_GCM_ENCRYPT, PN_PLAIN_LEN,
           nonce, PN_NONCE_LEN,
           (const uint8_t*)&h, PN_HDR_LEN,        // header as AAD
           (const uint8_t*)pl,                     // plaintext in
           buf + PN_HDR_LEN + PN_MUT_LEN,          // ciphertext out
           16, tag);
  }
  mbedtls_gcm_free(&ctx);
  if (rc != 0) return 0;

  // Truncate the 16-byte tag to 8. That still leaves a 1-in-2^64 chance of
  // a forged packet passing - ample here, and it saves 8 bytes of airtime
  // on every single transmission.
  memcpy(buf + PN_HDR_LEN + PN_MUT_LEN + PN_PLAIN_LEN, tag, PN_TAG_LEN);
  return PN_FRAME_LEN;
}

// ---------------------------------------------------------------------------
//  OPEN
// ---------------------------------------------------------------------------
bool pn_frame_open(const uint8_t* buf, int len, pn_hdr_t* hdr,
                   pn_payload_t* pl, uint8_t* ttl, uint8_t* hops) {
  if (len != PN_FRAME_LEN) return false;

  memcpy(hdr, buf, PN_HDR_LEN);
  if (hdr->magic != PN_MAGIC)     return false;
  if (hdr->version != PN_VERSION) return false;
  if (hdr->plen != PN_PLAIN_LEN)  return false;

  if (ttl)  *ttl  = buf[PN_HDR_LEN];
  if (hops) *hops = buf[PN_HDR_LEN + 1];

  uint8_t nonce[PN_NONCE_LEN];
  buildNonce(hdr->src, hdr->boot, hdr->msg, nonce);

  // The tag we received is 8 bytes; mbedtls wants to compare that length.
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, PN_KEY, 128);
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(
           &ctx, PN_PLAIN_LEN,
           nonce, PN_NONCE_LEN,
           buf, PN_HDR_LEN,                        // AAD = the header as sent
           buf + PN_HDR_LEN + PN_MUT_LEN + PN_PLAIN_LEN, PN_TAG_LEN,
           buf + PN_HDR_LEN + PN_MUT_LEN,          // ciphertext in
           (uint8_t*)pl);                          // plaintext out
  }
  mbedtls_gcm_free(&ctx);

  // rc != 0 means the tag did not verify: corrupted in flight, or forged.
  // Either way the contents are worthless and we return nothing at all.
  return rc == 0;
}

// ---------------------------------------------------------------------------
//  REPLAY REJECTION
//
//  Encryption alone does not stop an attacker recording a valid packet and
//  transmitting it again later - the tag still verifies, because it IS a
//  genuine packet. What stops it is remembering how far each node has got
//  and refusing anything that is not strictly newer.
// ---------------------------------------------------------------------------
#define REPLAY_SLOTS 8

static struct { uint8_t src; uint16_t boot; uint16_t msg; bool used; }
       s_seen[REPLAY_SLOTS];
static uint32_t s_rejected = 0;

uint32_t pn_replay_rejected() { return s_rejected; }

void pn_replay_clear() {
  memset(s_seen, 0, sizeof(s_seen));
  s_rejected = 0;
}

bool pn_replay_ok(uint8_t src, uint16_t boot, uint16_t msg) {
  int slot = -1, free_slot = -1;
  for (int i = 0; i < REPLAY_SLOTS; i++) {
    if (s_seen[i].used && s_seen[i].src == src) { slot = i; break; }
    if (!s_seen[i].used && free_slot < 0) free_slot = i;
  }

  if (slot < 0) {                       // first packet ever from this node
    if (free_slot < 0) return true;     // table full: fail open, do not lock out
    s_seen[free_slot] = { src, boot, msg, true };
    return true;
  }

  // Newer boot always wins - the node power-cycled and msg restarted at 1.
  if (boot > s_seen[slot].boot ||
      (boot == s_seen[slot].boot && msg > s_seen[slot].msg)) {
    s_seen[slot].boot = boot;
    s_seen[slot].msg  = msg;
    return true;
  }

  s_rejected++;
  Serial.printf("# CRYPTO: REPLAY REJECTED from node %u (boot %u msg %u, "
                "already seen up to boot %u msg %u)\n",
                (unsigned)src, (unsigned)boot, (unsigned)msg,
                (unsigned)s_seen[slot].boot, (unsigned)s_seen[slot].msg);
  return false;
}

// ---------------------------------------------------------------------------
void pn_hexdump(const char* label, const uint8_t* b, size_t n) {
  Serial.printf("# %s (%u bytes):", label, (unsigned)n);
  for (size_t i = 0; i < n; i++) {
    if (i % 16 == 0) Serial.printf("\n#   ");
    Serial.printf("%02X ", b[i]);
  }
  Serial.println();
}