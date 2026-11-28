package dev.t2can.unlock;

public final class DeviceProfile {
    public static final int MODE_AP_DEFAULT = 0;
    public static final int MODE_HOTSPOT_RELAY = 1;
    public static final int MODE_AP_PUBLIC = 2;

    public static final int STATUS_DOT_OFFLINE = 0;
    public static final int STATUS_DOT_CONNECTING = 1;
    public static final int STATUS_DOT_ONLINE = 2;

    public final String id;
    public final String name;
    public final String ssid;
    public final String security;
    public final String password;
    public final String url;
    public final int mode;
    public final String lastDiscoveredUrl;
    public final boolean autoConnect;
    public final boolean manual;

    public DeviceProfile(
            String id,
            String name,
            String ssid,
            String security,
            String password,
            String url,
            int mode,
            String lastDiscoveredUrl,
            boolean autoConnect,
            boolean manual) {
        this.id = id;
        this.name = name;
        this.ssid = ssid;
        this.security = security;
        this.password = password;
        this.url = url;
        this.mode = clampMode(mode);
        this.lastDiscoveredUrl = lastDiscoveredUrl == null ? "" : lastDiscoveredUrl;
        this.autoConnect = autoConnect;
        this.manual = manual;
    }

    public static int clampMode(int mode) {
        if (mode < MODE_AP_DEFAULT || mode > MODE_AP_PUBLIC) {
            return MODE_AP_DEFAULT;
        }
        return mode;
    }

    public static int inferModeFromUrl(String url) {
        String host = UrlPolicy.hostLabel(url);
        if (UrlPolicy.PUBLIC_HOST.equals(host)) {
            return MODE_AP_PUBLIC;
        }
        return MODE_AP_DEFAULT;
    }

    public static DeviceProfile createDefault() {
        return new DeviceProfile(
                "default",
                "P4-CAN / T2CAN",
                "P4-CAN-PRO",
                "WPA2",
                "12345678",
                UrlPolicy.DEFAULT_HOME,
                MODE_AP_DEFAULT,
                "",
                true,
                false);
    }

    public DeviceProfile withMode(int targetMode) {
        int nextMode = clampMode(targetMode);
        String nextUrl = url;
        if (nextMode == MODE_AP_DEFAULT) {
            if (url == null || url.isEmpty() || UrlPolicy.PUBLIC_HOME.equals(url)) {
                nextUrl = UrlPolicy.DEFAULT_HOME;
            }
        } else if (nextMode == MODE_AP_PUBLIC) {
            if (url == null || url.isEmpty() || UrlPolicy.DEFAULT_HOME.equals(url)) {
                nextUrl = UrlPolicy.PUBLIC_HOME;
            }
        } else {
            if (UrlPolicy.DEFAULT_HOME.equals(url) || UrlPolicy.PUBLIC_HOME.equals(url)) {
                nextUrl = "";
            }
        }
        return new DeviceProfile(
                id,
                name,
                ssid,
                security,
                password,
                nextUrl,
                nextMode,
                lastDiscoveredUrl,
                autoConnect,
                manual);
    }

    public DeviceProfile withLastDiscoveredUrl(String discovered) {
        return new DeviceProfile(
                id,
                name,
                ssid,
                security,
                password,
                url,
                mode,
                discovered == null ? "" : discovered,
                autoConnect,
                manual);
    }

    public String resolvedHomeUrl() {
        if (mode == MODE_HOTSPOT_RELAY) {
            if (url != null && !url.isEmpty()) {
                return url;
            }
            if (lastDiscoveredUrl != null && !lastDiscoveredUrl.isEmpty()) {
                return lastDiscoveredUrl;
            }
            return "";
        }
        if (url != null && !url.isEmpty()) {
            return url;
        }
        return UrlPolicy.defaultHomeForMode(mode);
    }

    public String modeShortLabel() {
        if (mode == MODE_HOTSPOT_RELAY) {
            return "热点中继";
        }
        if (mode == MODE_AP_PUBLIC) {
            return "100.100.1.1";
        }
        return "192.168.4.1";
    }

    public boolean canAutoConnect() {
        return mode != MODE_HOTSPOT_RELAY
                && autoConnect
                && !manual
                && ssid != null
                && !ssid.trim().isEmpty();
    }

    public boolean requiresDeviceApWifi() {
        return mode != MODE_HOTSPOT_RELAY;
    }

    public String credentialsKey() {
        return ssid + "\n" + security + "\n" + password + "\n" + manual + "\n" + url + "\n" + mode;
    }
}
