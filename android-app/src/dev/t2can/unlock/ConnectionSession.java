package dev.t2can.unlock;

import android.net.ConnectivityManager;
import android.net.Network;

public final class ConnectionSession {
    public static final int PHASE_IDLE = 0;
    public static final int PHASE_WIFI = 1;
    public static final int PHASE_PROBE = 2;
    public static final int PHASE_CONNECTED = 3;
    public static final int PHASE_FAILED = 4;

    public final int attemptId;
    public final DeviceProfile profile;
    public volatile int phase = PHASE_IDLE;
    public volatile String resolvedUrl = "";
    public ConnectivityManager.NetworkCallback networkCallback;
    public Network boundNetwork;
    public Runnable timeoutRunnable;
    public volatile boolean cancelled;

    public ConnectionSession(int attemptId, DeviceProfile profile) {
        this.attemptId = attemptId;
        this.profile = profile;
        this.resolvedUrl = profile.resolvedHomeUrl();
    }

    public boolean active(int currentAttemptId) {
        return !cancelled && attemptId == currentAttemptId;
    }
}
