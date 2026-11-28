package dev.t2can.unlock;

import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import java.util.Arrays;
import java.util.List;

public final class ConnectionPanel {
    public interface Callback {
        void onSaved(DeviceProfile profile);
    }

    private static final List<String> MODE_OPTIONS = Arrays.asList(
            "1. 原始 AP 直连 (192.168.4.1)",
            "2. 手机热点中继 (免连 ESP32 热点)",
            "3. 伪公网 IP 直连 (100.100.1.1)"
    );

    private static final List<String> SECURITY_OPTIONS = Arrays.asList("WPA2", "WPA3", "Open");

    private ConnectionPanel() {
    }

    public static void edit(Context context, DeviceProfile profile, Callback callback) {
        final float density = context.getResources().getDisplayMetrics().density;
        int pad = Math.round(20f * density);

        LinearLayout root = new LinearLayout(context);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, Math.round(12f * density), pad, Math.round(8f * density));

        final Spinner modeSpinner = addSpinner(context, root, "连接方式", MODE_OPTIONS, profile.mode, density);
        final EditText nameEdit = addField(
                context, root, "设备名称", profile.name,
                InputType.TYPE_CLASS_TEXT, density);
        final EditText ssidEdit = addField(
                context, root, "Wi-Fi 名称 (SSID，AP 模式填写)", profile.ssid,
                InputType.TYPE_CLASS_TEXT, density);
        int secIdx = SECURITY_OPTIONS.indexOf(profile.security);
        if (secIdx < 0) {
            secIdx = 0;
        }
        final Spinner securitySpinner = addSpinner(
                context, root, "Wi-Fi 加密方式", SECURITY_OPTIONS, secIdx, density);
        final EditText passEdit = addField(
                context, root, "Wi-Fi 密码", profile.password,
                InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD, density);
        final EditText urlEdit = addField(
                context, root, "控制台地址 (IP / URL)", profile.url,
                InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI, density);

        final TextView noteView = new TextView(context);
        noteView.setTextSize(12f);
        noteView.setTextColor(context.getColor(R.color.connection_muted));
        noteView.setPadding(0, Math.round(10f * density), 0, 0);
        root.addView(noteView);

        ScrollView scroll = new ScrollView(context);
        scroll.addView(root);

        final AlertDialog dialog = new AlertDialog.Builder(context)
                .setTitle("编辑设备连接")
                .setView(scroll)
                .setNegativeButton(R.string.dialog_cancel, null)
                .setPositiveButton("保存", null)
                .create();

        final boolean[] programmaticChange = new boolean[]{false};

        final Runnable refreshUiState = new Runnable() {
            @Override
            public void run() {
                int mode = modeSpinner.getSelectedItemPosition();
                boolean isRelay = mode == DeviceProfile.MODE_HOTSPOT_RELAY;
                ssidEdit.setEnabled(!isRelay);
                securitySpinner.setEnabled(!isRelay);
                passEdit.setEnabled(!isRelay && securitySpinner.getSelectedItemPosition() != 2);

                if (isRelay) {
                    noteView.setText(
                            "手机热点中继模式：手机只需保持开启个人热点并让 ESP32 连上该热点，"
                                    + "App 将通过 UDP 42100 与局域网扫描自动发现设备。\n"
                                    + "上方控制台地址留空 = 自动发现；也可手动填写固定内网 IP（如 192.168.43.88）。");
                } else if (mode == DeviceProfile.MODE_AP_PUBLIC) {
                    noteView.setText(
                            "伪公网直连模式：连接 ESP32 热点后直接通过 http://100.100.1.1/ 访问，"
                                    + "无需 HTTPS 证书。仅手动连接热点时可将 SSID 留空。");
                } else {
                    noteView.setText(
                            "原始 AP 直连模式：自动或手动连接 ESP32 热点并访问 http://192.168.4.1/ "
                                    + "（也支持任意内网 IPv4）。仅手动连接热点时可将 SSID 留空。");
                }

                String urlText = urlEdit.getText() == null ? "" : urlEdit.getText().toString();
                String err = UrlPolicy.errorForMode(mode, urlText);
                if (err != null) {
                    urlEdit.setError(err);
                } else {
                    urlEdit.setError(null);
                }

                boolean canSave = err == null;
                if (!isRelay && securitySpinner.getSelectedItemPosition() != 2) {
                    String pass = passEdit.getText() == null ? "" : passEdit.getText().toString();
                    String ssid = ssidEdit.getText() == null ? "" : ssidEdit.getText().toString().trim();
                    if (!ssid.isEmpty() && !pass.isEmpty() && pass.length() < 8) {
                        canSave = false;
                    }
                }
                Button positive = dialog.getButton(DialogInterface.BUTTON_POSITIVE);
                if (positive != null) {
                    positive.setEnabled(canSave);
                }
            }
        };

        modeSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            private int lastMode = profile.mode;

            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int pos, long id) {
                if (pos != lastMode) {
                    programmaticChange[0] = true;
                    String curUrl = urlEdit.getText() == null ? "" : urlEdit.getText().toString().trim();
                    String norm = UrlPolicy.normalize(curUrl);
                    if (pos == DeviceProfile.MODE_AP_DEFAULT) {
                        if (curUrl.isEmpty() || UrlPolicy.PUBLIC_HOME.equals(norm)
                                || UrlPolicy.PUBLIC_HOST.equals(curUrl)) {
                            urlEdit.setText(UrlPolicy.DEFAULT_HOME);
                        }
                    } else if (pos == DeviceProfile.MODE_AP_PUBLIC) {
                        if (curUrl.isEmpty() || UrlPolicy.DEFAULT_HOME.equals(norm)
                                || UrlPolicy.DEFAULT_HOST.equals(curUrl)) {
                            urlEdit.setText(UrlPolicy.PUBLIC_HOME);
                        }
                    } else {
                        if (UrlPolicy.DEFAULT_HOME.equals(norm) || UrlPolicy.PUBLIC_HOME.equals(norm)
                                || UrlPolicy.DEFAULT_HOST.equals(curUrl)
                                || UrlPolicy.PUBLIC_HOST.equals(curUrl)) {
                            urlEdit.setText("");
                        }
                    }
                    programmaticChange[0] = false;
                    lastMode = pos;
                }
                refreshUiState.run();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });

        securitySpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int pos, long id) {
                refreshUiState.run();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });

        TextWatcher watcher = new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                if (!programmaticChange[0]) {
                    refreshUiState.run();
                }
            }
        };
        nameEdit.addTextChangedListener(watcher);
        ssidEdit.addTextChangedListener(watcher);
        passEdit.addTextChangedListener(watcher);
        urlEdit.addTextChangedListener(watcher);

        dialog.setOnShowListener(new DialogInterface.OnShowListener() {
            @Override
            public void onShow(DialogInterface d) {
                refreshUiState.run();
                Button saveBtn = dialog.getButton(DialogInterface.BUTTON_POSITIVE);
                if (saveBtn != null) {
                    saveBtn.setOnClickListener(new View.OnClickListener() {
                        @Override
                        public void onClick(View v) {
                            int mode = modeSpinner.getSelectedItemPosition();
                            String rawUrl = urlEdit.getText() == null ? "" : urlEdit.getText().toString();
                            if (!UrlPolicy.validHomeForMode(mode, rawUrl)) {
                                refreshUiState.run();
                                return;
                            }
                            String normalizedUrl = UrlPolicy.normalizeForMode(mode, rawUrl);
                            String name = nameEdit.getText() == null ? "" : nameEdit.getText().toString().trim();
                            if (name.isEmpty()) {
                                name = "P4-CAN / T2CAN";
                            }
                            String ssid = ssidEdit.getText() == null ? "" : ssidEdit.getText().toString().trim();
                            String sec = SECURITY_OPTIONS.get(securitySpinner.getSelectedItemPosition());
                            String pass = "Open".equals(sec) ? "" : (passEdit.getText() == null ? "" : passEdit.getText().toString());
                            boolean manual = mode == DeviceProfile.MODE_HOTSPOT_RELAY ? false : profile.manual;

                            DeviceProfile updated = new DeviceProfile(
                                    profile.id,
                                    name,
                                    ssid,
                                    sec,
                                    pass,
                                    normalizedUrl,
                                    mode,
                                    profile.lastDiscoveredUrl,
                                    true,
                                    manual);
                            dialog.dismiss();
                            callback.onSaved(updated);
                        }
                    });
                }
            }
        });
        dialog.show();
    }

    public static void details(Context context, String title, String message) {
        new AlertDialog.Builder(context)
                .setTitle(title)
                .setMessage(message)
                .setPositiveButton(R.string.dialog_ok, null)
                .show();
    }

    private static EditText addField(
            Context context, LinearLayout parent, String label, String value, int inputType, float density) {
        TextView tv = new TextView(context);
        tv.setText(label);
        tv.setTextSize(12f);
        tv.setTextColor(context.getColor(R.color.connection_muted));
        tv.setPadding(0, Math.round(10f * density), 0, Math.round(2f * density));
        parent.addView(tv);

        EditText edit = new EditText(context);
        edit.setText(value == null ? "" : value);
        edit.setInputType(inputType);
        edit.setSingleLine(true);
        edit.setTextSize(15f);
        edit.setTextColor(context.getColor(R.color.connection_text));
        parent.addView(edit);
        return edit;
    }

    private static Spinner addSpinner(
            Context context, LinearLayout parent, String label, List<String> items, int selection, float density) {
        TextView tv = new TextView(context);
        tv.setText(label);
        tv.setTextSize(12f);
        tv.setTextColor(context.getColor(R.color.connection_muted));
        tv.setPadding(0, Math.round(10f * density), 0, Math.round(2f * density));
        parent.addView(tv);

        Spinner spinner = new Spinner(context);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(context, android.R.layout.simple_spinner_item, items);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
        if (selection >= 0 && selection < items.size()) {
            spinner.setSelection(selection);
        }
        parent.addView(spinner);
        return spinner;
    }
}
