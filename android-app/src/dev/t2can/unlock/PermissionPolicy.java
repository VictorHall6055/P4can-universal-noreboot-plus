package dev.t2can.unlock;

import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;

public final class PermissionPolicy {
    private PermissionPolicy() {
    }

    public static String permission() {
        if (Build.VERSION.SDK_INT >= 33) {
            return "android.permission.NEARBY_WIFI_DEVICES";
        }
        return "android.permission.ACCESS_FINE_LOCATION";
    }

    public static boolean needsPermissionForProfile(DeviceProfile profile) {
        return profile != null && profile.canAutoConnect();
    }

    public static boolean hasWifiPermission(Context context) {
        return context.checkSelfPermission(permission()) == PackageManager.PERMISSION_GRANTED;
    }
}
