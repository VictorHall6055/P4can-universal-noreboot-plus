package dev.t2can.unlock;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.ArrayList;
import java.util.List;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

public final class ProfileRepository {
    private static final String KEY_ALIAS = "t2can_profiles_key";
    private static final String PREFS_NAME = "t2can_profiles";
    private static final String PROFILES_KEY = "profiles";
    private static final String SELECTED_KEY = "selected_profile";
    private static final String PREFIX_ENC = "encrypted_v1:";
    private static final String PREFIX_PLAIN = "plain_v1:";
    private static final int GCM_IV_LEN = 12;
    private static final int GCM_TAG_BITS = 128;

    private final SharedPreferences preferences;

    public ProfileRepository(Context context) {
        this.preferences = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    }

    public List<DeviceProfile> load() {
        String raw = preferences.getString(PROFILES_KEY, null);
        if (raw == null || raw.trim().isEmpty()) {
            return createDefaults();
        }
        try {
            String json = decodeStoredPayload(raw);
            if (json == null || json.trim().isEmpty()) {
                return createDefaults();
            }
            JSONArray array = new JSONArray(json);
            List<DeviceProfile> list = new ArrayList<>();
            for (int i = 0; i < array.length(); i++) {
                JSONObject o = array.optJSONObject(i);
                if (o == null) {
                    continue;
                }
                String rawUrl = o.optString("url", UrlPolicy.DEFAULT_HOME);
                int mode = o.has("mode")
                        ? DeviceProfile.clampMode(o.optInt("mode", DeviceProfile.MODE_AP_DEFAULT))
                        : DeviceProfile.inferModeFromUrl(rawUrl);
                String normUrl = UrlPolicy.normalizeForMode(mode, rawUrl);
                if (mode != DeviceProfile.MODE_HOTSPOT_RELAY && normUrl.isEmpty()) {
                    normUrl = UrlPolicy.defaultHomeForMode(mode);
                }
                list.add(new DeviceProfile(
                        o.optString("id", "default"),
                        o.optString("name", "P4-CAN / T2CAN"),
                        o.optString("ssid", "P4-CAN-PRO"),
                        o.optString("security", "WPA2"),
                        o.optString("password", "12345678"),
                        normUrl,
                        mode,
                        UrlPolicy.normalize(o.optString("lastDiscoveredUrl", "")),
                        o.optBoolean("autoConnect", true),
                        o.optBoolean("manual", false)));
            }
            return list.isEmpty() ? createDefaults() : list;
        } catch (Exception ignored) {
            return createDefaults();
        }
    }

    public DeviceProfile active() {
        List<DeviceProfile> list = load();
        String selected = preferences.getString(SELECTED_KEY, null);
        for (DeviceProfile profile : list) {
            if (profile.id.equals(selected)) {
                return profile;
            }
        }
        return list.get(0);
    }

    public void save(DeviceProfile updated) {
        List<DeviceProfile> list = new ArrayList<>(load());
        boolean replaced = false;
        for (int i = 0; i < list.size(); i++) {
            if (list.get(i).id.equals(updated.id)) {
                list.set(i, updated);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            list.add(0, updated);
        }
        persistList(list, updated.id);
    }

    public void select(String id) {
        preferences.edit().putString(SELECTED_KEY, id).apply();
    }

    private List<DeviceProfile> createDefaults() {
        List<DeviceProfile> defaults = new ArrayList<>();
        defaults.add(DeviceProfile.createDefault());
        return defaults;
    }

    private void persistList(List<DeviceProfile> list, String selectedId) {
        try {
            JSONArray array = new JSONArray();
            for (DeviceProfile profile : list) {
                JSONObject o = new JSONObject();
                o.put("id", profile.id);
                o.put("name", profile.name);
                o.put("ssid", profile.ssid);
                o.put("security", profile.security);
                o.put("password", profile.password);
                o.put("url", profile.url);
                o.put("mode", profile.mode);
                o.put("lastDiscoveredUrl", profile.lastDiscoveredUrl);
                o.put("autoConnect", profile.autoConnect);
                o.put("manual", profile.manual);
                array.put(o);
            }
            String serialized = encodeStoredPayload(array.toString());
            preferences.edit()
                    .putString(PROFILES_KEY, serialized)
                    .putString(SELECTED_KEY, selectedId)
                    .apply();
        } catch (JSONException ignored) {
        }
    }

    private SecretKey getOrCreateKey() throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        KeyStore.Entry entry = ks.getEntry(KEY_ALIAS, null);
        if (entry instanceof KeyStore.SecretKeyEntry) {
            return ((KeyStore.SecretKeyEntry) entry).getSecretKey();
        }
        KeyGenerator gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        gen.init(new KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .build());
        return gen.generateKey();
    }

    private String encodeStoredPayload(String plainJson) {
        try {
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey());
            byte[] iv = cipher.getIV();
            byte[] encrypted = cipher.doFinal(plainJson.getBytes(StandardCharsets.UTF_8));
            byte[] combined = new byte[iv.length + encrypted.length];
            System.arraycopy(iv, 0, combined, 0, iv.length);
            System.arraycopy(encrypted, 0, combined, iv.length, encrypted.length);
            return PREFIX_ENC + Base64.encodeToString(combined, Base64.NO_WRAP);
        } catch (Exception ignored) {
            return PREFIX_PLAIN + Base64.encodeToString(
                    plainJson.getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP);
        }
    }

    private String decodeStoredPayload(String stored) throws Exception {
        if (stored.startsWith(PREFIX_ENC)) {
            byte[] combined = Base64.decode(stored.substring(PREFIX_ENC.length()), Base64.DEFAULT);
            if (combined.length <= GCM_IV_LEN) {
                return null;
            }
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(
                    Cipher.DECRYPT_MODE,
                    getOrCreateKey(),
                    new GCMParameterSpec(GCM_TAG_BITS, combined, 0, GCM_IV_LEN));
            byte[] plain = cipher.doFinal(combined, GCM_IV_LEN, combined.length - GCM_IV_LEN);
            return new String(plain, StandardCharsets.UTF_8);
        }
        if (stored.startsWith(PREFIX_PLAIN)) {
            byte[] plain = Base64.decode(stored.substring(PREFIX_PLAIN.length()), Base64.DEFAULT);
            return new String(plain, StandardCharsets.UTF_8);
        }
        return stored;
    }
}
