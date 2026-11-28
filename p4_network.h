#pragma once
#include "pin_config.h"
#ifdef ARDUINO
#include <esp_wifi.h>
#include <Preferences.h>
#endif

// The board sequence follows the supplied P4_CAN_Hosted sketch. WiFi/Hosted
// are initialized by setup(), then owned by the web task for AP changes/retries.
enum P4NetworkPhase : uint8_t {
  P4_NET_NOT_STARTED, P4_NET_PINS_FAILED, P4_NET_WIFI_FAILED,
  P4_NET_ADDRESS_FAILED, P4_NET_AP_FAILED, P4_NET_READY
};
enum P4WifiBandCfg : uint8_t {
  P4_WIFI_BAND_AUTO = 0, // 2.4G + 5G 双频自适应（C5 默认）
  P4_WIFI_BAND_2G   = 1, // 仅 2.4 GHz
  P4_WIFI_BAND_5G   = 2  // 仅 5 GHz（仅 C5）
};
enum P4WifiApIpMode : uint8_t {
  P4_WIFI_AP_IP_DEFAULT = 0, // 192.168.4.1（默认私有地址）
  P4_WIFI_AP_IP_PUBLIC  = 1  // 100.100.1.1（伪公网/CGNAT地址，兼容禁私网设备）
};
static bool p4NetworkPinsReady = false;
static bool p4NetworkWifiStarted = false;
static bool p4NetworkApStarted = false;
static P4NetworkPhase p4NetworkPhase = P4_NET_NOT_STARTED;
static uint32_t p4NetworkHostVersion[3] = {};
static uint32_t p4NetworkSlaveVersion[3] = {};

#ifdef P4CAN_HOSTED_C5
static constexpr bool p4WifiSupports5g = true;
static constexpr uint8_t p4WifiDefaultBand = P4_WIFI_BAND_AUTO;
#else
static constexpr bool p4WifiSupports5g = false;
static constexpr uint8_t p4WifiDefaultBand = P4_WIFI_BAND_2G;
#endif
static uint8_t p4WifiBandCfg = p4WifiDefaultBand;
static uint16_t p4WifiApChanCfg = 0; // 0 = 自动（C5 默认 ch36 / C6 默认 ch1）
static bool p4Wifi6Cfg = true;       // 802.11ax Wi-Fi 6
static uint8_t p4WifiApIpModeCfg = P4_WIFI_AP_IP_DEFAULT;
static int32_t p4WifiLastBandErr = 0;
static int32_t p4WifiLastProtoErr = 0;
static bool p4WifiRadioLoaded = false;
static uint32_t p4NetworkApConfigEpoch = 0;

static inline IPAddress p4WifiApIp() {
  return (p4WifiApIpModeCfg == P4_WIFI_AP_IP_PUBLIC)
             ? IPAddress(100, 100, 1, 1)
             : IPAddress(192, 168, 4, 1);
}
static inline IPAddress p4WifiApLeaseStart() {
  return (p4WifiApIpModeCfg == P4_WIFI_AP_IP_PUBLIC)
             ? IPAddress(100, 100, 1, 2)
             : IPAddress(192, 168, 4, 2);
}
static inline const char *p4WifiApIpStr() {
  return (p4WifiApIpModeCfg == P4_WIFI_AP_IP_PUBLIC) ? "100.100.1.1" : "192.168.4.1";
}

// 由 p4_uplink.h 实现：上游中继开着时 Wi-Fi 必须以 AP+STA 起，
// 否则下面的 WiFi.mode(WIFI_AP) 会把 STA 顶掉，中继永远连不上。
static bool p4UplinkWanted();

static inline bool p4WifiChannelValidForBand(uint16_t ch, uint8_t band) {
  if (ch == 0) return true;
  const bool is2g = (ch >= 1 && ch <= 13);
  const bool is5g = (ch == 36 || ch == 40 || ch == 44 || ch == 48 ||
                     ch == 149 || ch == 153 || ch == 157 || ch == 161 || ch == 165);
  if (!p4WifiSupports5g || band == P4_WIFI_BAND_2G) return is2g;
  if (band == P4_WIFI_BAND_5G) return is5g;
  return is2g || is5g;
}

static inline int p4WifiEffectiveChannel() {
  if (!p4WifiSupports5g || p4WifiBandCfg == P4_WIFI_BAND_2G) {
    return (p4WifiApChanCfg >= 1 && p4WifiApChanCfg <= 13) ? (int)p4WifiApChanCfg : 1;
  }
  if (p4WifiBandCfg == P4_WIFI_BAND_5G) {
    return (p4WifiApChanCfg >= 36) ? (int)p4WifiApChanCfg : 36;
  }
  // C5 自动双频模式：默认走 5GHz ch36（若中继连到 2.4G 路由器由 Hosted 自动协商）
  if (p4WifiApChanCfg != 0 && p4WifiChannelValidForBand(p4WifiApChanCfg, P4_WIFI_BAND_AUTO)) {
    return (int)p4WifiApChanCfg;
  }
  return 36;
}

#ifdef ARDUINO
static void p4WifiRadioLoad() {
  if (p4WifiRadioLoaded) return;
  p4WifiRadioLoaded = true;
  p4WifiBandCfg = p4WifiDefaultBand;
  p4WifiApChanCfg = 0;
  p4Wifi6Cfg = true;
  p4WifiApIpModeCfg = P4_WIFI_AP_IP_DEFAULT;
  Preferences p;
  if (p.begin("wifiap", true)) {
    uint8_t bm = p.getUChar("band_mode", p4WifiDefaultBand);
    uint16_t ch = p.getUShort("ap_chan", 0);
    bool ax = p.getBool("wifi6", true);
    uint8_t ipm = p.getUChar("ap_ip_mode", P4_WIFI_AP_IP_DEFAULT);
    p.end();
    if (!p4WifiSupports5g || bm > P4_WIFI_BAND_5G) bm = p4WifiSupports5g ? bm : P4_WIFI_BAND_2G;
    if (bm > P4_WIFI_BAND_5G) bm = p4WifiDefaultBand;
    if (!p4WifiChannelValidForBand(ch, bm)) ch = 0;
    if (ipm > P4_WIFI_AP_IP_PUBLIC) ipm = P4_WIFI_AP_IP_DEFAULT;
    p4WifiBandCfg = bm;
    p4WifiApChanCfg = ch;
    p4Wifi6Cfg = ax;
    p4WifiApIpModeCfg = ipm;
  }
}

static bool p4WifiRadioSave(uint8_t bandMode, uint16_t apChan, bool wifi6,
                            uint8_t apIpMode = p4WifiApIpModeCfg) {
  if (!p4WifiSupports5g) bandMode = P4_WIFI_BAND_2G;
  if (bandMode > P4_WIFI_BAND_5G || !p4WifiChannelValidForBand(apChan, bandMode) ||
      apIpMode > P4_WIFI_AP_IP_PUBLIC) return false;
  Preferences p;
  if (!p.begin("wifiap", false)) return false;
  const bool ok = p.putUChar("band_mode", bandMode) == 1 &&
                  p.putUShort("ap_chan", apChan) == sizeof(uint16_t) &&
                  p.putBool("wifi6", wifi6) == 1 &&
                  p.putUChar("ap_ip_mode", apIpMode) == 1;
  p.end();
  if (ok) {
    p4WifiRadioLoaded = true;
    p4WifiBandCfg = bandMode;
    p4WifiApChanCfg = apChan;
    p4Wifi6Cfg = wifi6;
    p4WifiApIpModeCfg = apIpMode;
  }
  return ok;
}

static void p4WifiApplyRadioSettings() {
  p4WifiRadioLoad();
  wifi_band_mode_t bm = WIFI_BAND_MODE_2G_ONLY;
#ifdef P4CAN_HOSTED_C5
  if (p4WifiBandCfg == P4_WIFI_BAND_AUTO) bm = WIFI_BAND_MODE_AUTO;
  else if (p4WifiBandCfg == P4_WIFI_BAND_5G) bm = WIFI_BAND_MODE_5G_ONLY;
  p4WifiLastBandErr = (int32_t)esp_wifi_set_band_mode(bm);
#else
  p4WifiLastBandErr = (int32_t)esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
#endif
  wifi_protocols_t protos = {};
  const uint16_t p2 = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N |
                      (p4Wifi6Cfg ? WIFI_PROTOCOL_11AX : 0);
  const uint16_t p5 = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC |
                      (p4Wifi6Cfg ? WIFI_PROTOCOL_11AX : 0);
  protos.ghz_2g = (bm == WIFI_BAND_MODE_5G_ONLY) ? 0 : p2;
  protos.ghz_5g = (bm == WIFI_BAND_MODE_2G_ONLY) ? 0 : p5;
  p4WifiLastProtoErr = (int32_t)esp_wifi_set_protocols(WIFI_IF_AP, &protos);
  const wifi_mode_t m = WiFi.getMode();
  if (m == WIFI_MODE_STA || m == WIFI_MODE_APSTA) {
    (void)esp_wifi_set_protocols(WIFI_IF_STA, &protos);
  }
}

static const char *p4WifiBandCfgName(uint8_t bm) {
  switch (bm) {
    case P4_WIFI_BAND_AUTO: return "auto";
    case P4_WIFI_BAND_5G: return "5g";
    default: return "2g";
  }
}

static const char *p4WifiLiveBandName() {
  wifi_band_t b = WIFI_BAND_2G;
  if (p4NetworkWifiStarted && esp_wifi_get_band(&b) == ESP_OK) {
    return (b == WIFI_BAND_5G) ? "5 GHz" : "2.4 GHz";
  }
  return (WiFi.channel() > 14) ? "5 GHz" : "2.4 GHz";
}

static const char *p4WifiLiveBandModeName() {
  wifi_band_mode_t bm = WIFI_BAND_MODE_2G_ONLY;
  if (p4NetworkWifiStarted && esp_wifi_get_band_mode(&bm) == ESP_OK) {
    if (bm == WIFI_BAND_MODE_AUTO) return "2.4G+5G Auto";
    if (bm == WIFI_BAND_MODE_5G_ONLY) return "5G Only";
    return "2.4G Only";
  }
  return p4WifiSupports5g ? "2.4G+5G Auto" : "2.4G Only";
}
#endif

static const char *p4NetworkPhaseName() {
  switch (p4NetworkPhase) {
    case P4_NET_PINS_FAILED: return "HOSTED_PINS_FAILED";
    case P4_NET_WIFI_FAILED: return "HOSTED_WIFI_FAILED";
    case P4_NET_ADDRESS_FAILED: return "AP_ADDRESS_FAILED";
    case P4_NET_AP_FAILED: return "AP_START_FAILED";
    case P4_NET_READY: return "READY";
    default: return "NOT_STARTED";
  }
}

static bool p4NetworkPrepare() {
  // Connected to C6 IO2: leave the external pull-up in control, as in the
  // reference sketch. The Hosted driver owns C6/C5 EN/reset on GPIO54.
#ifdef HOSTED_C6_BOOT
  pinMode(HOSTED_C6_BOOT, INPUT);
#endif
  p4NetworkPinsReady = hostedSetPins(HOSTED_CLK, HOSTED_CMD, HOSTED_D0,
      HOSTED_D1, HOSTED_D2, HOSTED_D3, HOSTED_RESET);
  if (!p4NetworkPinsReady) p4NetworkPhase = P4_NET_PINS_FAILED;
  return p4NetworkPinsReady;
}

static bool p4NetworkBeginWifi() {
  if (!p4NetworkPinsReady) { p4NetworkPhase = P4_NET_PINS_FAILED; return false; }
  if (p4NetworkWifiStarted) return true;
  // Do not disconnect(true)/switch WiFi off: that tears down ESP-Hosted.
#ifdef ARDUINO
  p4WifiRadioLoad();
  const wifi_mode_t wantMode = p4UplinkWanted() ? WIFI_AP_STA : WIFI_AP;
#else
  const int wantMode = p4UplinkWanted() ? WIFI_AP : WIFI_AP;
#endif
  if (!WiFi.mode(wantMode)) { p4NetworkPhase = P4_NET_WIFI_FAILED; return false; }
  p4NetworkWifiStarted = true;
#ifdef ARDUINO
  p4WifiApplyRadioSettings();
#endif
  hostedGetHostVersion(&p4NetworkHostVersion[0], &p4NetworkHostVersion[1], &p4NetworkHostVersion[2]);
  hostedGetSlaveVersion(&p4NetworkSlaveVersion[0], &p4NetworkSlaveVersion[1], &p4NetworkSlaveVersion[2]);
  return true;
}

static bool p4NetworkStartAp(const char *ssid, const char *password) {
  if (!p4NetworkBeginWifi()) return false;
#ifdef ARDUINO
  p4WifiApplyRadioSettings();
  const int ch = p4WifiEffectiveChannel();
  const IPAddress apIp = p4WifiApIp();
  const IPAddress apLease = p4WifiApLeaseStart();
  if (!WiFi.softAPConfig(apIp, apIp, IPAddress(255,255,255,0), apLease,
                         IPAddress(223,5,5,5))) {
    p4NetworkPhase = P4_NET_ADDRESS_FAILED;
    return false;
  }
  bool apOk = (ch == 1) ? WiFi.softAP(ssid, password) : WiFi.softAP(ssid, password, ch);
  if (!apOk && ch > 14) {
    // 5GHz AP fallback to 2.4GHz channel 1 so dashboard never becomes unreachable.
    (void)esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    apOk = WiFi.softAP(ssid, password, 1);
  }
  if (!apOk) { p4NetworkPhase = P4_NET_AP_FAILED; return false; }
  p4NetworkApConfigEpoch++;
#else
  if (!WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0))) {
    p4NetworkPhase = P4_NET_ADDRESS_FAILED;
    return false;
  }
  if (!WiFi.softAP(ssid, password)) { p4NetworkPhase = P4_NET_AP_FAILED; return false; }
#endif
  p4NetworkPhase = P4_NET_READY;
  // This admission gate only opens after the first successful AP start. Runtime
  // SSID changes reuse the active Hosted transport, including an active BLE host.
  __atomic_store_n(&p4NetworkApStarted, true, __ATOMIC_RELEASE);
  return true;
}

static bool p4NetworkAllowsBle() {
  return __atomic_load_n(&p4NetworkApStarted, __ATOMIC_ACQUIRE);
}
