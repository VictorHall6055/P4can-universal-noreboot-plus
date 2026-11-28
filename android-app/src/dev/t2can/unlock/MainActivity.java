package dev.t2can.unlock;

import android.app.Activity;
import android.content.Intent;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.webkit.DownloadListener;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

public class MainActivity extends Activity implements ConnectionManager.Listener {
    private static final int REQ_FILE_CHOOSER = 1001;
    private static final int REQ_CREATE_DOCUMENT = 1002;
    private static final int REQ_WIFI_PERMISSION = 1003;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private ProfileRepository repository;
    private ConnectionManager connectionManager;
    private FileExporter fileExporter;

    private FrameLayout rootContainer;
    private LinearLayout connectionScreen;
    private WebView webView;

    private View statusDot;
    private TextView statusPillText;
    private TextView titleView;
    private TextView modeHintView;
    private TextView summaryView;
    private TextView footerView;
    private Button primaryButton;
    private Button secondaryButton;
    private Button editButton;
    private Button detailsButton;
    private final Button[] modeButtons = new Button[3];

    private ValueCallback<Uri[]> pendingFileCallback;
    private WebMessagePort nativePort;
    private String bridgeScript = "";
    private String activeDashboardHome = UrlPolicy.DEFAULT_HOME;
    private boolean dashboardVisible = false;
    private boolean pageLoaded = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);

        repository = new ProfileRepository(this);
        DeviceProfile initial = repository.active();
        activeDashboardHome = initial.resolvedHomeUrl();
        bridgeScript = loadAssetText("dashboard-bridge.js");

        fileExporter = new FileExporter(this, new FileExporter.Launcher() {
            @Override
            public void launchCreateDocument(Intent intent) {
                startActivityForResult(intent, REQ_CREATE_DOCUMENT);
            }
        });

        connectionManager = new ConnectionManager(this, initial, this);

        buildUi();
        setContentView(rootContainer);
        renderState(connectionManager.state());
        renderModeSelector(initial.mode);
        updateFooter(initial);

        if (savedInstanceState == null) {
            if (connectionManager.needsWifiPermissionForActive()) {
                requestPermissions(new String[]{PermissionPolicy.permission()}, REQ_WIFI_PERMISSION);
            } else {
                connectionManager.connectIfIdle("应用启动");
            }
        }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (webView != null && pageLoaded) {
            boolean dark = isNightMode();
            webView.evaluateJavascript("window.__tuNativeTheme&&window.__tuNativeTheme(" + dark + ");", null);
        }
    }

    @Override
    protected void onDestroy() {
        mainHandler.removeCallbacksAndMessages(null);
        if (connectionManager != null) {
            connectionManager.destroy();
        }
        if (fileExporter != null) {
            fileExporter.close();
        }
        if (webView != null) {
            webView.destroy();
        }
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (dashboardVisible && webView != null) {
            webView.evaluateJavascript(
                    "window.__tuNativeBack?window.__tuNativeBack():'exit';",
                    new ValueCallback<String>() {
                        @Override
                        public void onReceiveValue(String value) {
                            String trimmed = value == null ? "" : value.replace("\"", "");
                            if ("busy".equals(trimmed)) {
                                Toast.makeText(
                                        MainActivity.this,
                                        R.string.file_transfer_busy,
                                        Toast.LENGTH_SHORT).show();
                            } else if ("modal".equals(trimmed)) {
                                Toast.makeText(
                                        MainActivity.this,
                                        R.string.modal_open,
                                        Toast.LENGTH_SHORT).show();
                            } else if (!"handled".equals(trimmed)) {
                                showConnectionScreen();
                            }
                        }
                    });
            return;
        }
        super.onBackPressed();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_WIFI_PERMISSION) {
            boolean granted = PermissionPolicy.hasWifiPermission(this);
            connectionManager.onPermissionResult(granted);
            if (granted && connectionManager.phase() == ConnectionSession.PHASE_IDLE) {
                connectionManager.connectIfIdle("授权后自动连接");
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_FILE_CHOOSER) {
            ValueCallback<Uri[]> cb = pendingFileCallback;
            pendingFileCallback = null;
            if (cb == null) {
                return;
            }
            Uri[] result = WebChromeClient.FileChooserParams.parseResult(resultCode, data);
            cb.onReceiveValue(result);
        } else if (requestCode == REQ_CREATE_DOCUMENT) {
            Uri uri = (resultCode == RESULT_OK && data != null) ? data.getData() : null;
            fileExporter.onDocumentPicked(uri);
        }
    }

    @Override
    public void onStateChanged(ConnectionManager.UiState state) {
        renderState(state);
        DeviceProfile profile = connectionManager.profile();
        renderModeSelector(profile.mode);
        updateFooter(profile);
    }

    @Override
    public void onReady(String resolvedUrl) {
        openDashboard(resolvedUrl);
    }

    @Override
    public void onProfileUpdated(DeviceProfile updated) {
        repository.save(updated);
        renderModeSelector(updated.mode);
        updateFooter(updated);
    }

    @Override
    public void onMessage(String message) {
        if ("NEED_WIFI_PERMISSION".equals(message)) {
            requestPermissions(new String[]{PermissionPolicy.permission()}, REQ_WIFI_PERMISSION);
        } else if (message != null && !message.isEmpty()) {
            Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
        }
    }

    private void buildUi() {
        float d = getResources().getDisplayMetrics().density;
        rootContainer = new FrameLayout(this);
        rootContainer.setBackgroundColor(getColor(R.color.connection_background));

        connectionScreen = new LinearLayout(this);
        connectionScreen.setOrientation(LinearLayout.VERTICAL);
        connectionScreen.setFitsSystemWindows(true);
        connectionScreen.setBackgroundColor(getColor(R.color.connection_background));
        connectionScreen.setPadding(dp(20, d), dp(18, d), dp(20, d), dp(18, d));

        // Header Row
        LinearLayout topBar = new LinearLayout(this);
        topBar.setOrientation(LinearLayout.HORIZONTAL);
        topBar.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams topLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        topLp.bottomMargin = dp(24, d);
        topBar.setLayoutParams(topLp);

        TextView brandTitle = new TextView(this);
        brandTitle.setText(R.string.header_title);
        brandTitle.setTextColor(getColor(R.color.connection_text));
        brandTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17f);
        brandTitle.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        LinearLayout.LayoutParams brandLp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        brandTitle.setLayoutParams(brandLp);
        topBar.addView(brandTitle);

        LinearLayout pill = new LinearLayout(this);
        pill.setOrientation(LinearLayout.HORIZONTAL);
        pill.setGravity(Gravity.CENTER_VERTICAL);
        pill.setBackground(pillDrawable(getColor(R.color.connection_chip), dp(18, d)));
        pill.setPadding(dp(12, d), dp(7, d), dp(12, d), dp(7, d));

        statusDot = new View(this);
        LinearLayout.LayoutParams dotLp = new LinearLayout.LayoutParams(dp(8, d), dp(8, d));
        dotLp.rightMargin = dp(8, d);
        statusDot.setLayoutParams(dotLp);
        statusDot.setBackground(pillDrawable(getColor(R.color.status_red), dp(4, d)));
        pill.addView(statusDot);

        statusPillText = new TextView(this);
        statusPillText.setTextColor(getColor(R.color.connection_text));
        statusPillText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        statusPillText.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        pill.addView(statusPillText);

        topBar.addView(pill);
        connectionScreen.addView(topBar);

        // Center Scroll Card
        ScrollView centerScroll = new ScrollView(this);
        centerScroll.setFillViewport(true);
        LinearLayout.LayoutParams scrollLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        centerScroll.setLayoutParams(scrollLp);

        LinearLayout cardHolder = new LinearLayout(this);
        cardHolder.setOrientation(LinearLayout.VERTICAL);
        cardHolder.setGravity(Gravity.CENTER_VERTICAL);

        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(cardDrawable(getColor(R.color.connection_card), dp(22, d)));
        card.setPadding(dp(20, d), dp(22, d), dp(20, d), dp(20, d));

        TextView sectionLabel = new TextView(this);
        sectionLabel.setText("选择连接方式");
        sectionLabel.setTextColor(getColor(R.color.connection_muted));
        sectionLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        sectionLabel.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        card.addView(sectionLabel);

        // 3-Mode Quick Segmented Selector
        LinearLayout modeRow = new LinearLayout(this);
        modeRow.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams modeRowLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        modeRowLp.topMargin = dp(8, d);
        modeRowLp.bottomMargin = dp(16, d);
        modeRow.setLayoutParams(modeRowLp);

        String[] modeTitles = new String[]{"AP 默认\n192.168.4.1", "手机热点中继\n免连ESP热", "伪公网直连\n100.100.1.1"};
        for (int i = 0; i < 3; i++) {
            final int modeIdx = i;
            Button mb = new Button(this);
            mb.setAllCaps(false);
            mb.setText(modeTitles[i]);
            mb.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            mb.setLineSpacing(0f, 1.1f);
            mb.setPadding(dp(6, d), dp(8, d), dp(6, d), dp(8, d));
            LinearLayout.LayoutParams mLp = new LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            if (i < 2) {
                mLp.rightMargin = dp(8, d);
            }
            mb.setLayoutParams(mLp);
            mb.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    switchModeAndConnect(modeIdx);
                }
            });
            modeButtons[i] = mb;
            modeRow.addView(mb);
        }
        card.addView(modeRow);

        titleView = new TextView(this);
        titleView.setTextColor(getColor(R.color.connection_text));
        titleView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f);
        titleView.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        card.addView(titleView);

        modeHintView = new TextView(this);
        modeHintView.setTextColor(getColor(R.color.connection_muted));
        modeHintView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        modeHintView.setPadding(0, dp(4, d), 0, dp(8, d));
        card.addView(modeHintView);

        summaryView = new TextView(this);
        summaryView.setTextColor(getColor(R.color.connection_text));
        summaryView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        summaryView.setLineSpacing(dp(3, d), 1.0f);
        summaryView.setPadding(0, dp(4, d), 0, dp(18, d));
        card.addView(summaryView);

        primaryButton = makeActionButton(true, d);
        primaryButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                int p = connectionManager.phase();
                if (p == ConnectionSession.PHASE_WIFI || p == ConnectionSession.PHASE_PROBE) {
                    connectionManager.stop();
                } else if (p == ConnectionSession.PHASE_CONNECTED
                        && connectionManager.state().resolvedUrl != null
                        && !connectionManager.state().resolvedUrl.isEmpty()) {
                    openDashboard(connectionManager.state().resolvedUrl);
                } else {
                    connectionManager.connect("用户点击连接");
                }
            }
        });
        card.addView(primaryButton);

        secondaryButton = makeActionButton(false, d);
        LinearLayout.LayoutParams secLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        secLp.topMargin = dp(10, d);
        secondaryButton.setLayoutParams(secLp);
        secondaryButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                DeviceProfile current = connectionManager.profile();
                if (current.mode == DeviceProfile.MODE_HOTSPOT_RELAY) {
                    openEditDialog();
                } else {
                    startActivity(new Intent(Settings.ACTION_WIFI_SETTINGS));
                }
            }
        });
        card.addView(secondaryButton);

        LinearLayout toolRow = new LinearLayout(this);
        toolRow.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams toolLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        toolLp.topMargin = dp(10, d);
        toolRow.setLayoutParams(toolLp);

        editButton = makeActionButton(false, d);
        editButton.setText("编辑设备");
        LinearLayout.LayoutParams editLp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        editLp.rightMargin = dp(8, d);
        editButton.setLayoutParams(editLp);
        editButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openEditDialog();
            }
        });
        toolRow.addView(editButton);

        detailsButton = makeActionButton(false, d);
        detailsButton.setText("连接详情");
        LinearLayout.LayoutParams detLp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        detailsButton.setLayoutParams(detLp);
        detailsButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                ConnectionPanel.details(
                        MainActivity.this,
                        "连接详情",
                        connectionManager.state().detailsText);
            }
        });
        toolRow.addView(detailsButton);

        card.addView(toolRow);
        cardHolder.addView(card);
        centerScroll.addView(cardHolder);
        connectionScreen.addView(centerScroll);

        footerView = new TextView(this);
        footerView.setGravity(Gravity.CENTER);
        footerView.setTextColor(getColor(R.color.connection_muted));
        footerView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        footerView.setPadding(0, dp(12, d), 0, dp(6, d));
        connectionScreen.addView(footerView);

        webView = createWebView();
        webView.setVisibility(View.GONE);

        rootContainer.addView(connectionScreen, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        rootContainer.addView(webView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private WebView createWebView() {
        WebView wv = new WebView(this);
        wv.setFitsSystemWindows(true);
        wv.setBackgroundColor(getColor(R.color.connection_background));

        WebSettings s = wv.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setDatabaseEnabled(true);
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(true);
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_COMPATIBILITY_MODE);
        s.setUseWideViewPort(true);
        s.setLoadWithOverviewMode(true);
        s.setCacheMode(WebSettings.LOAD_DEFAULT);

        wv.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                String url = request != null && request.getUrl() != null ? request.getUrl().toString() : "";
                if (UrlPolicy.isDevice(url, activeDashboardHome)) {
                    return false;
                }
                if (url.startsWith("http://") || url.startsWith("https://")) {
                    try {
                        startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
                    } catch (Exception ignored) {
                    }
                    return true;
                }
                return true;
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                super.onPageFinished(view, url);
                if (!UrlPolicy.isDevice(url, activeDashboardHome)) {
                    return;
                }
                pageLoaded = true;
                if (!bridgeScript.isEmpty()) {
                    view.evaluateJavascript(bridgeScript, null);
                }
                view.evaluateJavascript("window.__tuNativeTheme&&window.__tuNativeTheme(" + isNightMode() + ");", null);
                installWebMessageChannel(view);
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                super.onReceivedError(view, request, error);
                if (request != null && request.isForMainFrame()) {
                    pageLoaded = false;
                    showConnectionScreen();
                    Toast.makeText(
                            MainActivity.this,
                            "页面加载失败，请确认连接模式及设备 IP 是否一致。",
                            Toast.LENGTH_LONG).show();
                }
            }
        });

        wv.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onShowFileChooser(
                    WebView webView,
                    ValueCallback<Uri[]> filePathCallback,
                    FileChooserParams fileChooserParams) {
                if (pendingFileCallback != null) {
                    pendingFileCallback.onReceiveValue(null);
                }
                pendingFileCallback = filePathCallback;
                try {
                    Intent intent = fileChooserParams.createIntent();
                    startActivityForResult(intent, REQ_FILE_CHOOSER);
                    return true;
                } catch (Exception e) {
                    pendingFileCallback = null;
                    Toast.makeText(MainActivity.this, R.string.file_picker_unavailable, Toast.LENGTH_SHORT).show();
                    return false;
                }
            }
        });

        wv.setDownloadListener(new DownloadListener() {
            @Override
            public void onDownloadStart(
                    String url, String userAgent, String contentDisposition, String mimetype, long contentLength) {
                Toast.makeText(MainActivity.this, R.string.use_dashboard_download, Toast.LENGTH_SHORT).show();
            }
        });

        return wv;
    }

    private void installWebMessageChannel(WebView view) {
        try {
            if (nativePort != null) {
                nativePort.close();
                nativePort = null;
            }
            Uri targetOrigin = Uri.parse(activeDashboardHome);
            Uri baseOrigin = Uri.parse(
                    targetOrigin.getScheme() + "://" + targetOrigin.getAuthority() + "/");
            final WebMessagePort[] channel = view.createWebMessageChannel();
            nativePort = channel[0];
            fileExporter.attachPort(nativePort);
            nativePort.setWebMessageCallback(new WebMessagePort.WebMessageCallback() {
                @Override
                public void onMessage(WebMessagePort port, WebMessage message) {
                    if (message != null && message.getData() != null) {
                        fileExporter.handleBridgeMessage(message.getData(), port);
                    }
                }
            });
            view.postWebMessage(
                    new WebMessage("T2CAN_NATIVE_PORT", new WebMessagePort[]{channel[1]}),
                    baseOrigin);
        } catch (Exception ignored) {
        }
    }

    private void openDashboard(String resolvedUrl) {
        String clean = UrlPolicy.normalize(resolvedUrl);
        if (clean.isEmpty()) {
            clean = UrlPolicy.DEFAULT_HOME;
        }
        activeDashboardHome = clean;
        dashboardVisible = true;
        pageLoaded = false;
        connectionScreen.setVisibility(View.GONE);
        webView.setVisibility(View.VISIBLE);
        webView.loadUrl(clean);
    }

    private void showConnectionScreen() {
        dashboardVisible = false;
        webView.stopLoading();
        webView.setVisibility(View.GONE);
        connectionScreen.setVisibility(View.VISIBLE);
    }

    private void switchModeAndConnect(int modeIdx) {
        DeviceProfile updated = connectionManager.profile().withMode(modeIdx);
        repository.save(updated);
        renderModeSelector(updated.mode);
        updateFooter(updated);
        connectionManager.updateProfile(updated, true);
    }

    private void openEditDialog() {
        ConnectionPanel.edit(this, connectionManager.profile(), new ConnectionPanel.Callback() {
            @Override
            public void onSaved(DeviceProfile updated) {
                repository.save(updated);
                renderModeSelector(updated.mode);
                updateFooter(updated);
                connectionManager.updateProfile(updated, true);
            }
        });
    }

    private void renderState(ConnectionManager.UiState s) {
        statusPillText.setText(s.pillText);
        float d = getResources().getDisplayMetrics().density;
        int dotColor;
        if (s.dotState == DeviceProfile.STATUS_DOT_ONLINE) {
            dotColor = Color.parseColor("#34C759");
        } else if (s.dotState == DeviceProfile.STATUS_DOT_CONNECTING) {
            dotColor = Color.parseColor("#F5A623");
        } else {
            dotColor = getColor(R.color.status_red);
        }
        statusDot.setBackground(pillDrawable(dotColor, dp(4, d)));

        titleView.setText(s.titleText);
        DeviceProfile p = connectionManager.profile();
        if (p.mode == DeviceProfile.MODE_HOTSPOT_RELAY) {
            modeHintView.setText("当前模式 2：手机热点中继（不切 Wi-Fi，自动发现模块）");
        } else if (p.mode == DeviceProfile.MODE_AP_PUBLIC) {
            modeHintView.setText("当前模式 3：伪公网 IP 直连（" + p.resolvedHomeUrl() + "）");
        } else {
            modeHintView.setText("当前模式 1：原始 AP 直连（" + p.resolvedHomeUrl() + "）");
        }

        summaryView.setText(s.summaryText);
        primaryButton.setText(s.primaryLabel);
        primaryButton.setEnabled(s.primaryEnabled);
        secondaryButton.setText(s.secondaryLabel);
        secondaryButton.setVisibility(s.showSecondary ? View.VISIBLE : View.GONE);
    }

    private void renderModeSelector(int selectedMode) {
        float d = getResources().getDisplayMetrics().density;
        for (int i = 0; i < modeButtons.length; i++) {
            Button b = modeButtons[i];
            boolean selected = (i == selectedMode);
            int bg = selected ? getColor(R.color.connection_primary) : getColor(R.color.connection_secondary);
            int fg = selected ? getColor(R.color.connection_primary_text) : getColor(R.color.connection_secondary_text);
            int stroke = selected ? bg : getColor(R.color.connection_secondary_stroke);
            GradientDrawable gd = new GradientDrawable();
            gd.setColor(bg);
            gd.setCornerRadius(dp(12, d));
            gd.setStroke(dp(1, d), stroke);
            b.setBackground(gd);
            b.setTextColor(fg);
            b.setTypeface(Typeface.create(Typeface.DEFAULT, selected ? Typeface.BOLD : Typeface.NORMAL));
        }
    }

    private void updateFooter(DeviceProfile p) {
        String target = p.mode == DeviceProfile.MODE_HOTSPOT_RELAY
                ? (p.resolvedHomeUrl().isEmpty() ? "热点中继自动发现" : p.resolvedHomeUrl())
                : p.resolvedHomeUrl();
        footerView.setText(target + " · App 1.3.0 (v3.35.3.3) · 一缕微光");
        footerView.setEllipsize(TextUtils.TruncateAt.END);
        footerView.setSingleLine(true);
    }

    private Button makeActionButton(boolean primary, float d) {
        Button b = new Button(this);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
        b.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        b.setPadding(dp(16, d), dp(13, d), dp(16, d), dp(13, d));
        GradientDrawable gd = new GradientDrawable();
        gd.setCornerRadius(dp(14, d));
        if (primary) {
            gd.setColor(getColor(R.color.connection_primary));
            b.setTextColor(getColor(R.color.connection_primary_text));
        } else {
            gd.setColor(getColor(R.color.connection_secondary));
            gd.setStroke(dp(1, d), getColor(R.color.connection_secondary_stroke));
            b.setTextColor(getColor(R.color.connection_secondary_text));
        }
        b.setBackground(gd);
        return b;
    }

    private boolean isNightMode() {
        int mask = getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK;
        return mask == Configuration.UI_MODE_NIGHT_YES;
    }

    private String loadAssetText(String assetName) {
        StringBuilder sb = new StringBuilder();
        try (BufferedReader br = new BufferedReader(
                new InputStreamReader(getAssets().open(assetName), StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                sb.append(line).append('\n');
            }
        } catch (Exception ignored) {
        }
        return sb.toString();
    }

    private static GradientDrawable cardDrawable(int color, int radiusPx) {
        GradientDrawable gd = new GradientDrawable();
        gd.setColor(color);
        gd.setCornerRadius(radiusPx);
        return gd;
    }

    private static GradientDrawable pillDrawable(int color, int radiusPx) {
        GradientDrawable gd = new GradientDrawable();
        gd.setColor(color);
        gd.setCornerRadius(radiusPx);
        return gd;
    }

    private static int dp(int value, float density) {
        return Math.round(value * density);
    }
}
