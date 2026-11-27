# P4can-universal-noreboot —— 合并说明（上游 v3.35.3 + No-Reboot 免重启）

这个工程不是新写的固件，而是**把两个仓库各取所长合到一起**：

| 来源 | 取什么 |
| --- | --- |
| [06066060606060/P4can-universal-unlock](https://github.com/06066060606060/P4can-universal-unlock) `v3.35.3`（tag `3.35.3`） | 全部新功能与全部上游修复：灯光整页、CAN GPIO 设置、SmartShift、电池预热、R79 注入修复、FSD 可视化、服务模式、zh-CN、三控制器 TWAI 驱动等 |
| [baicyber/P4can-No-Reboot](https://github.com/baicyber/P4can-No-Reboot) `v3.29.3` + rebootfix | **TLSSC 免重启**（含它依赖的「被封车辆 + TLSSC 恢复」），以及上游没有的 **Wi-Fi 中继** 与 **独立抓包** |

## 为什么是这个方向

No-Reboot 的固件本身就是「v3.29.3 + 免重启补丁」。它的灯光整页、CAN GPIO、SmartShift
本来就是从作者 v3.35.x 移植过去的。所以正确做法是**以上游最新版为基线，把免重启补丁
重新落上去**，而不是反过来把 3.35 的功能往 3.29.3 上搬——后者等于把上游三十多个提交
（54k 行）手工重放一遍，几乎必错。

一个关键前提：**上游 3.31 之后把 Banned Car / TLSSC Restore 整块删了**
（上游 CHANGELOG：「Fully remove Banned Car/TLSSC Restore state, profile capabilities,
APIs and dedicated 0x331 path」）。而免重启正是靠这两项工作，所以本次把它们
**作为独立模块重建**，用单独的 NVS 命名空间 `tlsscnr`，不动上游 `features` 的
14 字节 blob 布局（上游已把 offset 2/4 标为 retired，改它会破坏别人的存档）。

## 本次新增/修改的文件

| 文件 | 改动 |
| --- | --- |
| `tlssc_norestart.h` | **新增**。被封车辆 + TLSSC 恢复 + 免重启补发，一份实现 |
| `p4_uplink.h` | **新增**。Wi-Fi 中继（AP+STA + NAPT + DNS 同步） |
| `feifan_capture.h` | **新增**。独立抓包环缓冲（PSRAM 优先，16k 行） |
| `can_runtime.h` | CAN B 分发加 `case 0x331`；CAN A/B/C 三处 `ffCapObserve` 观察点 |
| `web_api.h` | `/api/tlssc-nr/*`、`/api/wifi/uplink`、`/api/capture/*` 路由与处理器；`wifiApBootStart()` 先读中继配置 |
| `p4_network.h` | 中继开启时 Wi-Fi 以 `WIFI_AP_STA` 起（否则 STA 会被 AP 顶掉） |
| `dashboard_source.html`、`zh-CN/dashboard_source.zh-CN.html` | 设置页注入三张卡：TLSSC 免重启、Wi-Fi 中继、抓包 |
| `index_html.h` | 由 `tools/build_dashboard.py` 重新生成 |
| `.ino` | 版本号 `v3.35.3-norestart2`；加载配置、起 100 ms 补发任务、`loop()` 里推进中继 |

## 免重启是怎么工作的

「被封锁的车」在车机启动时只读一次 `0x331 DAS_autopilotConfig` 并锁存。上游只做
「车发一帧 → 我们改一帧」的旁路改写，所以车机启动那一刻总线上没有改过的帧，就锁存
「封禁」，每轮上电都得人工重启车机。

本实现把这件事**提前到车机读配置之前**：

1. 把补丁过的 `0x331` 载荷存进 NVS（每个上电周期最多写一次，护 flash）；
2. 每次上电 CAN B 就绪后开 60 s 窗口，以 10 Hz 补发缓存帧，不等车先发；
3. 车自己开始发 `0x331`（2 s 内有新鲜帧）立即停手，回到读-改-重发，不抢总线；
4. 窗口到期或补满 600 帧彻底停手。

只在「被封车辆」+「TLSSC 恢复」都开着时动作；没有缓存帧时什么都不发。

### 相对 v3.29.3 的三处实现改进

- **单次持锁**：epoch 读取、TX 屏障判定、入队在同一把屏障锁内完成。v3.29.3 是
  「先取 epoch 再发」，中间会被 5 ms 灯光任务 / AP 注入 / 维护模式插进来，偶发
  `ESP_ERR_TIMEOUT`。现在把锁忙单列为「让位」，不计失败、下周期自动重试。
- **唤醒重开窗**：recovery epoch 变化、或总线整体静默超过 30 s，都视为新一轮上电，重新开窗补发（对应 rebootfix 里「休眠唤醒后重开补发窗口」）。
- **0x331 恢复重开窗（v2，对齐闭源 rebootfix）**：通过对闭源 `*-rebootfix-ota.bin`
  的反汇编还原了其唤醒检测——`0x331` 静默超过 15 s 后恢复的第一帧视为唤醒事件，
  重开 60 s 窗口并累计 `wakeRearms` 计数（状态 JSON 与页面已同步展示该字段）。
  该触发覆盖「总线未睡、车机单独重启」的场景（epoch/总线空闲检测捕获不到）。
  本固件同时保留 epoch / 总线空闲两条触发路径，覆盖面比闭源版更宽。
- **Model Y L 不补发**：YL 的 `0x331` 路由与字段语义未经实车确认，只允许
  Standard 3/Y（CAN B = Chassis）使用；YL 上三个开关直接置灰，后端也拒绝写入。

## 页面在哪

设置页 → Features 卡片下面，依次是：

1. **TLSSC 免重启**：被封车辆 / TLSSC 恢复 / 免重启三个开关，下面一屏读数
   （缓存帧 hex、车端 0x331 帧数、补发 OK/失败、让位次数、窗口剩余、读改重发 OK/失败）。
2. **Wi-Fi 中继**：填上游 SSID/密码 → 连接；状态显示 IP、客户端数、RSSI。
3. **抓包**：勾 A/B/C 总线 + 可选 ID 过滤（如 `0x331`），开始/停止/清空/导出 CSV；
   下面是最近 20 行的实时列表。

## 刷机

产物在 `../Final-Build/`（`manifest.json` 里有 sha256）：

| 变体 | OTA（平时升级） | full（首次/救砖） |
| --- | --- | --- |
| 英文 | `en/...-ota.bin` 1,573,792 B | `en/...-full.bin` 33,554,432 B |
| 中文 | `zh/...-zh-ota.bin` 1,578,640 B | `zh/...-zh-full.bin` 33,554,432 B |

- **首次 / 救砖**：`esptool.py --chip esp32p4 -p <PORT> -b 460800 write_flash 0x0 <full.bin>`
- **平时升级**：网页 `http://192.168.4.1` → 设置 → 固件更新 → 选 `*-ota.bin`

分区表 `app5M_fat24M_32MB`：`nvs@0x9000`、`otadata@0xe000`、`app0@0x10000`、
`app1@0x490000`（各 0x480000），与 No-Reboot 现有固件**完全一致**，
所以两边可以直接互相 OTA 升级，NVS 里的配置也不会丢。

> ⚠️ 只适用于 32 MB Flash 的板子（微雪 ESP32-P4-C6-WIFI6 / rev3.2）。16 MB 版本刷这个包会变砖。

## 从 No-Reboot 固件升级过来时

旧固件的「被封车辆 / TLSSC 恢复」存在 `features/cfg3282` 的 byte2 / byte4（以及同名标量
`banned` / `tlRestore`）。上游 3.35.x 把这两格标成 retired 并写零，所以本固件**首次启动
会自动迁移一次**到新的 `tlsscnr` 命名空间——开关状态不用重新点。迁移只搬状态，不激活
任何东西：免重启本身仍是默认开启。

## 自检顺序（真车之前）

1. 上电看页面顶部版本号是否为 `v3.35.3-norestart2`。
2. 设置页能看到三张新卡片；Model Y L 下 TLSSC 卡三个开关应为置灰。
3. 打开「被封车辆」+「TLSSC 恢复」，等缓存帧出现（`cacheValid: true`），再打开免重启。
4. `GET /api/tlssc-nr/status` 观察 `stockRx` 增长、`replayOk` 增长、`replayFail` 应长期为 0。
5. 断电再上电：车机不应再自动重启一次。
