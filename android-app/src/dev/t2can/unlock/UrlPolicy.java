package dev.t2can.unlock;

import android.net.Uri;
import java.util.Locale;

public final class UrlPolicy {
    public static final String DEFAULT_HOME = "http://192.168.4.1/";
    public static final String PUBLIC_HOME = "http://100.100.1.1/";
    public static final String DEFAULT_HOST = "192.168.4.1";
    public static final String PUBLIC_HOST = "100.100.1.1";

    private UrlPolicy() {
    }

    public static boolean isAllowedDeviceIp(String host) {
        if (host == null || host.isEmpty()) {
            return false;
        }
        String[] parts = host.split("\\.", -1);
        if (parts.length != 4) {
            return false;
        }
        int[] o = new int[4];
        for (int i = 0; i < 4; i++) {
            String part = parts[i];
            if (part.isEmpty() || part.length() > 3) {
                return false;
            }
            for (int j = 0; j < part.length(); j++) {
                if (!Character.isDigit(part.charAt(j))) {
                    return false;
                }
            }
            try {
                o[i] = Integer.parseInt(part);
            } catch (NumberFormatException ignored) {
                return false;
            }
            if (o[i] < 0 || o[i] > 255) {
                return false;
            }
        }
        if (o[0] == 10) {
            return true;
        }
        if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) {
            return true;
        }
        if (o[0] == 192 && o[1] == 168) {
            return true;
        }
        // Carrier-grade NAT / pseudo-public range (covers 100.100.1.1).
        return o[0] == 100 && o[1] >= 64 && o[1] <= 127;
    }

    public static String defaultHomeForMode(int mode) {
        if (mode == DeviceProfile.MODE_AP_PUBLIC) {
            return PUBLIC_HOME;
        }
        if (mode == DeviceProfile.MODE_HOTSPOT_RELAY) {
            return "";
        }
        return DEFAULT_HOME;
    }

    public static String normalize(String raw) {
        if (raw == null) {
            return "";
        }
        String trimmed = raw.trim();
        if (trimmed.isEmpty()) {
            return "";
        }
        if (!trimmed.contains("://")) {
            trimmed = "http://" + trimmed;
        }
        Uri uri;
        try {
            uri = Uri.parse(trimmed);
        } catch (Exception ignored) {
            return "";
        }
        String scheme = uri.getScheme();
        if (scheme == null) {
            return "";
        }
        scheme = scheme.toLowerCase(Locale.US);
        if (!"http".equals(scheme) && !"https".equals(scheme)) {
            return "";
        }
        if (uri.getUserInfo() != null || uri.getQuery() != null || uri.getFragment() != null) {
            return "";
        }
        String path = uri.getPath();
        if (path != null && !path.isEmpty() && !"/".equals(path)) {
            return "";
        }
        String host = uri.getHost();
        if (!isAllowedDeviceIp(host)) {
            return "";
        }
        int port = uri.getPort();
        int defaultPort = "http".equals(scheme) ? 80 : 443;
        if (port == defaultPort) {
            port = -1;
        }
        if (port != -1 && (port < 1 || port > 65535)) {
            return "";
        }
        StringBuilder out = new StringBuilder();
        out.append(scheme).append("://").append(host);
        if (port > 0) {
            out.append(':').append(port);
        }
        out.append('/');
        return out.toString();
    }

    public static String normalizeForMode(int mode, String raw) {
        String trimmed = raw == null ? "" : raw.trim();
        if (trimmed.isEmpty()) {
            return defaultHomeForMode(mode);
        }
        return normalize(trimmed);
    }

    public static boolean validHomeForMode(int mode, String raw) {
        String trimmed = raw == null ? "" : raw.trim();
        if (mode == DeviceProfile.MODE_HOTSPOT_RELAY && trimmed.isEmpty()) {
            return true;
        }
        return !normalizeForMode(mode, trimmed).isEmpty();
    }

    public static String errorForMode(int mode, String raw) {
        if (validHomeForMode(mode, raw)) {
            return null;
        }
        return "请输入有效的设备 IP 或地址，例如 192.168.4.1、100.100.1.1 或热点局域网 IP（不要包含路径或参数）。";
    }

    public static String hostLabel(String url) {
        if (url == null || url.isEmpty()) {
            return "";
        }
        try {
            Uri uri = Uri.parse(url);
            String host = uri.getHost();
            return host == null ? "" : host;
        } catch (Exception ignored) {
            return "";
        }
    }

    public static boolean isDevice(String url, String activeHome) {
        if (url == null || url.isEmpty()) {
            return false;
        }
        Uri candidate;
        try {
            candidate = Uri.parse(url);
        } catch (Exception ignored) {
            return false;
        }
        String scheme = candidate.getScheme();
        if (scheme == null) {
            return false;
        }
        String lowerScheme = scheme.toLowerCase(Locale.US);
        if (!"http".equals(lowerScheme) && !"https".equals(lowerScheme)) {
            return false;
        }
        String host = candidate.getHost();
        if (host == null || !isAllowedDeviceIp(host)) {
            return false;
        }
        if (activeHome != null && !activeHome.isEmpty()) {
            try {
                Uri home = Uri.parse(activeHome);
                if (lowerScheme.equalsIgnoreCase(home.getScheme())
                        && host.equalsIgnoreCase(home.getHost())
                        && effectivePort(candidate) == effectivePort(home)) {
                    return true;
                }
            } catch (Exception ignored) {
            }
        }
        return DEFAULT_HOST.equals(host) || PUBLIC_HOST.equals(host);
    }

    private static int effectivePort(Uri uri) {
        int port = uri.getPort();
        if (port > 0) {
            return port;
        }
        return "https".equalsIgnoreCase(uri.getScheme()) ? 443 : 80;
    }
}
