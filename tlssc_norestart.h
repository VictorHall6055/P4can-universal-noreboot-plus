// ─────────────────────────────────────────────────────────────
// TLSSC 免重启（TLSSC anti-restart） —— 从 P4can-No-Reboot v3.29.3 移植
//
// 背景
// ----
// 上游 v3.29.3 有两个与 TLSSC 有关的开关：
//   1) 被封车辆（Banned Car）       —— 声明这辆车的 DAS 配置被锁成「封禁」
//   2) TLSSC 恢复（TLSSC Restore）  —— 收到车自己发的 0x331 DAS_autopilotConfig
//                                      就把 byte0 低 6 位改成 0x1B(SELF_DRIVING)，
//                                      原帧照发 + 我们再补一份改过的。
// 上游 3.31 之后整体移除了这两个开关（见上游 CHANGELOG：
// "Fully remove Banned Car/TLSSC Restore state..."），0x331 那条路也没了。
// 因此本文件把这两项**重建**出来，并把 P4can-No-Reboot 的「免重启」补发逻辑
// 合并进同一份实现：没有前者，免重启无从谈起。
//
// 要解决的问题
// ------------
// 「读-改-重发」是旁路改写，不是主动造帧。而车机（MCU）只在自己启动时读一次
// DAS 配置并锁存：那一刻总线上要是还没有被改过的 0x331，车机就锁存「封禁」，
// 必须人工重启车机让它重读 —— 于是每轮上电都要捅一次。
//
// 本模块的做法
// ------------
//   1) 把「上一次补丁过的 0x331 载荷」缓存到 NVS（每个上电周期最多写一次，
//      护 flash）。断电后仍在，下一轮上电即可用。
//   2) 每次上电 CAN B 就绪后开一个 60 s 窗口，以 10 Hz 把缓存帧补发回总线，
//      不等车先发。这样车机启动读配置的任何时刻，总线上都已经有一条
//      SELF_DRIVING 的 0x331。
//   3) 车自己开始发 0x331（2 秒内有新鲜帧）就立刻停手，回到「跟随原厂 +
//      读改重发」，不跟真帧抢总线。
//   4) 窗口到期或补到 600 帧彻底停手，不会无限期造帧。
//
// 相对 v3.29.3 的三处实现改进（对应 No-Reboot 仓库的 rebootfix 版本）
// --------------------------------------------------------------
//   A. 单次持锁：epoch 读取、TX 屏障判定、入队全部发生在一把屏障锁内，
//      与 5 ms 灯光任务 / AP 注入 / 维护模式之间不再有「读到旧 epoch 再发」
//      的窗口。
//   B. 休眠唤醒后重开窗口：检测 recovery epoch 变化、总线唤醒计数增加、
//      或 0x331 长时间静默，都视为「新一轮上电」，重新开窗补发。
//   C. Model Y L 不补发：YL 的 0x331 路由与字段语义未经实车确认，只允许
//      Standard 3/Y（Body+Chassis）使用；YL 上开关直接置灰。
//
// 边界与安全
// ----------
//   · 只在「被封车辆」+「TLSSC 恢复」都开着时才动作，少一个都直接返回。
//   · 只走 CAN B（chassis / VH），与 0x331 的原生路由一致。
//   · 过和普通注入一样的闸门：TX 屏障、管理员挂起、USB 被动、epoch。
//   · 没有缓存帧（首次使用）时什么都不发，页面显示「等待首帧」。
//   · 所有发送失败只计数，不阻断 CAN 任务，也不影响其它功能。
// ─────────────────────────────────────────────────────────────
#pragma once

#include <Preferences.h>

#define TLSS_NR_NS          "tlsscnr"   // 独立命名空间，不碰上游 features 布局
#define TLSS_NR_KEY_ON      "on"        // 免重启补发开关（跨上电保持）
#define TLSS_NR_KEY_BAN     "banned"    // 被封车辆
#define TLSS_NR_KEY_REST    "restore"   // TLSSC 恢复（0x331 读改重发）
#define TLSS_NR_KEY_HEX     "hex"       // 缓存的 0x331 载荷（hex）
#define TLSS_NR_KEY_LEN     "len"       // 缓存帧精确长度（通常 Model 3/Y 为 6 字节）

static constexpr uint32_t TLSS_NR_WINDOW_MS      = 60000u;  // 每次上电的补发窗口
static constexpr uint32_t TLSS_NR_PERIOD_MS      = 100u;    // 稳态补发周期 10 Hz
static constexpr uint32_t TLSS_NR_WAKE_PERIOD_MS = 50u;     // 唤醒前 5s 高频 20 Hz 抢占补发
static constexpr uint32_t TLSS_NR_WAKE_FAST_MS   = 5000u;
static constexpr uint16_t TLSS_NR_MAX_FRAMES     = 600u;    // 单窗口最多补发帧数
static constexpr uint32_t TLSS_NR_STOCK_FRESH_MS = 2000u;   // 车自己在发且回显成功时的让位阈值
static constexpr uint32_t TLSS_NR_IDLE_REARM_MS  = 6000u;   // 总线静默 >=6s 后恢复即视为休眠唤醒
static constexpr uint32_t TLSS_NR_STOCK_GAP_MS   = 6000u;   // 0x331 静默 >=6s 后恢复即视为车机唤醒
static constexpr uint16_t TLSS_NR_ID                = 0x331u;
static constexpr uint8_t  TLSS_NR_SELF_DRIVING_BITS  = 0x1Bu; // byte0 低 6 位

// TX 结果：把「锁忙 / 门未开」与「真发失败」分开，避免页面上出现假失败。
enum TlssNrTxResult : uint8_t {
  TLSS_NR_TX_OK = 0,
  TLSS_NR_TX_BUSY,    // 屏障锁被别的注入短暂占用（让位，非失败）
  TLSS_NR_TX_GATED,   // epoch/屏障/挂起/被动 未放行
  TLSS_NR_TX_FAIL     // 真正入队失败
};

static volatile bool tlssNrEnabled = true;    // 免重启默认开启
static volatile bool tlssNrBanned = false;    // 被封车辆默认关闭
static volatile bool tlssNrRestore = false;   // TLSSC 恢复默认关闭

static uint8_t tlssNrFrame[8] = {};           // 缓存的 0x331（已补丁）
static volatile uint8_t tlssNrFrameLen = 0;
static volatile bool tlssNrFrameValid = false;
static volatile bool tlssNrPersisted = false; // 本轮上电是否已写过 NVS

static volatile uint32_t tlssNrStockRx = 0;   // 车端 0x331 帧数
static volatile uint32_t tlssNrStockMs = 0;   // 最近一次车端 0x331 时刻
static volatile uint32_t tlssNrWakeRearms = 0; // 0x331 静默>15s 后恢复触发的重开窗次数
                                              // （与闭源 rebootfix 的 wakeRearms 诊断字段同义）
static volatile uint32_t tlssNrReplayOk = 0;
static volatile uint32_t tlssNrReplayFail = 0;
static volatile uint32_t tlssNrReplayBusy = 0;
static volatile uint32_t tlssNrReplaySkip = 0;
static volatile uint32_t tlssNrEchoOk = 0;    // 读改重发成功
static volatile uint32_t tlssNrEchoFail = 0;
static volatile uint32_t tlssNrLastReplayMs = 0;
static volatile int16_t  tlssNrLastErr = 0;   // esp_err_t 用 int16_t，int8_t 会截断
static volatile uint32_t tlssNrArmedMs = 0;   // 窗口起点
static volatile uint16_t tlssNrBurst = 0;     // 本轮已补发帧数
static volatile uint32_t tlssNrEpochSeen = 0; // 上次看到的 recovery epoch
static volatile uint32_t tlssNrWakeCountSeen = 0;
static volatile bool tlssNrIdleLatched = false; // 总线已进入长时间静默（等它恢复）
static volatile bool tlssNrLastEchoOk = true;   // 上一次 0x331 读改重发是否实际发出
static volatile bool tlssNrPriorityActive = false; // 防重启 TX 正在抢占最高优先级
static TaskHandle_t tlssNrTaskHandle = nullptr;
static portMUX_TYPE tlssNrStateMux = portMUX_INITIALIZER_UNLOCKED;

// ── 车型能力 ─────────────────────────────────────────────────
// 与 v3.29.3 的 vehicleProfileTlsscRestoreSupported() 同义：
// 只有 Standard 3/Y 的 Body+Chassis 拓扑（CAN B = Chassis）支持；
// Model Y L 的 0x331 语义未经确认，明确排除。
static bool tlssNrProfileSupported() {
  return activeCanBIsChassis() && !activeProfileIsYl();
}

// 返回 true 时，其他周期性注入（如灯光/领航）应主动让出 canTxBarrierMutex
static inline bool tlssNrPriorityYieldDue(uint32_t now) {
  if (__atomic_load_n(&tlssNrPriorityActive, __ATOMIC_ACQUIRE)) return true;
  if (!tlssNrEnabled || !tlssNrBanned || !tlssNrRestore) return false;
  if (!tlssNrProfileSupported() || !tlssNrFrameValid) return false;
  if (canUsbPassive() || canTxAdministrativeHold || !twaiReady) return false;
  if (tlssNrArmedMs == 0u || tlssNrBurst >= TLSS_NR_MAX_FRAMES) return false;
  if ((uint32_t)(now - tlssNrArmedMs) > TLSS_NR_WINDOW_MS) return false;
  if (tlssNrStockMs && tlssNrLastEchoOk && (uint32_t)(now - tlssNrStockMs) < TLSS_NR_STOCK_FRESH_MS) return false;
  const uint32_t period = ((uint32_t)(now - tlssNrArmedMs) < TLSS_NR_WAKE_FAST_MS)
                              ? TLSS_NR_WAKE_PERIOD_MS
                              : TLSS_NR_PERIOD_MS;
  return tlssNrLastReplayMs == 0u ||
         (uint32_t)(now - tlssNrLastReplayMs) + 12u >= period;
}

// ── 从旧固件迁移开关状态 ─────────────────────────────────────
// 3.29.3 / No-Reboot 把这两个开关放在 features/cfg3282 的 byte2(被封) 与
// byte4(恢复)，另有同名标量 banned / tlRestore 兜底。上游 3.35.x 把这两格
// 标成 retired 并写零，所以升级后必须迁一次，否则用户要从头再点一遍。
// 只在 tlsscnr 里还没有这两个键时执行一次。
static void tlssNrMigrateFromLegacy() {
  Preferences probe;
  if (!probe.begin(TLSS_NR_NS, true)) return;
  const bool fresh = !probe.isKey(TLSS_NR_KEY_BAN) && !probe.isKey(TLSS_NR_KEY_REST);
  probe.end();
  if (!fresh) return;

  bool banned = false, restore = false, readAnything = false;
  Preferences f;
  if (f.begin("features", true)) {
    uint8_t bytes[14] = {};
    const size_t len = f.getBytesLength("cfg3282");
    if (len == sizeof(bytes) && f.getBytes("cfg3282", bytes, sizeof(bytes)) == sizeof(bytes)) {
      if (bytes[0] == 1u) {
        banned = bytes[2] != 0u;
        restore = bytes[4] != 0u;
        readAnything = true;
      }
    } else if (f.isKey("banned") || f.isKey("tlRestore")) {
      banned = f.getBool("banned", false);
      restore = f.getBool("tlRestore", false);
      readAnything = true;
    }
    f.end();
  }
  if (!readAnything) return;
  // 迁移只搬运状态，不激活任何新东西：免重启仍是用户自己原来的选择（默认开）。
  Preferences p;
  if (!p.begin(TLSS_NR_NS, false)) return;
  (void)p.putBool(TLSS_NR_KEY_BAN, banned);
  (void)p.putBool(TLSS_NR_KEY_REST, restore);
  p.end();
}

// ── NVS ─────────────────────────────────────────────────────
static void tlssNrLoad() {
  tlssNrMigrateFromLegacy();
  Preferences p;
  if (!p.begin(TLSS_NR_NS, true)) return;
  tlssNrEnabled = p.getBool(TLSS_NR_KEY_ON, true);
  tlssNrBanned = p.getBool(TLSS_NR_KEY_BAN, false);
  tlssNrRestore = p.getBool(TLSS_NR_KEY_REST, false);
  String hex = p.getString(TLSS_NR_KEY_HEX, "");
  const bool hasLenKey = p.isKey(TLSS_NR_KEY_LEN);
  const uint8_t savedLen = (uint8_t)p.getUChar(TLSS_NR_KEY_LEN, 0u);
  p.end();

  if (hex.length() >= 2 && hex.length() <= 16 && (hex.length() % 2u) == 0u) {
    uint8_t byteCount = (uint8_t)(hex.length() / 2u);
    bool ok = true;
    for (uint8_t i = 0; i < byteCount; i++) {
      auto nib = [](char c) -> int8_t {
        if (c >= '0' && c <= '9') return (int8_t)(c - '0');
        if (c >= 'a' && c <= 'f') return (int8_t)(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return (int8_t)(c - 'A' + 10);
        return -1;
      };
      const int8_t a = nib(hex[i * 2]), b = nib(hex[i * 2 + 1]);
      if (a < 0 || b < 0) { ok = false; break; }
      tlssNrFrame[i] = (uint8_t)((a << 4) | b);
    }
    if (ok) {
      if (savedLen >= 1u && savedLen <= byteCount) {
        byteCount = savedLen;
      } else if (!hasLenKey && byteCount == 8u && tlssNrFrame[6] == 0u && tlssNrFrame[7] == 0u) {
        // 兼容旧版未保存 DLC 的 NVS 缓存：Model 3/Y 原生 0x331 为 6 字节，修正为 6 避免冷唤醒被车机丢弃
        byteCount = 6u;
      }
      tlssNrFrameLen = byteCount;
      tlssNrFrameValid = true;
    }
  }
  tlssNrPersisted = tlssNrFrameValid && hasLenKey;
  tlssNrEpochSeen = 0;
  tlssNrWakeCountSeen = canRecoveryWakeCount;
  tlssNrIdleLatched = false;
  tlssNrLastEchoOk = true;
}

// 每个上电周期最多写一次：0x331 的计数器每帧都变，逐帧落盘会把 flash 写废。
static void tlssNrPersistFrame() {
  if (tlssNrPersisted || !tlssNrFrameValid || tlssNrFrameLen == 0u || tlssNrFrameLen > 8u) return;
  char hex[17];
  const uint8_t len = tlssNrFrameLen;
  for (uint8_t i = 0; i < len; i++) snprintf(hex + i * 2, 3, "%02X", tlssNrFrame[i]);
  hex[len * 2] = '\0';
  Preferences p;
  if (!p.begin(TLSS_NR_NS, false)) return;
  const size_t n = p.putString(TLSS_NR_KEY_HEX, hex);
  const size_t nl = p.putUChar(TLSS_NR_KEY_LEN, len);
  p.end();
  if (n > 0 && nl > 0) tlssNrPersisted = true;
}

static bool tlssNrPersistFlag(const char *key, bool value) {
  Preferences p;
  if (!p.begin(TLSS_NR_NS, false)) return false;
  const size_t n = p.putBool(key, value);
  p.end();
  return n > 0;
}

// ── 发送：单次持锁读 epoch → 判 TX 屏障 → 入队 ────────────────
// 与 canTxTwaiTransmitWithMaskTagged() 同构，但把结果细分成四种，
// 让「让位」不被误记成「失败」（v3.29.3 后期修的就是这个问题）。
static TlssNrTxResult tlssNrTransmit(const twai_message_t &msg, bool priorityWait = false) {
  if (canTxAdministrativeHold) return TLSS_NR_TX_GATED;
  if (!canTxBarrierMutex) return TLSS_NR_TX_BUSY;
  if (priorityWait) __atomic_store_n(&tlssNrPriorityActive, true, __ATOMIC_RELEASE);
  const TickType_t waitTicks = priorityWait ? pdMS_TO_TICKS(5) : 0;
  if (xSemaphoreTake(canTxBarrierMutex, waitTicks) != pdTRUE) {
    if (priorityWait) __atomic_store_n(&tlssNrPriorityActive, false, __ATOMIC_RELEASE);
    return TLSS_NR_TX_BUSY;
  }

  esp_err_t err = ESP_ERR_INVALID_STATE;
  // epoch 在锁内读取；0x331 仅走 CAN B (CHASSIS / VH)，绝不因等待 CAN A 唤醒而阻断休眠唤醒窗口。
  const uint32_t epoch = canTxBarrierState.epoch;
  if (!canTxAdministrativeHold && twaiReady && epoch != 0u &&
      canTxBarrierAllowsMaskedPure(canTxBarrierState, epoch, CAN_TX_FRESH_VH))
    err = p4CanSendB(&msg, 0, CAN_TX_TRACE_SOURCE_TLSSC);

  xSemaphoreGive(canTxBarrierMutex);
  if (priorityWait) __atomic_store_n(&tlssNrPriorityActive, false, __ATOMIC_RELEASE);
  canBTraceRecordTx(&msg, err, CAN_TX_TRACE_SOURCE_TLSSC);
  tlssNrLastErr = (int16_t)err;

  if (err == ESP_OK) return TLSS_NR_TX_OK;
  if (err == ESP_ERR_TIMEOUT) return TLSS_NR_TX_BUSY;      // 队列满/锁节奏
  if (err == ESP_ERR_INVALID_STATE) return TLSS_NR_TX_GATED;
  return TLSS_NR_TX_FAIL;
}

// 由 CAN B RX 路径在检测到首帧 / 静默超过 4.5s 后恢复时即时唤醒，不等 100ms 轮询
static inline void tlsscNrNotifyWake(uint32_t now) {
  portENTER_CRITICAL(&tlssNrStateMux);
  tlssNrIdleLatched = false;
  const uint32_t stamp = now ? now : 1u;
  if (tlssNrArmedMs == 0u || (uint32_t)(now - tlssNrArmedMs) >= 3000u ||
      tlssNrBurst >= TLSS_NR_MAX_FRAMES) {
    tlssNrArmedMs = stamp;
    tlssNrBurst = 0;
    tlssNrLastReplayMs = 0;
    tlssNrStockMs = 0;
    tlssNrLastEchoOk = false;
    if (tlssNrWakeRearms != 0xFFFFFFFFu) tlssNrWakeRearms++;
  }
  portEXIT_CRITICAL(&tlssNrStateMux);
  TaskHandle_t h = tlssNrTaskHandle;
  if (h) xTaskNotifyGive(h);
}

// ── 车端 0x331 观察（CAN B 分发表调用）─────────────────────
// 两件事：①记录「车确实在自己发」；②缓存补丁后的载荷。
// 开关关着也缓存（只写 RAM），否则用户第一次打开要白等一轮上电。
static void tlsscNrObserve331(const twai_message_t &f) {
  if (!tlssNrProfileSupported()) return;   // YL / 非 Chassis 拓扑：不缓存也不补发
  if (f.extd || f.rtr || f.data_length_code < 1u) return;
  tlssNrStockRx++;
  const uint32_t now = (uint32_t)millis();
  const uint32_t prevStockMs = tlssNrStockMs;
  tlssNrStockMs = now ? now : 1u;

  uint8_t patched[8] = {};
  const uint8_t n = f.data_length_code > 8u ? 8u : f.data_length_code;
  memcpy(patched, f.data, n);
  patched[0] = (uint8_t)((patched[0] & 0xC0U) | TLSS_NR_SELF_DRIVING_BITS);

  bool needPersist = false;
  portENTER_CRITICAL(&tlssNrStateMux);
  if (tlssNrEnabled && tlssNrFrameValid && prevStockMs != 0u &&
      (uint32_t)(now - prevStockMs) > TLSS_NR_STOCK_GAP_MS) {
    tlssNrArmedMs = now ? now : 1u;
    tlssNrBurst = 0;
    tlssNrLastReplayMs = 0;
    if (tlssNrWakeRearms != 0xFFFFFFFFu) tlssNrWakeRearms++;
  }
  bool changed = (tlssNrFrameLen != n) || !tlssNrFrameValid;
  for (uint8_t i = 0; i < n && !changed; i++)
    if (tlssNrFrame[i] != patched[i]) changed = true;
  if (changed) {
    memcpy(tlssNrFrame, patched, n);
    tlssNrFrameLen = n;
    tlssNrFrameValid = true;
    tlssNrPersisted = false;
    needPersist = tlssNrEnabled;
  }
  portEXIT_CRITICAL(&tlssNrStateMux);
  if (needPersist) tlssNrPersistFrame();

  // 读-改-重发：只在两个开关都开着、且车型受支持时动作。
  if (!tlssNrBanned || !tlssNrRestore) return;
  if (!tlssNrProfileSupported()) return;
  if (canUsbPassive()) return;
  if (patched[0] == f.data[0]) {
    tlssNrLastEchoOk = true;
    return;          // 车已经是 SELF_DRIVING
  }

  twai_message_t out = f;
  out.flags = 0;
  memcpy(out.data, patched, n);
  out.data_length_code = n;
  const TlssNrTxResult r = tlssNrTransmit(out, true);
  if (r == TLSS_NR_TX_OK) {
    tlssNrEchoOk++;
    tlssNrLastEchoOk = true;
  } else {
    tlssNrLastEchoOk = false;
    if (r == TLSS_NR_TX_FAIL) tlssNrEchoFail++;
    TaskHandle_t h = tlssNrTaskHandle;
    if (h) xTaskNotifyGive(h);
  }
}

// ── 重开窗口：epoch 变化 / 恢复唤醒 / 总线静默后唤醒 ────────
static void tlssNrMaybeRearm(uint32_t now) {
  bool rearm = false;
  bool epochChanged = false;
  bool wakeEvent = false;

  const uint32_t epochNow = canTxEpochSnapshot();
  if (epochNow != 0u && epochNow != tlssNrEpochSeen) {
    tlssNrEpochSeen = epochNow;
    rearm = true;
    epochChanged = true;
  }

  const uint32_t wakeNow = canRecoveryWakeCount;
  if (wakeNow != tlssNrWakeCountSeen) {
    tlssNrWakeCountSeen = wakeNow;
    rearm = true;
    wakeEvent = true;
  }

  // 静默锁存：只在「进入静默」时置位、在「静默结束」时消费一次。
  const uint32_t lastB = (uint32_t)lastCanBFrameMs;
  if (lastB != 0u) {
    const uint32_t gap = (uint32_t)(now - lastB);
    if (gap > TLSS_NR_IDLE_REARM_MS) {
      tlssNrIdleLatched = true;
    } else if (tlssNrIdleLatched) {
      tlssNrIdleLatched = false;
      rearm = true;   // 静默结束：车醒了，重新开窗
      wakeEvent = true;
    }
  }

  if (!rearm) return;
  portENTER_CRITICAL(&tlssNrStateMux);
  const bool windowActive = tlssNrArmedMs != 0u &&
                            (uint32_t)(now - tlssNrArmedMs) <= TLSS_NR_WINDOW_MS &&
                            tlssNrBurst < TLSS_NR_MAX_FRAMES;
  if (!windowActive || epochChanged || wakeEvent) {
    tlssNrArmedMs = now ? now : 1u;
    tlssNrBurst = 0;
    tlssNrLastReplayMs = 0;
    if (wakeEvent || epochChanged) {
      tlssNrStockMs = 0;
      tlssNrLastEchoOk = false;
    }
  }
  portEXIT_CRITICAL(&tlssNrStateMux);
}

// ── 周期补发（独立任务：唤醒前 5s 为 50ms，其余为 100ms）──────
static void tlsscNrTick() {
  const uint32_t now = (uint32_t)millis();
  tlssNrMaybeRearm(now);

  if (!tlssNrEnabled) return;
  if (!tlssNrBanned || !tlssNrRestore) return;   // 少一个都不动手
  if (!tlssNrProfileSupported()) return;
  if (canUsbPassive() || canTxAdministrativeHold || !twaiReady) return;

  portENTER_CRITICAL(&tlssNrStateMux);
  if (tlssNrArmedMs == 0) tlssNrArmedMs = now ? now : 1u;
  const uint32_t armedMs = tlssNrArmedMs;
  const uint16_t burst = tlssNrBurst;
  const bool valid = tlssNrFrameValid;
  const uint8_t flen = tlssNrFrameLen;
  uint8_t frameCopy[8] = {};
  if (valid) memcpy(frameCopy, tlssNrFrame, 8);
  portEXIT_CRITICAL(&tlssNrStateMux);

  if ((uint32_t)(now - armedMs) > TLSS_NR_WINDOW_MS) return;
  if (burst >= TLSS_NR_MAX_FRAMES) return;
  if (!valid || flen == 0u) { tlssNrReplaySkip++; return; }

  // 车自己在发且刚才 echo 成功才让位；若 echo 未发成功（例如 CAN A 未醒/短暂被挡），立即由补发接管。
  if (tlssNrStockMs && tlssNrLastEchoOk && (uint32_t)(now - tlssNrStockMs) < TLSS_NR_STOCK_FRESH_MS) return;
  const uint32_t period = ((uint32_t)(now - armedMs) < TLSS_NR_WAKE_FAST_MS)
                              ? TLSS_NR_WAKE_PERIOD_MS
                              : TLSS_NR_PERIOD_MS;
  if (tlssNrLastReplayMs && (uint32_t)(now - tlssNrLastReplayMs) < period) return;

  twai_message_t out = {};
  out.identifier = TLSS_NR_ID;
  out.extd = 0;
  out.rtr = 0;
  out.data_length_code = flen;
  memcpy(out.data, frameCopy, flen);

  const TlssNrTxResult r = tlssNrTransmit(out, true);
  if (r == TLSS_NR_TX_BUSY) {
    tlssNrReplayBusy++;
    return;
  }
  if (r == TLSS_NR_TX_GATED) { tlssNrReplaySkip++; return; }

  tlssNrLastReplayMs = now ? now : 1u;
  portENTER_CRITICAL(&tlssNrStateMux);
  tlssNrBurst++;
  portEXIT_CRITICAL(&tlssNrStateMux);
  if (r == TLSS_NR_TX_OK) tlssNrReplayOk++;
  else tlssNrReplayFail++;
}

static void tlsscNrTask(void *) {
  tlssNrTaskHandle = xTaskGetCurrentTaskHandle();
  for (;;) {
    tlsscNrTick();
    const uint32_t now = (uint32_t)millis();
    const bool fast = tlssNrArmedMs != 0u &&
                      (uint32_t)(now - tlssNrArmedMs) < TLSS_NR_WAKE_FAST_MS;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(fast ? TLSS_NR_WAKE_PERIOD_MS : TLSS_NR_PERIOD_MS));
  }
}

// ── 开关写入 ─────────────────────────────────────────────────
static bool tlssNrSet(const char *key, bool value, const char *&note) {
  note = "applied";
  if (!key) { note = "bad-key"; return false; }
  const bool isOn = strcmp(key, "on") == 0;
  const bool isBan = strcmp(key, "banned") == 0;
  const bool isRes = strcmp(key, "restore") == 0;
  if (!isOn && !isBan && !isRes) { note = "bad-key"; return false; }
  // YL / 非 Chassis 拓扑上没有 0x331 的已验证路径，直接拒绝，避免写出
  // 「开着但永远不生效」的状态。
  if ((isBan || isRes) && !tlssNrProfileSupported()) {
    note = "profile-unsupported";
    return false;
  }

  const char *nvsKey = isOn ? TLSS_NR_KEY_ON : (isBan ? TLSS_NR_KEY_BAN : TLSS_NR_KEY_REST);
  if (!tlssNrPersistFlag(nvsKey, value)) { note = "nvs-write-failed"; return false; }

  if (isOn) tlssNrEnabled = value;
  else if (isBan) tlssNrBanned = value;
  else tlssNrRestore = value;

  if (isOn && value && tlssNrFrameValid && !tlssNrPersisted) tlssNrPersistFrame();
  // 刚点开时立刻开窗，不用等下一轮上电。
  if (value && (isOn || isBan || isRes)) {
    tlssNrArmedMs = 0;
    tlssNrBurst = 0;
    tlssNrLastReplayMs = 0;
  }
  return true;
}

static bool tlssNrClearCache(const char *&note) {
  note = "cleared";
  Preferences p;
  if (!p.begin(TLSS_NR_NS, false)) { note = "nvs-open-failed"; return false; }
  const size_t n = p.remove(TLSS_NR_KEY_HEX);
  p.end();
  memset(tlssNrFrame, 0, sizeof(tlssNrFrame));
  tlssNrFrameLen = 0;
  tlssNrFrameValid = false;
  tlssNrPersisted = false;
  tlssNrBurst = 0;
  tlssNrArmedMs = (uint32_t)millis();
  if (n == 0) { note = "nothing-to-clear"; return true; }
  return true;
}

// ── 状态 JSON ────────────────────────────────────────────────
static String tlsscNrStatusJson() {
  const uint32_t now = (uint32_t)millis();
  const bool profileOk = tlssNrProfileSupported();
  const bool prereq = tlssNrBanned && tlssNrRestore && profileOk;
  String j = "{";
  j += "\"enabled\":"; j += tlssNrEnabled ? "true" : "false";
  j += ",\"banned\":";  j += tlssNrBanned ? "true" : "false";
  j += ",\"restore\":"; j += tlssNrRestore ? "true" : "false";
  j += ",\"profileOk\":"; j += profileOk ? "true" : "false";
  j += ",\"prereq\":";  j += prereq ? "true" : "false";
  j += ",\"cacheValid\":"; j += tlssNrFrameValid ? "true" : "false";
  j += ",\"cacheHex\":\"";
  if (tlssNrFrameValid)
    for (uint8_t i = 0; i < tlssNrFrameLen; i++) {
      char b[3];
      snprintf(b, sizeof(b), "%02X", tlssNrFrame[i]);
      j += b;
    }
  j += "\",\"stockRx\":";   j += String((unsigned long)tlssNrStockRx);
  j += ",\"stockMs\":";     j += String((unsigned long)tlssNrStockMs);
  j += ",\"replayOk\":";    j += String((unsigned long)tlssNrReplayOk);
  j += ",\"replayFail\":";  j += String((unsigned long)tlssNrReplayFail);
  j += ",\"replayBusy\":";  j += String((unsigned long)tlssNrReplayBusy);
  j += ",\"replaySkip\":";  j += String((unsigned long)tlssNrReplaySkip);
  j += ",\"echoOk\":";      j += String((unsigned long)tlssNrEchoOk);
  j += ",\"echoFail\":";    j += String((unsigned long)tlssNrEchoFail);
  j += ",\"burst\":";       j += String((unsigned)tlssNrBurst);
  j += ",\"wakeRearms\":";  j += String((unsigned long)tlssNrWakeRearms);
  j += ",\"lastMs\":";      j += String((unsigned long)tlssNrLastReplayMs);
  j += ",\"lastErr\":";     j += String((int)tlssNrLastErr);
  j += ",\"lastErrName\":\"";
  switch ((uint16_t)tlssNrLastErr) {
    case 0:     j += "OK"; break;
    case 0x107: j += "TX-busy"; break;
    case 0x103: j += "gate-closed"; break;
    case 0x10B: j += "send-failed"; break;
    default:    j += "other"; break;
  }
  j += "\"";
  // 窗口剩余秒数：页面据此显示「补发中 12s / 已结束」。
  uint32_t left = 0;
  if (tlssNrArmedMs != 0u && (uint32_t)(now - tlssNrArmedMs) <= TLSS_NR_WINDOW_MS)
    left = (TLSS_NR_WINDOW_MS - (uint32_t)(now - tlssNrArmedMs)) / 1000u;
  j += ",\"windowLeft\":"; j += String((unsigned long)left);
  j += ",\"windowMs\":";   j += String((unsigned long)TLSS_NR_WINDOW_MS);
  j += ",\"maxFrames\":";  j += String((unsigned)TLSS_NR_MAX_FRAMES);
  j += ",\"epoch\":\"";    j += canTxEpochSnapshot() ? "live" : "boot";
  j += "\"}";
  return j;
}
