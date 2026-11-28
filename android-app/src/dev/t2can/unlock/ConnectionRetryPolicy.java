package dev.t2can.unlock;

public final class ConnectionRetryPolicy {
    private ConnectionRetryPolicy() {
    }

    public static long delayForAttempt(int attempt) {
        if (attempt <= 1) {
            return 1000L;
        }
        return attempt == 2 ? 2000L : 4000L;
    }
}
