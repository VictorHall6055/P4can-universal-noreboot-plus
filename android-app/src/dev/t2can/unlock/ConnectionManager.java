package dev.t2can.unlock;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.RouteInfo;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiNetworkSpecifier;
import android.os.Handler;
import android.os.Looper;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONObject;

public final class ConnectionManager {
    public interface Listener {
        void onStateChanged(UiState state);

        void onReady(String resolvedUrl);

        void onProfileUpdated(DeviceProfile updated);

        void onMessage(String message);
    }

    public static final class UiState {
        public final int dotState;
        public final String pillText;
        public final String titleText;
        public final String summaryText;
        public final String detailsText;
        public final String primaryLabel;
        public final boolean primaryEnabled;
        public final String secondaryLabel;
        public final boolean showSecondary;
        public final int phase;
        public final String resolvedUrl;

        public UiState(
                int dotState,
                String pillText,
                String titleText,
                String summaryText,
                String detailsText,
                String primaryLabel,
                boolean primaryEnabled,
                String secondaryLabel,
                boolean showSecondary,
                int phase,
                String resolvedUrl) {
            this.dotState = dotState;
            this.pillText = pillText;
            this.titleText = titleText;
            this.summaryText = summaryText;
            this.detailsText = detailsText;
            this.primaryLabel = primaryLabel;
            this.primaryEnabled = primaryEnabled;
            this.secondaryLabel = secondaryLabel;
            this.showSecondary = showSecondary;
            this.phase = phase;
            this.resolvedUrl = resolvedUrl;
        }
    }

    private final Context context;
    private final Listener listener;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ExecutorService executor = Executors.newCachedThreadPool();
    private final ConnectivityManager connectivityManager;
    private final WifiManager wifiManager;
    private final AtomicInteger attemptCounter = new AtomicInteger(0);

    private volatile DeviceProfile activeProfile;
    private volatile ConnectionSession session;
    private volatile AtomicBoolean probeCancelled = new AtomicBoolean(false);
    private volatile boolean pendingWifiPermission = false;
    private volatile UiState currentState;

    public ConnectionManager(Context context, DeviceProfile initialProfile, Listener listener) {
        this.context = context.getApplicationContext();
        this.activeProfile = initialProfile;
        this.listener = listener;
        this.connectivityManager =
                (ConnectivityManager) this.context.getSystemService(Context.CONNECTIVITY_SERVICE);
        this.wifiManager =
                (WifiManager) this.context.getSystemService(Context.WIFI_SERVICE);
        this.currentState = idleStateFor(initialProfile);
    }

    public DeviceProfile profile() {
        return activeProfile;
    }

    public int phase() {
        ConnectionSession s = session;
        return s == null ? ConnectionSession.PHASE_IDLE : s.phase;
    }

    public UiState state() {
        return currentState;
    }

    public void updateProfile(DeviceProfile updated, boolean reconnectImmediately) {
        this.activeProfile = updated;
        if (reconnectImmediately) {
            stop();
            connect("切换配置并重连");
        } else {
            publish(idleStateFor(updated));
        }
    }

    public void connectIfIdle(String reason) {
        if (LaunchConnectionPolicy.connectOnLaunch(activeProfile.autoConnect, phase())) {
            connect(reason);
        }
    }

    public void connect(String reason) {
        stopSessionOnly();
        final DeviceProfile profile = this.activeProfile;
        final int attemptId = attemptCounter.incrementAndGet();
        final ConnectionSession s = new ConnectionSession(attemptId, profile);
        final AtomicBoolean cancelToken = new AtomicBoolean(false);
        this.session = s;
        this.probeCancelled = cancelToken;

        if (profile.mode == DeviceProfile.MODE_HOTSPOT_RELAY) {
            connectHotspotRelay(s, cancelToken);
            return;
        }
        connectApMode(s, cancelToken, reason);
    }

    public void stop() {
        attemptCounter.incrementAndGet();
        stopSessionOnly();
        publish(idleStateFor(activeProfile));
    }

    public boolean needsWifiPermissionForActive() {
        return PermissionPolicy.needsPermissionForProfile(activeProfile)
                && !PermissionPolicy.hasWifiPermission(context);
    }

    public void onPermissionResult(boolean granted) {
        if (pendingWifiPermission) {
            pendingWifiPermission = false;
            if (granted) {
                connect("Wi-Fi 权限已授权");
            } else {
                DeviceProfile p = activeProfile;
                publish(new UiState(
                        DeviceProfile.STATUS_DOT_OFFLINE,
                        "需要手动连接",
                        "未授予附近 Wi-Fi 权限",
                        "请在系统 Wi-Fi 中手动连接 " + p.ssid + " 后点击「立即探测连接」。",
                        "您拒绝了 Wi-Fi 自动切换权限。AP 地址 " + p.resolvedHomeUrl() + " 仍可通过手动连接 Wi-Fi 使用，或改用「手机热点中继」模式。",
                        "立即探测连接",
                        true,
                        context.getString(R.string.open_wifi_settings),
                        true,
                        ConnectionSession.PHASE_FAILED,
                        p.resolvedHomeUrl()));
            }
        }
    }

    public void checkHealth(final String currentUrl, final HealthCallback cb) {
        final ConnectionSession s = this.session;
        final Network bound = (s != null && activeProfile.requiresDeviceApWifi()) ? s.boundNetwork : null;
        executor.execute(new Runnable() {
            @Override
            public void run() {
                String probeTarget = (currentUrl != null && !currentUrl.isEmpty())
                        ? currentUrl
                        : activeProfile.resolvedHomeUrl();
                boolean ok = !verifyDeviceHttp(bound, probeTarget, 900, 1100).isEmpty();
                mainHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        cb.onResult(ok);
                    }
                });
            }
        });
    }

    public void destroy() {
        attemptCounter.incrementAndGet();
        stopSessionOnly();
        executor.shutdownNow();
    }

    public interface HealthCallback {
        void onResult(boolean healthy);
    }

    private void connectHotspotRelay(final ConnectionSession s, final AtomicBoolean cancelToken) {
        s.phase = ConnectionSession.PHASE_PROBE;
        // Ensure we use the phone's normal routing (Cellular + Hotspot tether), not an old Wi-Fi binding.
        bindProcess(null);

        publish(new UiState(
                DeviceProfile.STATUS_DOT_CONNECTING,
                "热点扫描中…",
                "正在手机热点中寻找 ESP32",
                "正在通过 UDP 42100 与热点子网发现 " + s.profile.name + "，手机可继续使用热点与移动网络…",
                "连接模式：手机热点中继（无需切换手机 Wi-Fi）\n手动指定/历史地址："
                        + (s.profile.resolvedHomeUrl().isEmpty() ? "自动发现" : s.profile.resolvedHomeUrl()),
                "停止连接",
                true,
                "手动指定 IP",
                true,
                ConnectionSession.PHASE_PROBE,
                s.profile.resolvedHomeUrl()));

        executor.execute(new Runnable() {
            @Override
            public void run() {
                StringBuilder diag = new StringBuilder();
                String discovered = HotspotDiscovery.discoverOnHotspot(s.profile, cancelToken, diag);
                if (!s.active(attemptCounter.get()) || cancelToken.get()) {
                    return;
                }
                if (!discovered.isEmpty()) {
                    s.resolvedUrl = discovered;
                    s.phase = ConnectionSession.PHASE_CONNECTED;
                    final DeviceProfile updated = s.profile.withLastDiscoveredUrl(discovered);
                    activeProfile = updated;
                    mainHandler.post(new Runnable() {
                        @Override
                        public void run() {
                            if (!s.active(attemptCounter.get())) {
                                return;
                            }
                            listener.onProfileUpdated(updated);
                            publish(new UiState(
                                    DeviceProfile.STATUS_DOT_ONLINE,
                                    "热点已连接",
                                    "已发现 ESP32 热点设备",
                                    "设备地址：" + discovered + "\n正在打开控制台…",
                                    "连接方式：手机热点中继\n目标地址：" + discovered + "\n诊断：" + diag,
                                    "打开控制台",
                                    true,
                                    "重新扫描",
                                    true,
                                    ConnectionSession.PHASE_CONNECTED,
                                    discovered));
                            listener.onReady(discovered);
                        }
                    });
                } else {
                    s.phase = ConnectionSession.PHASE_FAILED;
                    final String details = "模式：手机热点中继连接\n"
                            + "排查步骤：\n"
                            + "1. 确认手机个人热点已开启（支持 2.4GHz 更佳）。\n"
                            + "2. 确认 ESP32 后台「Wi-Fi 中继」已连上手机热点且在 3.35.3.3+ 固件下运行。\n"
                            + "3. 如知晓 ESP32 分配到的局域网 IP，可在「编辑」中直接填入（如 192.168.43.xx）。\n"
                            + "诊断输出：" + diag;
                    mainHandler.post(new Runnable() {
                        @Override
                        public void run() {
                            if (!s.active(attemptCounter.get())) {
                                return;
                            }
                            publish(new UiState(
                                    DeviceProfile.STATUS_DOT_OFFLINE,
                                    "未找到设备",
                                    "手机热点下未发现 P4-CAN",
                                    "请确认手机个人热点已打开且 ESP32 中继已连上，或在「编辑」中填入设备 IP。",
                                    details,
                                    context.getString(R.string.retry_connection),
                                    true,
                                    "手动填 IP",
                                    true,
                                    ConnectionSession.PHASE_FAILED,
                                    ""));
                        }
                    });
                }
            }
        });
    }

    private void connectApMode(final ConnectionSession s, final AtomicBoolean cancelToken, String reason) {
        final String targetUrl = s.profile.resolvedHomeUrl();
        final String hostIp = UrlPolicy.hostLabel(targetUrl);
        s.resolvedUrl = targetUrl;
        s.phase = ConnectionSession.PHASE_WIFI;

        publish(new UiState(
                DeviceProfile.STATUS_DOT_CONNECTING,
                context.getString(R.string.status_connecting),
                "正在连接 " + hostIp + "…",
                "目标地址：" + targetUrl + "\n正在探测已连 Wi-Fi 或切换到 "
                        + (s.profile.ssid.isEmpty() ? "当前热点" : s.profile.ssid) + "…",
                "连接模式：" + s.profile.modeShortLabel() + "\n目标地址：" + targetUrl + "\n触发：" + reason,
                "停止连接",
                true,
                context.getString(R.string.open_wifi_settings),
                true,
                ConnectionSession.PHASE_WIFI,
                targetUrl));

        executor.execute(new Runnable() {
            @Override
            public void run() {
                // 1. Check if already on an active Wi-Fi network matching this ESP32 host.
                Network preBound = findMatchingWifiNetwork(hostIp);
                if (preBound != null && !cancelToken.get() && s.active(attemptCounter.get())) {
                    String ok = verifyDeviceHttp(preBound, targetUrl, 700, 900);
                    if (!ok.isEmpty()) {
                        completeApSuccess(s, preBound, ok, "已复用当前已连接的设备 Wi-Fi");
                        return;
                    }
                }
                // Also test direct default route (in case VPN/local routing already reaches 192.168.4.1 / 100.100.1.1).
                if (!cancelToken.get() && s.active(attemptCounter.get())) {
                    String okDefault = verifyDeviceHttp(null, targetUrl, 600, 800);
                    if (!okDefault.isEmpty()) {
                        completeApSuccess(s, preBound, okDefault, "直接路由访问设备成功");
                        return;
                    }
                }

                if (!s.active(attemptCounter.get()) || cancelToken.get()) {
                    return;
                }

                // 2. If user wants auto Wi-Fi specifier switch to ESP32 AP SSID.
                if (s.profile.canAutoConnect()) {
                    mainHandler.post(new Runnable() {
                        @Override
                        public void run() {
                            if (!s.active(attemptCounter.get())) {
                                return;
                            }
                            if (wifiManager != null && !wifiManager.isWifiEnabled()) {
                                failApSession(
                                        s,
                                        "手机 Wi-Fi 未开启",
                                        "请先打开手机 Wi-Fi 后重试，或在右上编辑切换至「手机热点中继」模式。");
                                return;
                            }
                            if (!PermissionPolicy.hasWifiPermission(context)) {
                                pendingWifiPermission = true;
                                listener.onMessage("NEED_WIFI_PERMISSION");
                                return;
                            }
                            requestWifiSpecifierNetwork(s, cancelToken, targetUrl);
                        }
                    });
                } else {
                    mainHandler.post(new Runnable() {
                        @Override
                        public void run() {
                            if (!s.active(attemptCounter.get())) {
                                return;
                            }
                            failApSession(
                                    s,
                                    "无法访问 " + targetUrl,
                                    "当前未填写热点 SSID 或设为手动连接。请先在系统 Wi-Fi 设置中连接 ESP32 热点后点击「重试连接」。");
                        }
                    });
                }
            }
        });
    }

    private void requestWifiSpecifierNetwork(
            final ConnectionSession s, final AtomicBoolean cancelToken, final String targetUrl) {
        if (android.os.Build.VERSION.SDK_INT < 29) {
            failApSession(
                    s,
                    "需要手动连接 Wi-Fi",
                    "Android 9 及以下请先在系统 Wi-Fi 设置连接 " + s.profile.ssid + " 后再点击重试。");
            return;
        }
        if (connectivityManager == null) {
            failApSession(s, "系统网络服务不可用", "无法请求 Wi-Fi 连接。");
            return;
        }

        WifiNetworkSpecifier.Builder specBuilder = new WifiNetworkSpecifier.Builder()
                .setSsid(s.profile.ssid);
        if ("WPA3".equalsIgnoreCase(s.profile.security) && s.profile.password != null && !s.profile.password.isEmpty()) {
            specBuilder.setWpa3Passphrase(s.profile.password);
        } else if (!"Open".equalsIgnoreCase(s.profile.security)
                && s.profile.password != null
                && !s.profile.password.isEmpty()) {
            specBuilder.setWpa2Passphrase(s.profile.password);
        }

        NetworkRequest request = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .setNetworkSpecifier(specBuilder.build())
                .build();

        final Runnable timeoutTask = new Runnable() {
            @Override
            public void run() {
                if (!s.active(attemptCounter.get()) || s.phase == ConnectionSession.PHASE_CONNECTED) {
                    return;
                }
                unregisterSessionCallback(s);
                failApSession(
                        s,
                        "连接 " + s.profile.ssid + " 超时",
                        "请检查模块供电、SSID（「" + s.profile.ssid + "」）与密码是否正确；或手动连好热点后重试。");
            }
        };
        s.timeoutRunnable = timeoutTask;
        mainHandler.postDelayed(timeoutTask, 22000L);

        ConnectivityManager.NetworkCallback cb = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(final Network network) {
                if (!s.active(attemptCounter.get()) || cancelToken.get()) {
                    return;
                }
                s.boundNetwork = network;
                s.phase = ConnectionSession.PHASE_PROBE;
                bindProcess(network);
                mainHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        if (!s.active(attemptCounter.get())) {
                            return;
                        }
                        publish(new UiState(
                                DeviceProfile.STATUS_DOT_CONNECTING,
                                "验证设备中…",
                                "已接入 " + s.profile.ssid,
                                "正在校验 " + targetUrl + "api/profile/status …",
                                "目标地址：" + targetUrl,
                                "停止连接",
                                true,
                                context.getString(R.string.open_wifi_settings),
                                true,
                                ConnectionSession.PHASE_PROBE,
                                targetUrl));
                    }
                });
                executor.execute(new Runnable() {
                    @Override
                    public void run() {
                        String found = "";
                        for (int retry = 0; retry < 4 && found.isEmpty() && s.active(attemptCounter.get()) && !cancelToken.get(); retry++) {
                            found = verifyDeviceHttp(network, targetUrl, 1000, 1200);
                            if (found.isEmpty()) {
                                try {
                                    Thread.sleep(ConnectionRetryPolicy.delayForAttempt(retry + 1));
                                } catch (InterruptedException ignored) {
                                    break;
                                }
                            }
                        }
                        if (!s.active(attemptCounter.get()) || cancelToken.get()) {
                            return;
                        }
                        if (!found.isEmpty()) {
                            mainHandler.removeCallbacks(timeoutTask);
                            completeApSuccess(s, network, found, "Wi-Fi 热点接入并验证成功");
                        } else {
                            mainHandler.removeCallbacks(timeoutTask);
                            unregisterSessionCallback(s);
                            failApSession(
                                    s,
                                    "连上热点但无法访问 " + targetUrl,
                                    "请确认固件内置后台 IP 设置与 App 所选地址（"
                                            + UrlPolicy.hostLabel(targetUrl)
                                            + "）是否一致！");
                        }
                    }
                });
            }

            @Override
            public void onUnavailable() {
                if (!s.active(attemptCounter.get())) {
                    return;
                }
                mainHandler.removeCallbacks(timeoutTask);
                unregisterSessionCallback(s);
                failApSession(
                        s,
                        "未能连接到 " + s.profile.ssid,
                        "系统 Wi-Fi 弹窗取消或未扫描到热点，请确认 ESP32 已上电或手动连接 Wi-Fi 后重试。");
            }

            @Override
            public void onLost(Network network) {
                if (s.active(attemptCounter.get()) && s.phase == ConnectionSession.PHASE_CONNECTED) {
                    bindProcess(null);
                }
            }
        };
        s.networkCallback = cb;
        try {
            connectivityManager.requestNetwork(request, cb);
        } catch (Exception e) {
            mainHandler.removeCallbacks(timeoutTask);
            failApSession(s, "Wi-Fi 连接请求失败", String.valueOf(e.getMessage()));
        }
    }

    private void completeApSuccess(
            final ConnectionSession s, final Network network, final String resolvedUrl, final String note) {
        if (network != null) {
            s.boundNetwork = network;
            bindProcess(network);
        }
        s.resolvedUrl = resolvedUrl;
        s.phase = ConnectionSession.PHASE_CONNECTED;
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                if (!s.active(attemptCounter.get())) {
                    return;
                }
                publish(new UiState(
                        DeviceProfile.STATUS_DOT_ONLINE,
                        "已连接",
                        s.profile.name,
                        context.getString(R.string.connection_loading),
                        "模式：" + s.profile.modeShortLabel() + "\n地址：" + resolvedUrl + "\n状态：" + note,
                        "打开控制台",
                        true,
                        context.getString(R.string.open_wifi_settings),
                        false,
                        ConnectionSession.PHASE_CONNECTED,
                        resolvedUrl));
                listener.onReady(resolvedUrl);
            }
        });
    }

    private void failApSession(final ConnectionSession s, final String title, final String details) {
        s.phase = ConnectionSession.PHASE_FAILED;
        bindProcess(null);
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                if (!s.active(attemptCounter.get())) {
                    return;
                }
                publish(new UiState(
                        DeviceProfile.STATUS_DOT_OFFLINE,
                        context.getString(R.string.status_disconnected),
                        title,
                        details,
                        "目标地址：" + s.resolvedUrl + "\nSSID：" + s.profile.ssid + "\n详情：" + details,
                        context.getString(R.string.retry_connection),
                        true,
                        context.getString(R.string.open_wifi_settings),
                        true,
                        ConnectionSession.PHASE_FAILED,
                        s.resolvedUrl));
            }
        });
    }

    private Network findMatchingWifiNetwork(String hostIp) {
        if (connectivityManager == null) {
            return null;
        }
        Network fallbackWifi = null;
        try {
            Network[] all = connectivityManager.getAllNetworks();
            for (Network net : all) {
                NetworkCapabilities caps = connectivityManager.getNetworkCapabilities(net);
                if (caps == null || !caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
                    continue;
                }
                LinkProperties lp = connectivityManager.getLinkProperties(net);
                if (lp != null && hostIp != null && !hostIp.isEmpty()) {
                    for (InetAddress dns : lp.getDnsServers()) {
                        if (hostIp.equals(dns.getHostAddress())) {
                            return net;
                        }
                    }
                    for (RouteInfo route : lp.getRoutes()) {
                        InetAddress gw = route.getGateway();
                        if (gw != null && hostIp.equals(gw.getHostAddress())) {
                            return net;
                        }
                    }
                    int dot = hostIp.lastIndexOf('.');
                    if (dot > 0) {
                        String prefix = hostIp.substring(0, dot + 1);
                        if (lp.getLinkAddresses() != null && !lp.getLinkAddresses().isEmpty()) {
                            String local = lp.getLinkAddresses().get(0).getAddress().getHostAddress();
                            if (local != null && local.startsWith(prefix)) {
                                return net;
                            }
                        }
                    }
                }
                if (!caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED) || fallbackWifi == null) {
                    fallbackWifi = net;
                }
            }
        } catch (Exception ignored) {
        }
        return fallbackWifi;
    }

    private String verifyDeviceHttp(Network network, String baseUrl, int connectMs, int readMs) {
        String norm = UrlPolicy.normalize(baseUrl);
        if (norm.isEmpty()) {
            return "";
        }
        HttpURLConnection conn = null;
        try {
            URI uri = URI.create(norm + "api/profile/status");
            conn = (HttpURLConnection) (network != null
                    ? network.openConnection(uri.toURL())
                    : uri.toURL().openConnection());
            conn.setRequestMethod("GET");
            conn.setConnectTimeout(connectMs);
            conn.setReadTimeout(readMs);
            conn.setUseCaches(false);
            conn.setInstanceFollowRedirects(false);
            if (conn.getResponseCode() != 200) {
                return "";
            }
            StringBuilder sb = new StringBuilder();
            try (BufferedReader br = new BufferedReader(
                    new InputStreamReader(conn.getInputStream(), StandardCharsets.UTF_8))) {
                char[] buf = new char[512];
                int n;
                while ((n = br.read(buf)) > 0 && sb.length() < 4096) {
                    sb.append(buf, 0, n);
                }
            }
            JSONObject obj = new JSONObject(sb.toString());
            if (obj.has("setupMode") && obj.has("profile")) {
                return norm;
            }
        } catch (Exception ignored) {
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
        return "";
    }

    private void bindProcess(Network network) {
        if (connectivityManager != null) {
            try {
                connectivityManager.bindProcessToNetwork(network);
            } catch (Exception ignored) {
            }
        }
    }

    private void unregisterSessionCallback(ConnectionSession s) {
        if (connectivityManager != null && s.networkCallback != null) {
            try {
                connectivityManager.unregisterNetworkCallback(s.networkCallback);
            } catch (Exception ignored) {
            }
            s.networkCallback = null;
        }
    }

    private void stopSessionOnly() {
        AtomicBoolean c = this.probeCancelled;
        if (c != null) {
            c.set(true);
        }
        ConnectionSession prev = this.session;
        if (prev != null) {
            prev.cancelled = true;
            if (prev.timeoutRunnable != null) {
                mainHandler.removeCallbacks(prev.timeoutRunnable);
                prev.timeoutRunnable = null;
            }
            unregisterSessionCallback(prev);
        }
        this.session = null;
        bindProcess(null);
    }

    private UiState idleStateFor(DeviceProfile profile) {
        if (profile.mode == DeviceProfile.MODE_HOTSPOT_RELAY) {
            return new UiState(
                    DeviceProfile.STATUS_DOT_OFFLINE,
                    "待连接",
                    profile.name,
                    "已选择「手机热点中继连接」\n请保持手机热点开启并确认 ESP32 已连上该热点，然后点击下方开始发现并连接。",
                    "模式：手机热点中继\n目标："
                            + (profile.resolvedHomeUrl().isEmpty() ? "自动发现" : profile.resolvedHomeUrl()),
                    "扫描并连接热点设备",
                    true,
                    "手动填 IP",
                    true,
                    ConnectionSession.PHASE_IDLE,
                    profile.resolvedHomeUrl());
        }
        String target = profile.resolvedHomeUrl();
        return new UiState(
                DeviceProfile.STATUS_DOT_OFFLINE,
                context.getString(R.string.status_disconnected),
                profile.name,
                "目标控制台地址：" + target + "\n点击下方连接到 P4-CAN / T2CAN 设备。",
                "模式：" + profile.modeShortLabel() + "\nSSID：" + profile.ssid + "\n目标地址：" + target,
                "连接设备",
                true,
                context.getString(R.string.open_wifi_settings),
                true,
                ConnectionSession.PHASE_IDLE,
                target);
    }

    private void publish(final UiState next) {
        this.currentState = next;
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                listener.onStateChanged(next);
            }
        });
    }
}
