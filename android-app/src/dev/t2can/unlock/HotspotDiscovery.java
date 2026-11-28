package dev.t2can.unlock;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Enumeration;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;

public final class HotspotDiscovery {
    public static final int DISCOVER_UDP_PORT = 42100;
    private static final byte[] DISCOVER_MAGIC = "P4CAN_DISCOVER".getBytes(StandardCharsets.US_ASCII);

    private HotspotDiscovery() {
    }

    public static String probeP4CanHttp(String baseUrl, int connectMs, int readMs) {
        String norm = UrlPolicy.normalize(baseUrl);
        if (norm.isEmpty()) {
            return "";
        }
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) URI.create(norm + "api/profile/status").toURL().openConnection();
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

    public static String discoverOnHotspot(
            DeviceProfile profile,
            AtomicBoolean cancelled,
            StringBuilder diagOut) {
        if (cancelled.get()) {
            return "";
        }
        // 1. Try explicit configured URL or last discovered hotspot IP first (< 500ms).
        Set<String> quickCandidates = new LinkedHashSet<>();
        if (profile.url != null && !profile.url.isEmpty()) {
            quickCandidates.add(profile.url);
        }
        if (profile.lastDiscoveredUrl != null && !profile.lastDiscoveredUrl.isEmpty()) {
            quickCandidates.add(profile.lastDiscoveredUrl);
        }
        for (String candidate : quickCandidates) {
            if (cancelled.get()) {
                return "";
            }
            String found = probeP4CanHttp(candidate, 550, 750);
            if (!found.isEmpty()) {
                diagOut.append("命中历史热点 IP: ").append(found);
                return found;
            }
        }

        // 2. Try UDP broadcast discovery on port 42100 across hotspot / local interfaces.
        String udpHit = discoverViaUdp(cancelled, diagOut);
        if (!udpHit.isEmpty() || cancelled.get()) {
            return udpHit;
        }

        // 3. Fallback: parallel /24 HTTP probe on active non-cellular local subnets.
        return discoverViaSubnetScan(cancelled, diagOut);
    }

    private static String discoverViaUdp(AtomicBoolean cancelled, StringBuilder diagOut) {
        DatagramSocket socket = null;
        try {
            socket = new DatagramSocket();
            socket.setBroadcast(true);
            socket.setSoTimeout(160);

            Set<InetAddress> targets = new LinkedHashSet<>();
            targets.add(InetAddress.getByName("255.255.255.255"));
            for (SubnetInfo info : collectHotspotSubnets()) {
                if (info.broadcast != null) {
                    targets.add(info.broadcast);
                } else {
                    try {
                        targets.add(InetAddress.getByName(info.prefix24 + "255"));
                    } catch (Exception ignored) {
                    }
                }
            }

            for (int round = 0; round < 2 && !cancelled.get(); round++) {
                for (InetAddress target : targets) {
                    try {
                        socket.send(new DatagramPacket(
                                DISCOVER_MAGIC, DISCOVER_MAGIC.length, target, DISCOVER_UDP_PORT));
                    } catch (Exception ignored) {
                    }
                }
                long deadline = System.currentTimeMillis() + 340L;
                byte[] rxBuf = new byte[512];
                while (System.currentTimeMillis() < deadline && !cancelled.get()) {
                    try {
                        DatagramPacket pkt = new DatagramPacket(rxBuf, rxBuf.length);
                        socket.receive(pkt);
                        String body = new String(
                                pkt.getData(), pkt.getOffset(), pkt.getLength(), StandardCharsets.UTF_8);
                        JSONObject json = new JSONObject(body);
                        if (!json.optBoolean("p4can", false)) {
                            continue;
                        }
                        String staIp = json.optString("staIp", "").trim();
                        String senderIp = pkt.getAddress() != null ? pkt.getAddress().getHostAddress() : "";
                        String candidateIp = !staIp.isEmpty() ? staIp : senderIp;
                        String probed = probeP4CanHttp("http://" + candidateIp + "/", 650, 800);
                        if (!probed.isEmpty()) {
                            diagOut.append("UDP 42100 发现设备: ").append(probed);
                            return probed;
                        }
                        if (!senderIp.isEmpty() && !senderIp.equals(candidateIp)) {
                            String senderProbe = probeP4CanHttp("http://" + senderIp + "/", 650, 800);
                            if (!senderProbe.isEmpty()) {
                                diagOut.append("UDP 42100 来源发现设备: ").append(senderProbe);
                                return senderProbe;
                            }
                        }
                    } catch (SocketTimeoutException ignored) {
                    } catch (Exception ignored) {
                    }
                }
            }
        } catch (Exception ignored) {
        } finally {
            if (socket != null) {
                socket.close();
            }
        }
        return "";
    }

    private static String discoverViaSubnetScan(AtomicBoolean cancelled, StringBuilder diagOut) {
        List<SubnetInfo> subnets = collectHotspotSubnets();
        if (subnets.isEmpty()) {
            diagOut.append("未检测到手机个人热点或局域网网段");
            return "";
        }
        diagOut.append("正在扫描热点网段: ");
        for (int i = 0; i < subnets.size(); i++) {
            if (i > 0) {
                diagOut.append(", ");
            }
            diagOut.append(subnets.get(i).prefix24).append("0/24");
        }

        final AtomicReference<String> winner = new AtomicReference<>("");
        ExecutorService pool = Executors.newFixedThreadPool(24);
        try {
            List<String> targets = new ArrayList<>();
            for (SubnetInfo sub : subnets) {
                for (int host = 2; host <= 254; host++) {
                    if (host == sub.selfLastOctet) {
                        continue;
                    }
                    targets.add("http://" + sub.prefix24 + host + "/");
                }
                if (sub.selfLastOctet != 1) {
                    targets.add("http://" + sub.prefix24 + "1/");
                }
            }

            List<Callable<Void>> tasks = new ArrayList<>(targets.size());
            for (final String url : targets) {
                tasks.add(new Callable<Void>() {
                    @Override
                    public Void call() {
                        if (cancelled.get() || !winner.get().isEmpty()) {
                            return null;
                        }
                        String ok = probeP4CanHttp(url, 220, 450);
                        if (!ok.isEmpty()) {
                            winner.compareAndSet("", ok);
                        }
                        return null;
                    }
                });
            }
            List<Future<Void>> futures = new ArrayList<>(tasks.size());
            for (Callable<Void> task : tasks) {
                if (cancelled.get() || !winner.get().isEmpty()) {
                    break;
                }
                futures.add(pool.submit(task));
            }
            for (Future<Void> f : futures) {
                if (cancelled.get() || !winner.get().isEmpty()) {
                    break;
                }
                try {
                    f.get();
                } catch (Exception ignored) {
                }
            }
        } finally {
            pool.shutdownNow();
        }
        return winner.get();
    }

    private static List<SubnetInfo> collectHotspotSubnets() {
        List<SubnetInfo> out = new ArrayList<>();
        Set<String> seenPrefixes = new LinkedHashSet<>();
        try {
            Enumeration<NetworkInterface> nifs = NetworkInterface.getNetworkInterfaces();
            if (nifs == null) {
                return out;
            }
            while (nifs.hasMoreElements()) {
                NetworkInterface nif = nifs.nextElement();
                if (!nif.isUp() || nif.isLoopback()) {
                    continue;
                }
                String name = nif.getName() == null ? "" : nif.getName().toLowerCase(Locale.US);
                if (isCellularOrTunnelInterface(name)) {
                    continue;
                }
                for (InterfaceAddress ia : nif.getInterfaceAddresses()) {
                    InetAddress addr = ia.getAddress();
                    if (!(addr instanceof Inet4Address) || addr.isLoopbackAddress()) {
                        continue;
                    }
                    String ip = addr.getHostAddress();
                    if (!UrlPolicy.isAllowedDeviceIp(ip)) {
                        continue;
                    }
                    int lastDot = ip.lastIndexOf('.');
                    if (lastDot <= 0) {
                        continue;
                    }
                    String prefix = ip.substring(0, lastDot + 1);
                    if (!seenPrefixes.add(prefix)) {
                        continue;
                    }
                    int lastOctet = 0;
                    try {
                        lastOctet = Integer.parseInt(ip.substring(lastDot + 1));
                    } catch (NumberFormatException ignored) {
                    }
                    out.add(new SubnetInfo(name, prefix, lastOctet, ia.getBroadcast(), priorityScore(name)));
                }
            }
        } catch (Exception ignored) {
        }
        Collections.sort(out, new Comparator<SubnetInfo>() {
            @Override
            public int compare(SubnetInfo a, SubnetInfo b) {
                return Integer.compare(a.priority, b.priority);
            }
        });
        return out;
    }

    private static boolean isCellularOrTunnelInterface(String name) {
        return name.startsWith("rmnet")
                || name.startsWith("ccmni")
                || name.startsWith("pdp")
                || name.startsWith("seth")
                || name.startsWith("v4-rmnet")
                || name.startsWith("tun")
                || name.startsWith("ppp")
                || name.startsWith("clat")
                || name.startsWith("dummy");
    }

    private static int priorityScore(String name) {
        if (name.startsWith("ap") || name.startsWith("swlan") || name.startsWith("softap") || name.contains("tether")) {
            return 0;
        }
        if (name.startsWith("wlan") || name.startsWith("rndis") || name.startsWith("usb")) {
            return 1;
        }
        return 2;
    }

    private static final class SubnetInfo {
        final String ifName;
        final String prefix24;
        final int selfLastOctet;
        final InetAddress broadcast;
        final int priority;

        SubnetInfo(String ifName, String prefix24, int selfLastOctet, InetAddress broadcast, int priority) {
            this.ifName = ifName;
            this.prefix24 = prefix24;
            this.selfLastOctet = selfLastOctet;
            this.broadcast = broadcast;
            this.priority = priority;
        }
    }
}
