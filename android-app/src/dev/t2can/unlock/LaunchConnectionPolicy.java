package dev.t2can.unlock;

public final class LaunchConnectionPolicy {
    private LaunchConnectionPolicy() {
    }

    public static boolean connectOnLaunch(boolean autoEnabled, int phase) {
        return autoEnabled && phase == ConnectionSession.PHASE_IDLE;
    }
}
