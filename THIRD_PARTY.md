# Third-party code & attribution

This open-source repository integrates and extends multiple community projects for ESP32-P4 Tesla CAN gateways. Original copyrights and licenses remain with their respective authors:

## Primary Merged Sources

1. **[CARyanChan/P4can-universal-noreboot](https://github.com/CARyanChan/P4can-universal-noreboot)** (`v3.35.3-norestart2`, GPL-3.0-or-later)
   - Core baseline: TLSSC Anti-Reboot (`tlssc_norestart.h` `0x331` highest-priority replay), CAN-A/B/C 3-controller TWAI driver, CAN GPIO runtime remapping, Web API, dual-language dashboard architecture, R79 timing fix, SmartShift, Battery Preheat (beta), Continuous AP, Auto Blinker, Service Mode, FSD Vis, Wi-Fi Uplink (`p4_uplink.h`) & Capture (`feifan_capture.h`).
2. **[i520e3e4/p4can-unlock-more](https://github.com/i520e3e4/p4can-unlock-more)** (MIT for new additions / reference specs)
   - Reference specs and feature inspiration for extended lighting interactions (Horn-to-High-Beam flash concept, high-beam strobe, Rear Fog Pilot modes, reverse gear rear fog assist) and UI menu design. Unverified/BLE-only features (such as BLE ambient/seat commands without confirmed CAN IDs) were intentionally excluded rather than fabricated on CAN.

## Upstream Lineage

- **[06066060606060/P4can-universal-unlock](https://github.com/06066060606060/P4can-universal-unlock)** (tag `3.35.3`, GPL-3.0-or-later) — upstream firmware base (`docs/README-upstream-v3.35.3.md`).
- **[baicyber/P4can-No-Reboot](https://github.com/baicyber/P4can-No-Reboot)** (`v3.29.3`, MIT) — original anti-restart concept, `p4_uplink.h` and `feifan_capture.h`.
- **[kl79613/P4Can-automatic-shifting](https://github.com/kl79613/P4Can-automatic-shifting)** — SmartShift automatic shifting logic (via upstream).

## Community Additions in `v3.35.3-norestart2-plus`

- ESP32-P4 + ESP32-C5 ESP-Hosted `transport_drv.c.obj` chip-ID `23` (`esp32c5`) binary compatibility patch (`tools/patch_c5_hosted.py`) & C5/C6 unified multi-variant build script (`Compile-P4-C5-C6.ps1`).
- Dual-band 5 GHz + Wi-Fi 6 (`802.11ax`) AP radio configuration (`p4_network.h`, `web_api.h`, dashboards).
- Wi-Fi Repeater DNS + NAPT forwarding fix (`OFFER_DNS`, `esp_netif_set_dns_info`, `WiFi.STA.setDefault()`, `192.168.4.x` conflict detection).
- Horn-to-High-Beam Flash (`0x3C2` MUX0 `VCLEFT_hornSwitchPressed`), Rear Fog Pilot & Reverse Gear Rear Fog (`0x118` -> `0x273`).
- Dynamic C5/C6 CAN GPIO validation + pre-setup GPIO access + real-time ESP32-P4 chip temperature & uptime header telemetry.

## License Chain

- Whole repository: **GPL-3.0-or-later** (see [LICENSE](LICENSE)).
- Original MIT-licensed modules (`p4_uplink.h`, `feifan_capture.h`) retain their MIT headers.
- Tesla, Model 3, Model Y, Model Y L, Autopilot, and FSD are trademarks of Tesla, Inc.; this project has no affiliation with Tesla, Inc.
