package com.rv1106.usbcam;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.media.MediaScannerConnection;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * TGO-ABC 두피 스캐너 앱 (연동 시나리오 20260812).
 *
 *   최초 등록     BLE 검색 -> 연결 -> Device Info -> Wi-Fi Provisioning(SSID/PW) -> 장치 IP/Port 수신 -> 저장
 *   자동 재연결   저장된 장치를 마지막 주소 / mDNS(_tgoabc._tcp) 로 찾아 바로 미리보기
 *   미리보기·촬영 WebView 의 WebRTC (H.264 + DataChannel) - assets/viewer.html (board/scannerd/web/viewer.html 사본)
 *                 촬영 완료(capture.state completed) 를 받으면 HTTP 로 JPEG 2장을 받아 사진/TGOABC 에 저장
 */
public class MainActivity extends Activity {

    private static final int C_BG = 0xFF0D1117, C_CARD = 0xFF161B22, C_TEXT = 0xFFE8ECF1,
            C_MUTED = 0xFF9AA6B2, C_ACCENT = 0xFF4FB3A9, C_BAD = 0xFFF85149;
    private static final int REQ_BLE = 1, REQ_STORAGE = 2;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ExecutorService bg = Executors.newSingleThreadExecutor();

    private FrameLayout root;
    private DeviceStore store;
    private DeviceFinder finder;
    private BleProvisioner ble;

    private WebView web;
    private JSONObject current;       // 미리보기 중인 장치
    private int viewerFails;
    private boolean autoTried;
    private String pendingCapture;    // 저장 권한을 기다리는 촬영 결과

    // 등록 화면 상태
    private final Map<String, BluetoothDevice> found = new LinkedHashMap<>();
    private final Map<String, String> foundNames = new LinkedHashMap<>();
    private LinearLayout foundList;
    private TextView regStatus;
    private JSONObject regInfo;
    private String regSsid;

    // ------------------------------------------------------------ lifecycle

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setStatusBarColor(C_BG);
        store = new DeviceStore(this);
        finder = new DeviceFinder(this);
        ble = new BleProvisioner(this, bleListener);
        root = new FrameLayout(this);
        root.setBackgroundColor(C_BG);
        setContentView(root);
        watchNetwork();
        showHome(true);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (web != null) web.onResume();
    }

    @Override
    protected void onPause() {
        if (web != null) web.onPause();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        finder.cancel();
        ble.stopScan();
        ble.close();
        closeViewer();
        bg.shutdownNow();
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (web != null || regStatus != null) {
            closeViewer();
            ble.stopScan();
            ble.close();
            showHome(false);
        } else {
            super.onBackPressed();
        }
    }

    // ------------------------------------------------------------ UI 도우미

    private int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    private void toast(String s) { Toast.makeText(this, s, Toast.LENGTH_SHORT).show(); }

    private TextView text(String s, int sizeSp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sizeSp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private Button button(String s, boolean primary, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(primary ? 0xFF04110F : C_TEXT);
        GradientDrawable g = new GradientDrawable();
        g.setCornerRadius(dp(12));
        g.setColor(primary ? C_ACCENT : 0x22FFFFFF);
        b.setBackground(g);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, dp(52));
        lp.topMargin = dp(10);
        b.setLayoutParams(lp);
        return b;
    }

    private LinearLayout card() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable g = new GradientDrawable();
        g.setCornerRadius(dp(14));
        g.setColor(C_CARD);
        c.setBackground(g);
        c.setPadding(dp(16), dp(14), dp(16), dp(14));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(10);
        c.setLayoutParams(lp);
        return c;
    }

    /** 가운데 정렬된 세로 화면 틀 (최대 폭 560dp) */
    private LinearLayout page(String title, String subtitle) {
        root.removeAllViews();
        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        FrameLayout wrap = new FrameLayout(this);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(20), dp(28), dp(20), dp(28));
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                Math.min(dp(560), getResources().getDisplayMetrics().widthPixels), -2, Gravity.CENTER_HORIZONTAL);
        wrap.addView(col, lp);
        sv.addView(wrap);
        root.addView(sv);
        col.addView(text(title, 24, C_TEXT, true));
        if (subtitle != null) {
            TextView s = text(subtitle, 14, C_MUTED, false);
            s.setPadding(0, dp(6), 0, dp(10));
            col.addView(s);
        }
        return col;
    }

    // ------------------------------------------------------------ 홈

    private TextView homeStatus;

    private void showHome(boolean autoConnect) {
        regStatus = null;
        LinearLayout col = page("TGO-ABC 스캐너", "두피 스캐너를 BLE 로 등록하고, 같은 Wi-Fi 에서 미리보기·촬영합니다");
        List<JSONObject> devs = store.list();

        homeStatus = text("", 14, C_MUTED, false);
        homeStatus.setPadding(0, dp(4), 0, dp(4));

        if (devs.isEmpty()) {
            LinearLayout c = card();
            c.addView(text("등록된 장치가 없습니다", 16, C_TEXT, true));
            TextView d = text("장치 전원을 켜고 [새 장치 등록]을 누르세요. 블루투스로 장치를 찾아 Wi-Fi 정보를 넣어 줍니다.", 14, C_MUTED, false);
            d.setPadding(0, dp(6), 0, 0);
            c.addView(d);
            col.addView(c);
        } else {
            TextView h = text("등록된 장치", 14, C_MUTED, true);
            h.setPadding(0, dp(14), 0, 0);
            col.addView(h);
            for (final JSONObject d : devs) {
                LinearLayout c = card();
                c.addView(text(d.optString("name"), 17, C_TEXT, true));
                TextView s = text(d.optString("model") + " · " + d.optString("id")
                        + "\n마지막 주소 " + d.optString("ip") + ":" + d.optInt("port", 8090)
                        + (d.optString("ssid").isEmpty() ? "" : " (" + d.optString("ssid") + ")"), 13, C_MUTED, false);
                s.setPadding(0, dp(4), 0, 0);
                c.addView(s);
                c.setOnClickListener(v -> reconnect(d));
                c.setOnLongClickListener(v -> {
                    new AlertDialog.Builder(this).setTitle(d.optString("name"))
                            .setItems(new String[]{"Wi-Fi 다시 설정 (BLE)", "주소 직접 입력", "목록에서 삭제"}, (dl, w) -> {
                                if (w == 0) showRegister();
                                else if (w == 1) askAddress(d);
                                else { store.remove(d.optString("id")); showHome(false); }
                            }).show();
                    return true;
                });
                col.addView(c);
            }
        }
        col.addView(homeStatus);
        col.addView(button("새 장치 등록 (BLE)", devs.isEmpty(), v -> showRegister()));

        if (autoConnect && !devs.isEmpty() && !autoTried) {
            autoTried = true;
            reconnect(devs.get(0));
        }
    }

    /** 자동 재연결: 저장된 Wi-Fi 에 붙은 장치를 마지막 주소 / mDNS 로 찾는다 */
    private void reconnect(final JSONObject d) {
        if (homeStatus != null) {
            homeStatus.setTextColor(C_MUTED);
            homeStatus.setText(d.optString("name") + " 찾는 중… (mDNS)");
        }
        finder.find(d.optString("id"), d.optString("ip"), d.optInt("port", 8090), 12000, new DeviceFinder.Callback() {
            @Override public void onFound(String ip, int port, JSONObject info) {
                JSONObject n = merge(d, info, ip, port);
                store.save(n);
                openViewer(n);
            }
            @Override public void onFail(String why) {
                if (homeStatus == null) return;
                homeStatus.setTextColor(C_BAD);
                homeStatus.setText(d.optString("name") + ": " + why
                        + "\n장치 전원과 Wi-Fi 를 확인하세요. 길게 누르면 Wi-Fi 를 다시 설정할 수 있습니다.");
            }
        });
    }

    private static JSONObject merge(JSONObject base, JSONObject info, String ip, int port) {
        JSONObject n = new JSONObject();
        try {
            if (base != null) for (java.util.Iterator<String> it = base.keys(); it.hasNext(); ) {
                String k = it.next();
                n.put(k, base.get(k));
            }
            for (String k : new String[]{"id", "name", "model", "fw"}) if (info.has(k)) n.put(k, info.get(k));
            n.put("ip", ip);
            n.put("port", port);
        } catch (Exception ignored) { }
        return n;
    }

    private void askAddress(final JSONObject d) {
        final EditText e = new EditText(this);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        e.setText(d.optString("ip"));
        new AlertDialog.Builder(this).setTitle("장치 주소").setView(e)
                .setPositiveButton("연결", (dl, w) -> {
                    final String ip = e.getText().toString().trim().replaceAll("^https?://", "").replaceAll("[:/].*$", "");
                    final int port = d.optInt("port", 8090);
                    bg.execute(() -> {
                        JSONObject info = DeviceFinder.probe(ip, port, 2000);
                        ui.post(() -> {
                            if (info == null) toast(ip + ":" + port + " 응답 없음");
                            else { JSONObject n = merge(d, info, ip, port); store.save(n); openViewer(n); }
                        });
                    });
                })
                .setNegativeButton("취소", null).show();
    }

    // ------------------------------------------------------------ 최초 등록 (BLE)

    private void showRegister() {
        finder.cancel();
        if (!ble.available()) { toast("이 기기는 블루투스를 지원하지 않습니다"); return; }
        if (!hasBlePermissions()) { requestBlePermissions(); return; }
        if (!ble.enabled()) {
            startActivity(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE));
            return;
        }
        regInfo = null;
        found.clear();
        foundNames.clear();
        LinearLayout col = page("새 장치 등록", "1. 장치 전원을 켜면 블루투스로 광고합니다. 목록에서 장치를 고르세요.");
        regStatus = text("BLE 검색 중…", 14, C_MUTED, false);
        col.addView(regStatus);
        foundList = new LinearLayout(this);
        foundList.setOrientation(LinearLayout.VERTICAL);
        col.addView(foundList);
        col.addView(button("다시 검색", false, v -> showRegister()));
        col.addView(button("취소", false, v -> { ble.stopScan(); ble.close(); showHome(false); }));
        ble.startScan();
        ui.postDelayed(() -> {
            if (regStatus != null && found.isEmpty() && regInfo == null)
                regStatus.setText("장치를 찾지 못했습니다. 장치 전원, 위치(GPS) 설정이 켜져 있는지 확인하세요.");
        }, 10000);
    }

    private void renderFound() {
        if (foundList == null) return;
        foundList.removeAllViews();
        for (Map.Entry<String, BluetoothDevice> e : found.entrySet()) {
            final BluetoothDevice d = e.getValue();
            LinearLayout c = card();
            c.addView(text(foundNames.get(e.getKey()), 17, C_TEXT, true));
            c.addView(text(d.getAddress(), 13, C_MUTED, false));
            c.setOnClickListener(v -> {
                regStatus.setText(foundNames.get(e.getKey()) + " 연결 중…");
                ble.connect(d);
            });
            foundList.addView(c);
        }
        if (!found.isEmpty()) regStatus.setText("장치 " + found.size() + "대 발견 - 눌러서 연결");
    }

    /** 2. Device Info 를 받은 뒤 Wi-Fi 입력 화면 */
    private void showWifiForm(JSONObject info) {
        regInfo = info;
        LinearLayout col = page("Wi-Fi 설정", "2. 장치가 접속할 공유기 정보를 넣으세요. 이 폰과 같은 Wi-Fi 여야 자동 재연결이 됩니다.");
        LinearLayout c = card();
        c.addView(text(info.optString("name"), 17, C_TEXT, true));
        c.addView(text("모델 " + info.optString("model") + " · 펌웨어 " + info.optString("fw")
                + "\n식별자 " + info.optString("id"), 13, C_MUTED, false));
        col.addView(c);

        final EditText ssid = new EditText(this);
        ssid.setHint("Wi-Fi 이름 (SSID)");
        ssid.setSingleLine(true);
        ssid.setTextColor(C_TEXT);
        ssid.setHintTextColor(C_MUTED);
        String cur = currentSsid();
        if (cur != null) ssid.setText(cur);
        col.addView(ssid);

        final EditText pw = new EditText(this);
        pw.setHint("비밀번호 (개방형이면 비움)");
        pw.setSingleLine(true);
        pw.setTextColor(C_TEXT);
        pw.setHintTextColor(C_MUTED);
        pw.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        col.addView(pw);

        regStatus = text("", 14, C_MUTED, false);
        regStatus.setPadding(0, dp(8), 0, 0);
        col.addView(regStatus);

        final Button go = button("Wi-Fi 연결", true, null);
        go.setOnClickListener(v -> {
            String s = ssid.getText().toString().trim();
            String p = pw.getText().toString();
            if (s.isEmpty()) { toast("SSID 를 입력하세요"); return; }
            if (!p.isEmpty() && (p.length() < 8 || p.length() > 63)) { toast("비밀번호는 8~63자입니다"); return; }
            regSsid = s;
            if (!ble.provision(s, p)) { regStatus.setText("BLE 연결이 끊겼습니다. 다시 등록하세요."); return; }
            go.setEnabled(false);
            regStatus.setTextColor(C_MUTED);
            regStatus.setText("Wi-Fi 정보 전송…");
            ui.postDelayed(() -> go.setEnabled(true), 3000);
        });
        col.addView(go);
        col.addView(button("취소", false, v -> { ble.close(); showHome(false); }));
    }

    private final BleProvisioner.Listener bleListener = new BleProvisioner.Listener() {
        @Override public void onFound(BluetoothDevice d, String name, int rssi) {
            if (regInfo != null || regStatus == null) return;
            if (!found.containsKey(d.getAddress())) {
                found.put(d.getAddress(), d);
                foundNames.put(d.getAddress(), name + "  (" + rssi + " dBm)");
                renderFound();
            }
        }

        @Override public void onReady(JSONObject info) {
            showWifiForm(info);
        }

        @Override public void onStatus(JSONObject st) {
            if (regStatus == null || regInfo == null) return;
            String state = st.optString("state");
            switch (state) {
                case "connecting":
                    regStatus.setTextColor(C_MUTED);
                    regStatus.setText("장치가 '" + st.optString("ssid") + "' 에 접속하는 중… (최대 1분)");
                    break;
                case "connected":
                    // 이번에 보낸 요청의 결과만 받는다 (구독 직후 오는 예전 상태는 무시)
                    if (regSsid == null || !regSsid.equals(st.optString("ssid"))) break;
                    regStatus.setTextColor(C_ACCENT);
                    regStatus.setText("연결됨: " + st.optString("ip") + ":" + st.optInt("port"));
                    JSONObject d = merge(regInfo, regInfo, st.optString("ip"), st.optInt("port", 8090));
                    try { d.put("ssid", st.optString("ssid")); } catch (Exception ignored) { }
                    store.save(d);
                    regSsid = null;
                    ble.close();
                    ui.postDelayed(() -> openViewer(d), 800);
                    break;
                case "failed":
                    regStatus.setTextColor(C_BAD);
                    regStatus.setText("실패: " + st.optString("error", "Wi-Fi 접속 실패") + "\n정보를 고쳐 다시 시도하세요.");
                    break;
            }
        }

        @Override public void onError(String msg) {
            if (regStatus != null) { regStatus.setTextColor(C_BAD); regStatus.setText(msg); }
            else toast(msg);
        }

        @Override public void onDisconnected() {
            if (regStatus != null && regInfo != null && regSsid != null) {
                regStatus.setTextColor(C_BAD);
                regStatus.setText("BLE 연결이 끊겼습니다. 장치가 Wi-Fi 에 붙었는지 홈에서 확인하세요.");
            }
        }
    };

    private String currentSsid() {
        try {
            WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            WifiInfo wi = wm != null ? wm.getConnectionInfo() : null;
            String s = wi != null ? wi.getSSID() : null;
            if (s == null || s.contains("unknown ssid")) return null;
            return s.replaceAll("^\"|\"$", "");
        } catch (Exception e) {
            return null;
        }
    }

    // ------------------------------------------------------------ 권한

    private boolean hasBlePermissions() {
        if (Build.VERSION.SDK_INT < 23) return true;
        return checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestBlePermissions() {
        if (Build.VERSION.SDK_INT >= 23)
            requestPermissions(new String[]{Manifest.permission.ACCESS_FINE_LOCATION}, REQ_BLE);
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        boolean ok = res.length > 0 && res[0] == PackageManager.PERMISSION_GRANTED;
        if (code == REQ_BLE) {
            if (ok) showRegister();
            else toast("BLE 검색에는 위치 권한이 필요합니다");
        } else if (code == REQ_STORAGE && pendingCapture != null) {
            String js = pendingCapture;
            pendingCapture = null;
            if (ok) saveCapture(js);
            else toast("저장 권한이 없어 사진을 저장하지 못했습니다");
        }
    }

    // ------------------------------------------------------------ 미리보기 (WebRTC)

    private void openViewer(JSONObject d) {
        finder.cancel();
        closeViewer();
        regStatus = null;
        current = d;
        viewerFails = 0;
        root.removeAllViews();

        WebView.setWebContentsDebuggingEnabled(true); // PC chrome://inspect 로 볼 수 있게
        web = new WebView(this);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setCacheMode(WebSettings.LOAD_NO_CACHE);
        web.setBackgroundColor(Color.BLACK);
        web.setWebViewClient(new WebViewClient());
        web.addJavascriptInterface(new Bridge(), "Android");
        root.addView(web, new FrameLayout.LayoutParams(-1, -1));

        // 앱에 들어 있는 뷰어를 장치 주소를 기준(origin)으로 띄운다 -> fetch('/api/..') 가 장치로 간다
        String base = "http://" + d.optString("ip") + ":" + d.optInt("port", 8090) + "/";
        web.loadDataWithBaseURL(base, readAsset("viewer.html"), "text/html", "utf-8", null);
    }

    private void closeViewer() {
        if (web != null) {
            root.removeView(web);
            web.loadUrl("about:blank");
            web.destroy();
            web = null;
        }
    }

    private String readAsset(String name) {
        try (InputStream in = getAssets().open(name)) {
            return DeviceFinder.readAll(in);
        } catch (Exception e) {
            return "<p style='color:#fff'>viewer.html 없음: " + e + "</p>";
        }
    }

    /** viewer.html 이 부르는 다리 (window.Android) */
    final class Bridge {
        @JavascriptInterface
        public void onCapture(String json) {
            ui.post(() -> saveCapture(json));
        }

        @JavascriptInterface
        public void toast(String t) {
            ui.post(() -> MainActivity.this.toast(t));
        }

        @JavascriptInterface
        public void onState(String s) {
            ui.post(() -> {
                if (s.startsWith("ok:")) { viewerFails = 0; return; }
                if (!s.startsWith("bad:") || current == null) return;
                // 연결이 계속 안 되면 장치 주소가 바뀌었을 수 있다 (DHCP) -> mDNS 로 다시 찾는다
                if (++viewerFails == 3) {
                    final JSONObject d = current;
                    finder.find(d.optString("id"), null, 0, 15000, new DeviceFinder.Callback() {
                        @Override public void onFound(String ip, int port, JSONObject info) {
                            if (current != d) return;
                            if (ip.equals(d.optString("ip")) && port == d.optInt("port")) return;
                            JSONObject n = merge(d, info, ip, port);
                            store.save(n);
                            MainActivity.this.toast("장치 주소가 바뀌었습니다: " + ip);
                            openViewer(n);
                        }
                        @Override public void onFail(String why) { }
                    });
                }
            });
        }
    }

    // ------------------------------------------------------------ 촬영 결과 저장

    /** 온도/습도/JPEG(일반광/UV광) - HTTP 로 받아 사진/TGOABC 에 저장한다 */
    private void saveCapture(final String json) {
        if (Build.VERSION.SDK_INT >= 23 && Build.VERSION.SDK_INT <= 28
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
            pendingCapture = json;
            requestPermissions(new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQ_STORAGE);
            return;
        }
        final JSONObject d = current;
        if (d == null) return;
        final String base = "http://" + d.optString("ip") + ":" + d.optInt("port", 8090);
        bg.execute(() -> {
            try {
                JSONObject r = new JSONObject(json);
                String id = r.getString("capture_id");
                File dir = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES), "TGOABC");
                if (!dir.isDirectory() && !dir.mkdirs()) throw new Exception("폴더를 만들 수 없습니다");
                JSONArray ims = r.getJSONArray("images");
                int n = 0;
                for (int i = 0; i < ims.length(); i++) {
                    JSONObject im = ims.getJSONObject(i);
                    File f = new File(dir, id + "_" + im.getString("kind") + ".jpg");
                    download(base + im.getString("url"), f);
                    MediaScannerConnection.scanFile(this, new String[]{f.getAbsolutePath()}, new String[]{"image/jpeg"}, null);
                    n++;
                }
                try (FileOutputStream out = new FileOutputStream(new File(dir, id + ".json"))) {
                    out.write(r.toString(2).getBytes(StandardCharsets.UTF_8));
                }
                JSONObject c = r.optJSONObject("climate");
                final String msg = "사진/TGOABC 에 " + n + "장 저장"
                        + (c != null ? String.format(java.util.Locale.US, " · %.1f℃ %.1f%%",
                                c.optDouble("temperature"), c.optDouble("humidity")) : "");
                ui.post(() -> toast(msg));
            } catch (Exception e) {
                ui.post(() -> toast("저장 실패: " + e.getMessage()));
            }
        });
    }

    private static void download(String url, File f) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        try {
            c.setConnectTimeout(3000);
            c.setReadTimeout(15000);
            if (c.getResponseCode() != 200) throw new Exception("HTTP " + c.getResponseCode());
            try (InputStream in = c.getInputStream(); FileOutputStream out = new FileOutputStream(f)) {
                byte[] buf = new byte[64 * 1024];
                for (int r; (r = in.read(buf)) > 0; ) out.write(buf, 0, r);
            }
        } finally {
            c.disconnect();
        }
    }

    // ------------------------------------------------------------ 네트워크

    /**
     * 공유기에 인터넷이 없으면 Android 가 앱 트래픽을 모바일 데이터로 보낼 수 있다.
     * Wi-Fi 가 잡히면 앱 프로세스를 Wi-Fi 에 묶는다 (장치는 같은 LAN 에 있다).
     */
    private void watchNetwork() {
        if (Build.VERSION.SDK_INT < 23) return;
        final ConnectivityManager cm = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
        if (cm == null) return;
        NetworkRequest req = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build();
        cm.registerNetworkCallback(req, new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network n) { cm.bindProcessToNetwork(n); }
            @Override public void onLost(Network n) { cm.bindProcessToNetwork(null); }
        });
    }
}
