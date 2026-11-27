# Merge notes: upstream v3.35.3 + No-Reboot anti-restart

This project is not newly-written firmware — it **combines the strengths of two repositories**:

| Source | What was taken |
| --- | --- |
| [06066060606060/P4can-universal-unlock](https://github.com/06066060606060/P4can-universal-unlock) `v3.35.3` (tag `3.35.3`) | All new features and upstream fixes: lighting page, CAN GPIO settings, SmartShift, battery preheating, R79 injection fix, FSD visualisation, service mode, zh-CN variant, 3-controller TWAI driver, etc. |
| [baicyber/P4can-No-Reboot](https://github.com/baicyber/P4can-No-Reboot) `v3.29.3` + rebootfix | **TLSSC anti-restart** (including the Banned-Car + TLSSC-Restore primitives it depends on), plus the **Wi-Fi uplink relay** and **standalone capture** page that upstream lacks |

## Why this direction

The No-Reboot firmware is itself "v3.29.3 + anti-restart patch". Its lighting page, CAN GPIO and SmartShift were originally ported *from* the author's v3.35.x. So the correct approach was to **take the latest upstream as the baseline and re-apply the anti-restart patch on top** — not the reverse, which would mean manually replaying thirty-plus upstream commits (~54k lines) onto 3.29.3 with near-certain errors.

One key prerequisite: **upstream removed the whole Banned Car / TLSSC Restore block after v3.31** (upstream CHANGELOG: "Fully remove Banned Car/TLSSC Restore state, profile capabilities, APIs and dedicated 0x331 path"). Anti-restart depends exactly on those, so they were **rebuilt as a standalone module** with its own NVS namespace `tlsscnr`, leaving upstream's 14-byte `features` blob layout untouched (upstream marks offsets 2/4 as retired; writing there would corrupt other users' saved settings).

## Files added / modified in this merge

| File | Change |
| --- | --- |
| `tlssc_norestart.h` | **Added.** Banned Car + TLSSC Restore + anti-restart replay, one implementation |
| `p4_uplink.h` | **Added.** Wi-Fi uplink relay (AP+STA + NAPT + DNS sync) |
| `feifan_capture.h` | **Added.** Standalone capture ring buffer (PSRAM-first, 16k rows) |
| `can_runtime.h` | CAN-B dispatch gains a `case 0x331`; three `ffCapObserve` observe points on CAN A/B/C |
| `web_api.h` | `/api/tlssc-nr/*`, `/api/wifi/uplink`, `/api/capture/*` routes and handlers; `wifiApBootStart()` reads uplink config first |
| `p4_network.h` | Wi-Fi starts as `WIFI_AP_STA` when the relay is enabled (otherwise STA gets kicked off by the AP) |
| `dashboard_source.html`, `zh-CN/dashboard_source.zh-CN.html` | Three cards injected into Settings: TLSSC anti-restart, Wi-Fi uplink, Capture |
| `index_html.h` | Regenerated via `tools/build_dashboard.py` |
| `.ino` | Version `v3.35.3-norestart2`; config load, 100 ms replay task, uplink tick in `loop()` |

## How anti-restart works

A "banned" vehicle's head unit latches `0x331 DAS_autopilotConfig` **once at boot**. The upstream approach is read-modify-write on frames the car itself sends, so at boot time no patched frame is on the bus — the car latches "banned" and every power cycle needs a manual restart. This implementation moves the patch **ahead of** the moment the head unit reads its config:

1. The patched `0x331` payload is stored in NVS (written at most once per power cycle, flash-friendly);
2. On every boot, once CAN-B is ready, a 60 s window opens and the cached frame is replayed at 10 Hz without waiting for the car to speak first;
3. The moment the car resumes its own `0x331` (fresh frame within 2 s) replay stops and it falls back to read-modify-write, never fighting the car for the bus;
4. After the window expires or 600 frames are replayed it stops for good.

It acts only when both *Banned Car* and *TLSSC Restore* are enabled; with no cached frame nothing is transmitted.

### Three implementation improvements over v3.29.3

- **Single lock hold**: epoch read, TX-barrier check and enqueue happen inside one barrier-lock hold. v3.29.3 read the epoch then sent, letting the 5 ms lighting task / AP injection / maintenance mode slip in between — occasionally causing `ESP_ERR_TIMEOUT`. Lock-busy is now counted as "yielded", not failed, and retried next cycle automatically.
- **Wake re-arm**: a recovery-epoch change or the bus going silent >30 s is treated as a new power cycle and re-opens the replay window (matches the rebootfix "re-open window after sleep/wake" behaviour).
- **0x331-resume re-arm (v2, aligned with the closed-source rebootfix)**: disassembly of the closed `*-rebootfix-ota.bin` recovered its wake detection — the first `0x331` frame after >15 s of silence is treated as a wake event, re-opening the 60 s window and incrementing the `wakeRearms` counter (shown in the status JSON and on the page). This covers the "bus stayed alive, head unit alone restarted" scenario that epoch/bus-idle detection misses. This firmware keeps all three trigger paths — wider coverage than the closed-source build.
- **Model Y L excluded**: YL's `0x331` routing and field semantics are unverified on a real car; only Standard 3/Y (CAN B = Chassis) is allowed. On YL the three toggles are greyed out and the backend refuses writes.

## Where the UI lives

Settings page, right below the Features card:

1. **TLSSC anti-restart**: Banned Car / TLSSC Restore / Anti-restart toggles plus a live readout (cached frame hex, car 0x331 frame count, replay OK/fail, yield count, window remaining, read-modify-write OK/fail, wake re-arms).
2. **Wi-Fi uplink**: enter upstream SSID/password → connect; status shows IP, station count, RSSI.
3. **Capture**: tick buses A/B/C + optional ID filter (e.g. `0x331`), start/stop/clear/export CSV; live list of the last 20 rows below.

## Flashing

Artifacts live in the GitHub Release (`manifest.json` / release notes carry SHA-256):

| Variant | OTA (regular update) | full (first flash / brick recovery) |
| --- | --- | --- |
| English | `en/...-ota.bin` | `en/...-full.bin` |
| Chinese | `zh/...-zh-ota.bin` | `zh/...-zh-full.bin` |

- **First flash / recovery**: `esptool.py --chip esp32p4 -p <PORT> -b 460800 write_flash 0x0 <full.bin>`
- **Regular update**: web UI `http://192.168.4.1` → Settings → Firmware update → pick `*-ota.bin`

Partition table `app5M_fat24M_32MB`: `nvs@0x9000`, `otadata@0xe000`, `app0@0x10000`, `app1@0x490000` (each 0x480000) — **identical** to the existing No-Reboot firmware, so both can OTA-upgrade each other and NVS settings survive.

> ⚠️ 32 MB flash boards only (Waveshare ESP32-P4-C6-WIFI6 / rev3.2). Flashing a 16 MB board with this image will brick it.

## Upgrading from No-Reboot firmware

The old firmware stored Banned Car / TLSSC Restore in `features/cfg3282` byte2 / byte4 (plus scalar keys `banned` / `tlRestore`). Upstream 3.35.x marks those retired and zeroes them, so on **first boot this firmware auto-migrates once** into the new `tlsscnr` namespace — no need to re-toggle anything. Migration only carries state; nothing is activated: anti-restart itself stays off until enabled as usual.

## Verification order (before a real car)

1. Power up, check the version string at the top of the page: `v3.35.3-norestart2`.
2. Settings page shows the three new cards; on Model Y L the TLSSC card toggles must be greyed out.
3. Enable *Banned Car* + *TLSSC Restore*, wait for the cached frame (`cacheValid: true`), then enable anti-restart.
4. Watch `GET /api/tlssc-nr/status`: `stockRx` grows, `replayOk` grows, `replayFail` stays 0.
5. Power-cycle the car: the head unit must not reboot again.
