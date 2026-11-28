#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Wi-Fi 中继 DNS 域名过滤 + 联网检测伪装（200 / 204）
// ═══════════════════════════════════════════════════════════════════════════

#include <stdint.h>
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

enum P4DnsFilterMode : uint8_t {
  P4_DNS_FILTER_OFF       = 0,
  P4_DNS_FILTER_BLACKLIST = 1,
  P4_DNS_FILTER_WHITELIST = 2
};

static inline bool p4DnsLabelCharOkPure(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
}

// 匹配域名规则（精确匹配或子域名后缀匹配，例如 apple.com 匹配 captive.apple.com）。
static inline bool p4DnsDomainMatchesRulePure(const char *host, size_t hostLen,
                                              const char *rule, size_t ruleLen) {
  if (!host || !rule || hostLen == 0u || ruleLen == 0u || hostLen < ruleLen) return false;
  if (hostLen == ruleLen) return memcmp(host, rule, ruleLen) == 0;
  return host[hostLen - ruleLen - 1u] == '.' &&
         memcmp(host + (hostLen - ruleLen), rule, ruleLen) == 0;
}

static inline bool p4DnsIsBuiltInProbeDomainPure(const char *host, size_t hostLen) {
  static const char *const kProbes[] = {
      "captive.apple.com",
      "hotspot-detect.apple.com",
      "connectivitycheck.gstatic.com",
      "connectivitycheck.android.com",
      "clients3.google.com",
      "clients4.google.com",
      "www.msftconnecttest.com",
      "www.msftncsi.com",
      "dns.msftncsi.com",
      "nmcheck.gnome.org",
      "detectportal.firefox.com",
      "connect.rom.miui.com",
      "connectivitycheck.platform.hicloud.com",
      "connectivitycheck.cbg-app.huawei.com"
  };
  for (size_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); ++i) {
    if (p4DnsDomainMatchesRulePure(host, hostLen, kProbes[i], strlen(kProbes[i]))) return true;
  }
  return false;
}

#ifdef ARDUINO

static constexpr const char *P4_DNS_NVS_NS = "wififlt";
static constexpr size_t P4_DNS_MAX_RULES = 64u;
static constexpr size_t P4_DNS_MAX_DOMAIN_LEN = 127u;
static constexpr size_t P4_DNS_MAX_TEXT_LEN = 1536u;

static portMUX_TYPE p4DnsMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t p4DnsModeCfg = P4_DNS_FILTER_OFF;
static bool p4DnsProbeSpoofCfg = false;
static String p4DnsBlacklistText;
static String p4DnsWhitelistText;
static String p4DnsBlackRules[P4_DNS_MAX_RULES];
static size_t p4DnsBlackRuleCount = 0u;
static String p4DnsWhiteRules[P4_DNS_MAX_RULES];
static size_t p4DnsWhiteRuleCount = 0u;

static volatile uint32_t p4DnsBlockedCount = 0u;
static volatile uint32_t p4DnsProbeCount = 0u;
static volatile bool p4DnsConfigLoaded = false;
static volatile bool p4DnsUplinkReady = false;
static volatile uint32_t p4DnsUpstreamIpRaw = 0u;
static TaskHandle_t p4DnsTaskHandle = nullptr;

static inline bool p4DnsFilterActive() {
  portENTER_CRITICAL(&p4DnsMux);
  const bool active = (p4DnsModeCfg != P4_DNS_FILTER_OFF) || p4DnsProbeSpoofCfg;
  portEXIT_CRITICAL(&p4DnsMux);
  return active;
}

static inline bool p4DnsProbeSpoofEnabled() {
  portENTER_CRITICAL(&p4DnsMux);
  const bool on = p4DnsProbeSpoofCfg;
  portEXIT_CRITICAL(&p4DnsMux);
  return on;
}

static String p4DnsCleanToken(String tok) {
  tok.trim();
  tok.toLowerCase();
  if (tok.startsWith("http://")) tok.remove(0, 7);
  else if (tok.startsWith("https://")) tok.remove(0, 8);
  const int slash = tok.indexOf('/');
  if (slash >= 0) tok.remove(slash);
  const int colon = tok.indexOf(':');
  if (colon >= 0) tok.remove(colon);
  while (tok.startsWith("*.")) tok.remove(0, 2);
  while (tok.startsWith(".")) tok.remove(0, 1);
  while (tok.endsWith(".")) tok.remove(tok.length() - 1);
  if (tok.length() < 3 || tok.length() > P4_DNS_MAX_DOMAIN_LEN) return String();
  bool hasDot = false;
  for (size_t i = 0; i < tok.length(); ++i) {
    const char c = tok[i];
    if (c == '.') hasDot = true;
    if (!p4DnsLabelCharOkPure(c)) return String();
  }
  return hasDot ? tok : String();
}

static size_t p4DnsParseRules(const String &raw, String outRules[], size_t maxRules, String &normalizedOut) {
  size_t count = 0u;
  normalizedOut = "";
  String tok;
  const size_t n = raw.length();
  for (size_t i = 0; i <= n; ++i) {
    const char c = (i < n) ? raw[i] : '\n';
    if (c == '\r' || c == '\n' || c == ',' || c == ';' || c == ' ' || c == '\t') {
      if (tok.length() > 0) {
        String clean = p4DnsCleanToken(tok);
        tok = "";
        if (clean.length() > 0) {
          bool dup = false;
          for (size_t k = 0; k < count; ++k) {
            if (outRules[k] == clean) { dup = true; break; }
          }
          if (!dup && count < maxRules &&
              (normalizedOut.length() + clean.length() + 2u) <= P4_DNS_MAX_TEXT_LEN) {
            outRules[count++] = clean;
            if (normalizedOut.length() > 0) normalizedOut += "\n";
            normalizedOut += clean;
          }
        }
      }
    } else {
      tok += c;
    }
  }
  return count;
}

static void p4DnsFilterLoad() {
  if (p4DnsConfigLoaded) return;
  uint8_t mode = P4_DNS_FILTER_OFF;
  bool spoof = false;
  String blRaw, wlRaw;
  Preferences p;
  if (p.begin(P4_DNS_NVS_NS, true)) {
    mode = p.getUChar("mode", P4_DNS_FILTER_OFF);
    spoof = p.getBool("spoof", false);
    blRaw = p.getString("bl", "");
    wlRaw = p.getString("wl", "");
    p.end();
  }
  if (mode > P4_DNS_FILTER_WHITELIST) mode = P4_DNS_FILTER_OFF;
  String blRules[P4_DNS_MAX_RULES], wlRules[P4_DNS_MAX_RULES], blNorm, wlNorm;
  const size_t blCnt = p4DnsParseRules(blRaw, blRules, P4_DNS_MAX_RULES, blNorm);
  const size_t wlCnt = p4DnsParseRules(wlRaw, wlRules, P4_DNS_MAX_RULES, wlNorm);

  portENTER_CRITICAL(&p4DnsMux);
  p4DnsModeCfg = mode;
  p4DnsProbeSpoofCfg = spoof;
  p4DnsBlacklistText = blNorm;
  p4DnsWhitelistText = wlNorm;
  p4DnsBlackRuleCount = blCnt;
  p4DnsWhiteRuleCount = wlCnt;
  for (size_t i = 0; i < blCnt; ++i) p4DnsBlackRules[i] = blRules[i];
  for (size_t i = 0; i < wlCnt; ++i) p4DnsWhiteRules[i] = wlRules[i];
  p4DnsConfigLoaded = true;
  portEXIT_CRITICAL(&p4DnsMux);
}

static bool p4DnsFilterSave(uint8_t mode, bool spoof, bool hasBl, const String &blRaw,
                            bool hasWl, const String &wlRaw) {
  if (mode > P4_DNS_FILTER_WHITELIST) return false;
  p4DnsFilterLoad();
  String nextBl = hasBl ? blRaw : p4DnsBlacklistText;
  String nextWl = hasWl ? wlRaw : p4DnsWhitelistText;
  String blRules[P4_DNS_MAX_RULES], wlRules[P4_DNS_MAX_RULES], blNorm, wlNorm;
  const size_t blCnt = p4DnsParseRules(nextBl, blRules, P4_DNS_MAX_RULES, blNorm);
  const size_t wlCnt = p4DnsParseRules(nextWl, wlRules, P4_DNS_MAX_RULES, wlNorm);

  Preferences p;
  if (!p.begin(P4_DNS_NVS_NS, false)) return false;
  const bool ok = p.putUChar("mode", mode) == 1 &&
                  p.putBool("spoof", spoof) == 1 &&
                  (!hasBl || p.putString("bl", blNorm) == blNorm.length()) &&
                  (!hasWl || p.putString("wl", wlNorm) == wlNorm.length());
  p.end();
  if (!ok) return false;

  portENTER_CRITICAL(&p4DnsMux);
  p4DnsModeCfg = mode;
  p4DnsProbeSpoofCfg = spoof;
  p4DnsBlacklistText = blNorm;
  p4DnsWhitelistText = wlNorm;
  p4DnsBlackRuleCount = blCnt;
  p4DnsWhiteRuleCount = wlCnt;
  for (size_t i = 0; i < blCnt; ++i) p4DnsBlackRules[i] = blRules[i];
  for (size_t i = 0; i < wlCnt; ++i) p4DnsWhiteRules[i] = wlRules[i];
  portEXIT_CRITICAL(&p4DnsMux);
  return true;
}

static String p4DnsJsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') {}
    else if ((uint8_t)c >= 0x20u) out += c;
  }
  return out;
}

static String p4DnsFilterStatusJson() {
  p4DnsFilterLoad();
  portENTER_CRITICAL(&p4DnsMux);
  const uint8_t mode = p4DnsModeCfg;
  const bool spoof = p4DnsProbeSpoofCfg;
  const size_t ruleCount = (mode == P4_DNS_FILTER_WHITELIST) ? p4DnsWhiteRuleCount : p4DnsBlackRuleCount;
  const String bl = p4DnsBlacklistText;
  const String wl = p4DnsWhitelistText;
  portEXIT_CRITICAL(&p4DnsMux);
  const bool upReady = __atomic_load_n(&p4DnsUplinkReady, __ATOMIC_ACQUIRE);
  const uint32_t blocked = __atomic_load_n(&p4DnsBlockedCount, __ATOMIC_RELAXED);
  const uint32_t probes = __atomic_load_n(&p4DnsProbeCount, __ATOMIC_RELAXED);
  const bool active = upReady && (mode != P4_DNS_FILTER_OFF || spoof);

  String j;
  j.reserve(bl.length() + wl.length() + 256);
  j += "{\"ok\":true,\"mode\":"; j += String((unsigned)mode);
  j += ",\"modeName\":\""; j += (mode == 1 ? "blacklist" : mode == 2 ? "whitelist" : "off");
  j += "\",\"probeSpoof\":"; j += (spoof ? "true" : "false");
  j += ",\"active\":"; j += (active ? "true" : "false");
  j += ",\"uplinkConnected\":"; j += (upReady ? "true" : "false");
  j += ",\"ruleCount\":"; j += String((unsigned)ruleCount);
  j += ",\"blockedCount\":"; j += String((unsigned long)blocked);
  j += ",\"probeCount\":"; j += String((unsigned long)probes);
  j += ",\"blacklist\":\""; j += p4DnsJsonEscape(bl);
  j += "\",\"whitelist\":\""; j += p4DnsJsonEscape(wl);
  j += "\"}";
  return j;
}

// ── UDP 53 DNS Proxy Task ────────────────────────────────────────────────────

struct P4DnsPendingSlot {
  bool used;
  uint16_t upId;
  uint16_t origId;
  uint32_t clientIpRaw;
  uint16_t clientPort;
  uint32_t sentMs;
};

static size_t p4DnsParseQuestion(const uint8_t *pkt, size_t len, char *qnameOut, size_t qnameCap,
                                 uint16_t &qtypeOut, uint16_t &qclassOut) {
  if (!pkt || len < 17u || qnameCap < 4u) return 0u;
  size_t pos = 12u;
  size_t outLen = 0u;
  while (pos < len) {
    const uint8_t lab = pkt[pos++];
    if (lab == 0u) break;
    if ((lab & 0xC0u) != 0u || lab > 63u || pos + lab > len) return 0u;
    if (outLen != 0u) {
      if (outLen + 1u >= qnameCap) return 0u;
      qnameOut[outLen++] = '.';
    }
    if (outLen + lab >= qnameCap) return 0u;
    for (uint8_t i = 0; i < lab; ++i) {
      char c = (char)pkt[pos++];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      qnameOut[outLen++] = c;
    }
  }
  qnameOut[outLen] = '\0';
  if (pos + 4u > len) return 0u;
  qtypeOut = (uint16_t)((pkt[pos] << 8) | pkt[pos + 1u]);
  qclassOut = (uint16_t)((pkt[pos + 2u] << 8) | pkt[pos + 3u]);
  return pos + 4u; // 紧接 Question 结束的偏移
}

static size_t p4DnsBuildLocalReply(const uint8_t *req, size_t qEnd, uint16_t qtype, uint8_t rcode,
                                   bool answerSelfIp, uint32_t selfIpRaw, uint8_t *out, size_t outCap) {
  if (!req || !out || qEnd < 12u || qEnd > outCap) return 0u;
  memcpy(out, req, qEnd);
  out[2] = (uint8_t)(0x81u | (req[2] & 0x01u)); // QR=1, AA=0, RD=req.RD
  out[3] = (uint8_t)(0x80u | (rcode & 0x0Fu)); // RA=1, RCODE
  out[4] = 0u; out[5] = 1u; // QDCOUNT = 1
  out[6] = 0u; out[7] = (answerSelfIp && rcode == 0u && qtype == 1u) ? 1u : 0u; // ANCOUNT
  out[8] = 0u; out[9] = 0u;
  out[10] = 0u; out[11] = 0u;
  if (!answerSelfIp || rcode != 0u || qtype != 1u) return qEnd;
  if (qEnd + 16u > outCap) return 0u;
  uint8_t *a = out + qEnd;
  a[0] = 0xC0u; a[1] = 0x0Cu; // NAME ptr to offset 12
  a[2] = 0x00u; a[3] = 0x01u; // TYPE A
  a[4] = 0x00u; a[5] = 0x01u; // CLASS IN
  a[6] = 0x00u; a[7] = 0x00u; a[8] = 0x00u; a[9] = 0x1Eu; // TTL 30s
  a[10] = 0x00u; a[11] = 0x04u; // RDLENGTH 4
  memcpy(a + 12, &selfIpRaw, 4u);
  return qEnd + 16u;
}

enum P4DnsDecision : uint8_t {
  P4_DNS_DECIDE_FORWARD = 0,
  P4_DNS_DECIDE_SPOOF_IP,
  P4_DNS_DECIDE_NXDOMAIN
};

static P4DnsDecision p4DnsEvaluateDomain(const char *qname, size_t len, bool &countedBlock) {
  countedBlock = false;
  if (len == 0u) return P4_DNS_DECIDE_FORWARD;
  // 本机 mDNS/局部域名放行
  if (p4DnsDomainMatchesRulePure(qname, len, "p4can.local", 11u)) {
    return P4_DNS_DECIDE_SPOOF_IP;
  }

  portENTER_CRITICAL(&p4DnsMux);
  const uint8_t mode = p4DnsModeCfg;
  const bool spoof = p4DnsProbeSpoofCfg;
  bool matchedRule = false;
  if (mode == P4_DNS_FILTER_BLACKLIST) {
    for (size_t i = 0; i < p4DnsBlackRuleCount; ++i) {
      if (p4DnsDomainMatchesRulePure(qname, len, p4DnsBlackRules[i].c_str(), p4DnsBlackRules[i].length())) {
        matchedRule = true;
        break;
      }
    }
  } else if (mode == P4_DNS_FILTER_WHITELIST) {
    for (size_t i = 0; i < p4DnsWhiteRuleCount; ++i) {
      if (p4DnsDomainMatchesRulePure(qname, len, p4DnsWhiteRules[i].c_str(), p4DnsWhiteRules[i].length())) {
        matchedRule = true;
        break;
      }
    }
  }
  portEXIT_CRITICAL(&p4DnsMux);

  const bool blocked = (mode == P4_DNS_FILTER_BLACKLIST && matchedRule) ||
                       (mode == P4_DNS_FILTER_WHITELIST && !matchedRule);
  if (blocked) {
    countedBlock = true;
    __atomic_add_fetch(&p4DnsBlockedCount, 1u, __ATOMIC_RELAXED);
    return spoof ? P4_DNS_DECIDE_SPOOF_IP : P4_DNS_DECIDE_NXDOMAIN;
  }
  if (spoof && p4DnsIsBuiltInProbeDomainPure(qname, len)) {
    return P4_DNS_DECIDE_SPOOF_IP;
  }
  return P4_DNS_DECIDE_FORWARD;
}

static void p4DnsProxyUpdateRuntime(bool uplinkReady, uint32_t upstreamDnsRaw) {
  __atomic_store_n(&p4DnsUpstreamIpRaw, upstreamDnsRaw, __ATOMIC_RELEASE);
  __atomic_store_n(&p4DnsUplinkReady, uplinkReady, __ATOMIC_RELEASE);
}

static constexpr uint16_t P4_DISCOVER_UDP_PORT = 42100u;

static void p4DnsHandleDiscoverUdp(WiFiUDP &udpDisc, bool &worked) {
  uint8_t dbuf[64];
  char jbuf[192];
  for (int iter = 0; iter < 4; ++iter) {
    const int pktLen = udpDisc.parsePacket();
    if (pktLen <= 0) break;
    worked = true;
    const int got = udpDisc.read(dbuf, sizeof(dbuf));
    const IPAddress clientIp = udpDisc.remoteIP();
    const uint16_t clientPort = udpDisc.remotePort();
    if (got < 14 || pktLen > 128 || clientPort == 0u ||
        memcmp(dbuf, "P4CAN_DISCOVER", 14) != 0) {
      continue;
    }
    const IPAddress staIp = WiFi.localIP();
    const bool staUp = (WiFi.status() == WL_CONNECTED) && ((uint32_t)staIp != 0u);
    char staStr[20] = "";
    if (staUp) {
      snprintf(staStr, sizeof(staStr), "%u.%u.%u.%u",
               (unsigned)staIp[0], (unsigned)staIp[1], (unsigned)staIp[2], (unsigned)staIp[3]);
    }
#ifdef FW_VERSION
    const char *fwStr = FW_VERSION;
#else
    const char *fwStr = "3.35.3.3";
#endif
    const int jLen = snprintf(
        jbuf, sizeof(jbuf),
        "{\"p4can\":true,\"fw\":\"%s\",\"staIp\":\"%s\",\"apIp\":\"%s\",\"uplink\":%s}",
        fwStr, staStr, p4WifiApIpStr(), staUp ? "true" : "false");
    if (jLen > 0 && (size_t)jLen < sizeof(jbuf)) {
      udpDisc.beginPacket(clientIp, clientPort);
      udpDisc.write((const uint8_t *)jbuf, (size_t)jLen);
      udpDisc.endPacket();
    }
  }
}

static void p4DnsProxyTask(void *) {
  WiFiUDP udpSrv, udpUp, udpDisc;
  bool listening = false;
  bool discListening = false;
  P4DnsPendingSlot pending[16] = {};
  uint16_t nextUpId = 0x4100u;
  uint8_t buf[520];
  uint8_t reply[1460];
  char qname[P4_DNS_MAX_DOMAIN_LEN + 2u];

  for (;;) {
    bool worked = false;

    // App 手机热点中继 UDP 发现监听（端口 42100，不受 DNS 过滤开关影响）。
    if (p4NetworkWifiStarted) {
      if (!discListening) {
        discListening = udpDisc.begin(P4_DISCOVER_UDP_PORT);
      }
      if (discListening) {
        p4DnsHandleDiscoverUdp(udpDisc, worked);
      }
    } else if (discListening) {
      udpDisc.stop();
      discListening = false;
    }

    const bool wantListen = __atomic_load_n(&p4DnsUplinkReady, __ATOMIC_ACQUIRE) && p4DnsFilterActive();
    if (!wantListen) {
      if (listening) {
        udpSrv.stop();
        udpUp.stop();
        memset(pending, 0, sizeof(pending));
        listening = false;
      }
      vTaskDelay(pdMS_TO_TICKS(worked ? 2 : (discListening ? 20 : 50)));
      continue;
    }
    if (!listening) {
      if (udpSrv.begin(53) && udpUp.begin(53053)) {
        listening = true;
      } else {
        udpSrv.stop();
        udpUp.stop();
        vTaskDelay(pdMS_TO_TICKS(worked ? 2 : 100));
        continue;
      }
    }

    for (int iter = 0; iter < 6; ++iter) {
      const int pktLen = udpSrv.parsePacket();
      if (pktLen <= 0) break;
      worked = true;
      const int got = udpSrv.read(buf, sizeof(buf));
      const IPAddress clientIp = udpSrv.remoteIP();
      const uint16_t clientPort = udpSrv.remotePort();
      if (got < 17 || (size_t)pktLen > sizeof(buf) || (buf[2] & 0x80u) != 0u) continue;
      const size_t n = (size_t)got;

      uint16_t qtype = 0u, qclass = 0u;
      const size_t qEnd = p4DnsParseQuestion(buf, n, qname, sizeof(qname), qtype, qclass);
      if (qEnd == 0u || qclass != 1u) continue;

      bool countedBlock = false;
      const P4DnsDecision decision = p4DnsEvaluateDomain(qname, strlen(qname), countedBlock);
      if (decision == P4_DNS_DECIDE_NXDOMAIN) {
        const size_t rLen = p4DnsBuildLocalReply(buf, qEnd, qtype, 3u, false, 0u, reply, sizeof(reply));
        if (rLen > 0u) {
          udpSrv.beginPacket(clientIp, clientPort);
          udpSrv.write(reply, rLen);
          udpSrv.endPacket();
        }
        continue;
      }
      if (decision == P4_DNS_DECIDE_SPOOF_IP) {
        const uint32_t selfRaw = (uint32_t)p4WifiApIp();
        const size_t rLen = p4DnsBuildLocalReply(buf, qEnd, qtype, 0u, true, selfRaw, reply, sizeof(reply));
        if (rLen > 0u) {
          udpSrv.beginPacket(clientIp, clientPort);
          udpSrv.write(reply, rLen);
          udpSrv.endPacket();
        }
        continue;
      }

      const uint32_t upRaw = __atomic_load_n(&p4DnsUpstreamIpRaw, __ATOMIC_ACQUIRE);
      if (upRaw == 0u) continue;
      const uint32_t nowMs = (uint32_t)millis();
      int slotIdx = -1;
      uint32_t oldestAge = 0u;
      int oldestIdx = 0;
      for (int k = 0; k < 16; ++k) {
        if (!pending[k].used || (uint32_t)(nowMs - pending[k].sentMs) > 4000u) {
          slotIdx = k;
          break;
        }
        const uint32_t age = (uint32_t)(nowMs - pending[k].sentMs);
        if (age >= oldestAge) { oldestAge = age; oldestIdx = k; }
      }
      if (slotIdx < 0) slotIdx = oldestIdx;
      const uint16_t origId = (uint16_t)((buf[0] << 8) | buf[1]);
      const uint16_t upId = ++nextUpId;
      pending[slotIdx] = {true, upId, origId, (uint32_t)clientIp, clientPort, nowMs};
      buf[0] = (uint8_t)(upId >> 8);
      buf[1] = (uint8_t)(upId & 0xFFu);
      udpUp.beginPacket(IPAddress(upRaw), 53u);
      udpUp.write(buf, n);
      udpUp.endPacket();
    }

    for (int iter = 0; iter < 6; ++iter) {
      const int rLen = udpUp.parsePacket();
      if (rLen <= 0) break;
      worked = true;
      const int got = udpUp.read(reply, sizeof(reply));
      if (got < 12 || (size_t)rLen > sizeof(reply)) continue;
      const size_t n = (size_t)got;
      const uint16_t upId = (uint16_t)((reply[0] << 8) | reply[1]);
      for (int k = 0; k < 16; ++k) {
        if (pending[k].used && pending[k].upId == upId) {
          reply[0] = (uint8_t)(pending[k].origId >> 8);
          reply[1] = (uint8_t)(pending[k].origId & 0xFFu);
          udpSrv.beginPacket(IPAddress(pending[k].clientIpRaw), pending[k].clientPort);
          udpSrv.write(reply, n);
          udpSrv.endPacket();
          pending[k].used = false;
          break;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(worked ? 2 : 10));
  }
}

static void p4DnsProxyStartTask() {
  p4DnsFilterLoad();
  if (!p4DnsTaskHandle) {
    xTaskCreatePinnedToCore(p4DnsProxyTask, "dnsProxy", 6144, nullptr, 1, &p4DnsTaskHandle, 0);
  }
}

#endif
