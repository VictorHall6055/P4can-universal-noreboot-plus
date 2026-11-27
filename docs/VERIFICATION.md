# Anti-restart verification protocol (four on-vehicle experiments)

Firmware: `v3.35.3-norestart2` · For Standard Model 3 / Y (CAN B = Chassis).
**Do not run on Model Y L** (the firmware greys out and refuses writes on YL).

> 中文版：[VERIFICATION.zh-CN.md](VERIFICATION.zh-CN.md)

## Preparation

1. **Venue & state**: enclosed grounds / private parking, vehicle in P, parked, lights off. Do not run any of these experiments on public roads.
2. **Firmware check**: open `http://192.168.4.1` (join the device AP); the Settings page version must read `v3.35.3-norestart2`; the three new cards must be present (TLSSC anti-restart / Wi-Fi uplink / Capture).
3. **Enable the feature**: turn on *Banned Car*, then *TLSSC Restore*; once the card shows `cacheValid=true` and `cacheHex` in `xx 1B ...` form, turn on *Anti-restart*.
4. **Record the baseline** from `GET /api/tlssc-nr/status` (or the card readout):
   `stockRx / replayOk / replayFail / replayBusy / replaySkip / echoOk / echoFail / wakeRearms / lastErr`
5. **Tools**: phone or laptop for the web UI, pen/screenshot for records. Save a status-JSON snapshot before each experiment.

---

## Experiment 1: replay-path health (`replayFail` / `echoFail` stay 0)

**Purpose**: prove the replay module never accumulates failures — the first proof of behavioural equivalence with the original.

1. Vehicle powered on (in P), keep running for 10+ minutes;
2. Every 1 minute record: `replayOk`, `replayFail`, `echoOk`, `echoFail`, `replayBusy`, `lastErr`;
3. Watch the hand-off: once `stockRx` starts growing (car speaks again), `replayOk` should quickly stop growing (device yields the bus).

**Pass criteria**:
- ✅ `replayFail = 0` and `echoFail = 0` throughout;
- ✅ `replayBusy` may be > 0 (lock-busy yielding is normal) but must not keep growing rapidly;
- ✅ `lastErr` stays `OK` (0).

**Fail**: any counter starts growing → stop, export the status JSON (`/api/tlssc-nr/status`) and a capture CSV filtered to ID `0x331` for analysis.

---

## Experiment 2: cold-boot replay (device power-on window)

**Purpose**: verify the "device boots → CAN-B ready → 60 s window opens" trigger path.

1. Power the vehicle fully down (screen off, leave the car >5 minutes);
2. Power it on and start timing: for **60 s after CAN-B is ready**, record `replayOk / stockRx / burst` every 10 s;
3. Watch the head unit boot: gauge/screen must come up normally with **no extra second reboot**;
4. Check whether Summon is available (app or in-car UI).

**Pass criteria**:
- ✅ `replayOk` grows inside the window (up to 600 frames or until the car takes over);
- ✅ Head unit lights up once, does not reboot again;
- ✅ After the window, the device falls back to read-modify-write mode (`echoOk` grows slowly).

---

## Experiment 3: full sleep/wake (`wakeRearms` +1 — the core experiment)

**Purpose**: verify the closed-source rebootfix's unique trigger — "first `0x331` frame after >15 s of silence re-opens the window" — and the **no-reboot** behaviour itself.

1. **Put the car to sleep**: lock it, walk away, wait **30+ minutes** (overnight is better). Optionally open the Capture page listening on buses A/B/C to confirm the bus goes silent (`0x331` stops);
2. Note the current `wakeRearms` value (call it N₀);
3. **Wake it**: unlock and enter the car, start timing;
4. For the next 2 minutes record every 20 s: `wakeRearms`, `replayOk`, `windowLeft`, `stockRx`;
5. Watch the head unit: after lighting up it must **not** enter a reboot loop or black-screen reboot;
6. Check whether Summon is immediately available after wake (this is the practical "85 m summon without reboot" proof).

**Pass criteria**:
- ✅ `wakeRearms` increased by ≥1 from N₀ (each wake +1; two sleep/wake cycles → +2);
- ✅ `windowLeft` shows a 60 s countdown and `replayOk` grows during it;
- ✅ Replay stops on its own once `stockRx` resumes;
- ✅ No head-unit reboot; the unlock stays in effect.

---

## Experiment 4: head-unit-alone reboot (bus stays alive — hardest to reproduce)

**Purpose**: verify anti-restart still covers the "bus alive, only the head unit rebooted" case — the scenario pure epoch/bus-idle detection misses but the closed-source build handles.

1. Complete Experiment 3 first (confirm `wakeRearms` works);
2. Trigger a head-unit reboot by either —
   - waiting for a **natural** head-unit reboot (occasionally after software updates), or
   - the head unit's own safe restart option (Settings, if your software version offers "restart infotainment / touchscreen"; **do not** cut vehicle power — that turns this into Experiments 2/3);
3. During the reboot `0x331` goes silent >15 s while other bus traffic continues — right after it completes, record: `wakeRearms` (should be +1), `replayOk`, when `stockRx` resumes;
4. Record afterwards: whether a "config check failed → reboots again" chain occurs; whether Summon still works.

**Pass criteria**:
- ✅ `wakeRearms` +1 (proves the 15 s silence trigger works, not just full-vehicle sleep);
- ✅ After one head-unit reboot it runs stably with **no second** reboot;
- ✅ The unlock survives the head-unit reboot.

---

## Result sheet

| Exp | replayFail | echoFail | wakeRearms change | Head-unit reboots | Summon OK | Verdict |
|---|---|---|---|---|---|---|
| 1 (path health) | | | — | — | | |
| 2 (cold boot) | | | — | | | |
| 3 (sleep/wake) | | | N₀ → | | | |
| 4 (HU reboot) | | | | | | |

## If anything fails, collect these before reporting back

1. Full `/api/tlssc-nr/status` JSON (especially `lastErr` code and name);
2. Capture CSV filtered to `0x331`, starting 1 minute before the anomaly;
3. What you did and the timestamps.

## Safe rollback

If at any point the head unit misbehaves (reboot loop, config errors, features failing): **turn off the *Anti-restart* toggle** on the web page. With it off, the device never transmits any `0x331` of its own and becomes read-only — the vehicle returns to stock behaviour immediately.
