#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// 上游 Wi-Fi 中继（AP + STA + NAPT）
// ═══════════════════════════════════════════════════════════════════════════
//
// 现象：手机连 P4 热点（192.168.4.1）后就没网了 —— 因为 P4 只是个 AP。
//
// 做法：把 Wi-Fi 切到 AP+STA，STA 连家里的路由器，再用 lwIP 的 NAPT
// （IP 转发 + 端口映射）把 AP 侧客户端的流量转出去。
//
//   · 本机编译用的 sdkconfig 里 CONFIG_LWIP_IP_FORWARD / IPV4_NAPT 都是 y，
//     所以 esp_netif_napt_enable() 可以直接用；
//   · AP 的 DHCP 要把 DNS 改成上游的 DNS，否则客户端「有 IP 但解析不了域名」，
//     看起来还是没网 —— 这一步最容易漏；
//   · 连不上时不阻塞：WiFi.begin() 只是发起，真正的状态由 p4UpLoop() 每秒轮询，
//     失败每 15 s 重试一次，不重启任何总线。
//
// 注意：不要调 WiFi.disconnect(true) / WiFi.mode(WIFI_OFF)。本机 Wi-Fi 是
// esp-hosted（P4 + C6），关掉 Wi-Fi 会把 Hosted 传输一起拆掉，页面上就再也
// 连不回来了。
// ═══════════════════════════════════════════════════════════════════════════

#include <esp_netif.h>
#include <Preferences.h>
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"

#define P4_UP_NS "wifi"
#define P4_UP_SSID "up_ssid"
#define P4_UP_PASS "up_pass"
#define P4_UP_ON "up_on"
#define P4_UP_RETRY_MS 15000u

static String p4UpSsid;
static String p4UpPass;
static bool p4UpEnabled = false;      // 用户打开过中继
static bool p4UpHaveCfg = false;
static bool p4UpPending = false;      // 需要重新发起连接
static bool p4UpConnected = false;
static bool p4UpNapt = false;
static bool p4UpDnsOk = false;
static uint32_t p4UpLastTryMs = 0;
static uint32_t p4UpAttempts = 0;
static uint32_t p4UpConnectedMs = 0;
static uint32_t p4UpLastApEpoch = 0;
static char p4UpLastErr[40] = "";
static char p4UpApDnsStr[20] = "223.5.5.5";

static bool p4UplinkWanted() { return p4UpEnabled && p4UpHaveCfg; }

static void p4UpLoad() {
  Preferences p;
  if (!p.begin(P4_UP_NS, true)) return;
  p4UpSsid = p.getString(P4_UP_SSID, "");
  p4UpPass = p.getString(P4_UP_PASS, "");
  p4UpEnabled = p.getBool(P4_UP_ON, false);
  p.end();
  p4UpHaveCfg = p4UpSsid.length() > 0;
  if (p4UplinkWanted()) p4UpPending = true;
}

static bool p4UpSave() {
  Preferences p;
  if (!p.begin(P4_UP_NS, false)) return false;
  const bool ok = p.putString(P4_UP_SSID, p4UpSsid) == p4UpSsid.length() &&
                  p.putString(P4_UP_PASS, p4UpPass) == p4UpPass.length() &&
                  p.putBool(P4_UP_ON, p4UpEnabled);
  p.end();
  return ok;
}

// 打开 / 更新上游：只存配置并把连接排进队列，真正 begin() 在 loop 里做，
// 这样 HTTP 请求不会被扫描+关联的几秒钟卡住。
static bool p4UpApply(const String &ssid, const String &pass) {
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 63) return false;
  p4UpSsid = ssid;
  p4UpPass = pass;
  p4UpHaveCfg = true;
  p4UpEnabled = true;
  p4UpPending = true;
  p4UpConnectedMs = 0;
  p4UpLastErr[0] = '\0';
  return p4UpSave();
}

// 关闭中继：先关闭 AP 转发，再断开 STA 退回纯 AP。不动 AP 本身，页面不会掉线。
static void p4UpDisable() {
  p4UpEnabled = false;
  p4UpPending = false;
  p4UpConnected = false;
  p4UpNapt = false;
  p4UpDnsOk = false;
  p4UpConnectedMs = 0;
  p4UpLastErr[0] = '\0';
  p4UpSave();
  WiFi.AP.enableNAPT(false);
  WiFi.disconnect(false, false);
  if (WiFi.getMode() != WIFI_AP) WiFi.mode(WIFI_AP);
  p4WifiApplyRadioSettings();
}

// 必须同时写入 AP 的 netif DNS 并将 ESP_NETIF_DOMAIN_NAME_SERVER 设成 OFFER_DNS 位掩码！
// 之前直接把 IPv4 字节当 dhcps_offer_t 传入导致 DHCP 停止下发 DNS（电脑连接无网络）。
static bool p4UpSyncDns() {
  esp_netif_t *sta = WiFi.STA.netif();
  esp_netif_t *ap = WiFi.AP.netif();
  if (!sta || !ap) return false;

  esp_netif_dns_info_t dns = {};
  const bool staDnsValid = (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) &&
                           dns.ip.type == ESP_IPADDR_TYPE_V4 &&
                           dns.ip.u_addr.ip4.addr != 0 &&
                           dns.ip.u_addr.ip4.addr != (uint32_t)IPAddress(192, 168, 4, 1);
  if (!staDnsValid) {
    dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = (uint32_t)IPAddress(223, 5, 5, 5);
  }
  esp_netif_dns_info_t backup = {};
  backup.ip.type = ESP_IPADDR_TYPE_V4;
  backup.ip.u_addr.ip4.addr = (dns.ip.u_addr.ip4.addr == (uint32_t)IPAddress(223, 5, 5, 5))
                                  ? (uint32_t)IPAddress(8, 8, 8, 8)
                                  : (uint32_t)IPAddress(223, 5, 5, 5);

  const uint8_t *b = (const uint8_t *)&dns.ip.u_addr.ip4.addr;
  snprintf(p4UpApDnsStr, sizeof(p4UpApDnsStr), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);

  (void)esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
  (void)esp_netif_set_dns_info(ap, ESP_NETIF_DNS_BACKUP, &backup);

  const esp_err_t stopErr = esp_netif_dhcps_stop(ap);
  dhcps_offer_t offer = OFFER_DNS;
  const esp_err_t optErr = esp_netif_dhcps_option(
      ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
  const esp_err_t startErr = esp_netif_dhcps_start(ap);
  return (stopErr == ESP_OK || stopErr == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) &&
         optErr == ESP_OK &&
         (startErr == ESP_OK || startErr == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED);
}

static bool p4UpCheckSubnetConflict() {
  const uint32_t staNet = (uint32_t)WiFi.localIP() & (uint32_t)WiFi.subnetMask();
  const uint32_t apMask = (uint32_t)IPAddress(255, 255, 255, 0);
  const uint32_t apNet = (uint32_t)WiFi.softAPIP() & apMask;
  if (staNet != 0 && staNet == apNet) {
    snprintf(p4UpLastErr, sizeof(p4UpLastErr), "subnet-conflict");
    return true;
  }
  return false;
}

static bool p4UpEnableNapt() {
  esp_netif_t *sta = WiFi.STA.netif();
  esp_netif_t *ap = WiFi.AP.netif();
  if (!sta || !ap) return false;
  (void)p4UpCheckSubnetConflict();
  // 将上游 STA 设为 lwIP 默认路由出口（ESP-Hosted AP->STA 转发依赖 netif_default）。
  (void)WiFi.STA.setDefault();
  if (WiFi.AP.enableNAPT(true)) return true;
  return esp_netif_napt_enable(ap) == ESP_OK;
}

static String p4UpJsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 4);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if ((uint8_t)c >= 0x20u) out += c;
  }
  return out;
}

static bool p4UpEnsureStaScanMode() {
  if (!p4NetworkBeginWifi()) return false;
  const wifi_mode_t m = WiFi.getMode();
  if (m != WIFI_AP_STA) {
    if (!WiFi.mode(WIFI_AP_STA)) {
      snprintf(p4UpLastErr, sizeof(p4UpLastErr), "mode-failed");
      return false;
    }
    p4WifiApplyRadioSettings();
  }
  return true;
}

static void p4UpBeginSta() {
  if (p4UpPass.length() > 0) WiFi.begin(p4UpSsid.c_str(), p4UpPass.c_str());
  else WiFi.begin(p4UpSsid.c_str());
}

// 异步扫描附近 Wi-Fi（不阻塞 Web / CAN 任务）。
static String p4UpScanJson() {
  if (!p4UpEnsureStaScanMode()) {
    return "{\"ok\":false,\"error\":\"wifi-mode-failed\",\"scanning\":false,\"networks\":[]}";
  }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    // 尚未开始或上次已清理：启动一次异步扫描
    const int started = WiFi.scanNetworks(true, false);
    if (started == WIFI_SCAN_RUNNING) {
      return "{\"ok\":true,\"scanning\":true,\"networks\":[]}";
    }
    n = started;
  }
  if (n == WIFI_SCAN_RUNNING) {
    return "{\"ok\":true,\"scanning\":true,\"networks\":[]}";
  }
  if (n < 0) {
    return "{\"ok\":false,\"error\":\"scan-failed\",\"scanning\":false,\"networks\":[]}";
  }

  // 按信号强度降序、按 SSID 去重（保留信号最强条目）
  const int count = n;
  int idx[32];
  int kept = 0;
  for (int i = 0; i < count; ++i) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (int k = 0; k < kept; ++k) {
      if (WiFi.SSID(idx[k]) == s) {
        if (WiFi.RSSI(i) > WiFi.RSSI(idx[k])) idx[k] = i;
        dup = true;
        break;
      }
    }
    if (!dup && kept < 32) idx[kept++] = i;
  }
  for (int i = 0; i < kept; ++i) {
    for (int j = i + 1; j < kept; ++j) {
      if (WiFi.RSSI(idx[j]) > WiFi.RSSI(idx[i])) {
        const int tmp = idx[i];
        idx[i] = idx[j];
        idx[j] = tmp;
      }
    }
  }

  String j = "{\"ok\":true,\"scanning\":false,\"networks\":[";
  for (int k = 0; k < kept; ++k) {
    const int i = idx[k];
    if (k > 0) j += ",";
    const bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    const int ch = (int)WiFi.channel(i);
    j += "{\"ssid\":\"";
    j += p4UpJsonEscape(WiFi.SSID(i));
    j += "\",\"rssi\":";
    j += String((int)WiFi.RSSI(i));
    j += ",\"ch\":";
    j += String(ch);
    j += ",\"band\":\"";
    j += (ch > 14) ? "5G" : "2.4G";
    j += "\",\"open\":";
    j += open ? "true" : "false";
    j += "}";
  }
  j += "]}";
  WiFi.scanDelete();
  return j;
}

// 每秒调一次（loop()）。
static void p4UpLoop() {
  const uint32_t now = (uint32_t)millis();

  if (!p4UplinkWanted()) {
    p4UpConnected = false;
    return;
  }

  if (p4UpPending) {
    p4UpPending = false;
    p4UpAttempts++;
    p4UpLastTryMs = now;
    if (WiFi.getMode() != WIFI_AP_STA) {
      if (!WiFi.mode(WIFI_AP_STA)) {
        snprintf(p4UpLastErr, sizeof(p4UpLastErr), "mode-failed");
        return;
      }
      p4WifiApplyRadioSettings();
    }
    p4UpBeginSta();
    return;
  }

  const bool linked = (WiFi.status() == WL_CONNECTED);
  if (linked) {
    const bool apRestarted = (p4NetworkApConfigEpoch != p4UpLastApEpoch);
    if (!p4UpConnected || apRestarted) {
      p4UpConnected = true;
      p4UpConnectedMs = apRestarted && p4UpConnectedMs ? p4UpConnectedMs : now;
      p4UpLastApEpoch = p4NetworkApConfigEpoch;
      p4UpLastErr[0] = '\0';
      p4UpDnsOk = p4UpSyncDns();
      p4UpNapt = p4UpEnableNapt();
    } else if (!p4UpNapt || !p4UpDnsOk || !WiFi.STA.isDefault()) {
      if (!p4UpDnsOk) p4UpDnsOk = p4UpSyncDns();
      p4UpNapt = p4UpEnableNapt();
    }
    return;
  }

  p4UpConnected = false;
  p4UpNapt = false;
  p4UpDnsOk = false;
  if ((uint32_t)(now - p4UpLastTryMs) >= P4_UP_RETRY_MS) {
    p4UpAttempts++;
    p4UpLastTryMs = now;
    p4UpBeginSta();
  }
}

static String p4UpStatusJson() {
  String j = "{";
  j += "\"enabled\":"; j += p4UpEnabled ? "true" : "false";
  j += ",\"haveCfg\":"; j += p4UpHaveCfg ? "true" : "false";
  j += ",\"ssid\":\""; j += p4UpJsonEscape(p4UpSsid); j += "\"";
  j += ",\"connected\":"; j += p4UpConnected ? "true" : "false";
  j += ",\"napt\":"; j += p4UpNapt ? "true" : "false";
  j += ",\"dnsOk\":"; j += p4UpDnsOk ? "true" : "false";
  j += ",\"apDns\":\""; j += p4UpApDnsStr; j += "\"";
  j += ",\"ip\":\""; j += p4UpConnected ? WiFi.localIP().toString() : String(""); j += "\"";
  j += ",\"gw\":\""; j += p4UpConnected ? WiFi.gatewayIP().toString() : String(""); j += "\"";
  j += ",\"dns\":\""; j += p4UpConnected ? WiFi.dnsIP(0).toString() : String(""); j += "\"";
  j += ",\"rssi\":"; j += p4UpConnected ? String((int)WiFi.RSSI()) : String("0");
  j += ",\"attempts\":"; j += (unsigned long)p4UpAttempts;
  j += ",\"upSec\":"; j += p4UpConnected ? (unsigned long)((millis() - p4UpConnectedMs) / 1000UL) : 0UL;
  j += ",\"stations\":"; j += String((int)WiFi.softAPgetStationNum());
  j += ",\"error\":\""; j += p4UpLastErr; j += "\"";
  j += "}";
  return j;
}
