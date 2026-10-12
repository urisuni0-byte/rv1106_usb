package com.rv1106.usbcam;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.media.MediaScannerConnection;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.Uri;
import android.net.wifi.WifiConfiguration;
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
import android.util.Base64;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.URL;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorCompletionService;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

/**
 * RV1106 USB 카메라 뷰어 (Luckfox Pico RV1106 + Arducam B0290 IMX298 UVC).
 *
 * 보드 IP 를 고정하지 않는다. 시작할 때와 연결이 끊길 때마다 다음 순서로 보드를 찾는다.
 *   1) 보드 자체 AP 주소 (RvCam-xxxx Wi-Fi, 192.168.4.1 고정)
 *   2) 마지막으로 찾았던 주소
 *   3) 지금 붙어 있는 Wi-Fi 서브넷 전체 (/status.json 응답으로 판별)
 * 어디에서도 못 찾으면 보드 AP(RvCam-xxxx) 에 자동으로 접속한다.
 */
public class MainActivity extends Activity {

    static final int PORT = 8090;   // 8080 은 RvCam 프로비저닝 API
    static final long HEALTH_MS = 4000;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ExecutorService bg = Executors.newSingleThreadExecutor();

    private WebView web;
    private LinearLayout panel;
    private TextView title, detail;
    private Button bApWifi;
    private SharedPreferences prefs;

    private volatile String boardIp;
    private volatile boolean searching;
    private int healthFails;
    private boolean triedAp;
    private String pendingDownload;

    // ------------------------------------------------------------ lifecycle

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("rv1106usb", MODE_PRIVATE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        buildUi();
        watchNetwork();
        discover("보드를 찾는 중…");
    }

    @Override
    protected void onResume() {
        super.onResume();
        immersive();
        ui.removeCallbacks(health);
        ui.postDelayed(health, HEALTH_MS);
        if (web != null) web.onResume();
    }

    @Override
    protected void onPause() {
        ui.removeCallbacks(health);
        if (web != null) web.onPause();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        bg.shutdownNow();
        if (web != null) web.destroy();
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) immersive();
    }

    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    // ------------------------------------------------------------ UI

    private void buildUi() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);

        // USB(ADB) 로 연결한 PC 의 chrome://inspect 에서만 볼 수 있다
        WebView.setWebContentsDebuggingEnabled(true);
        web = new WebView(this);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        // WebRTC 영상(<video muted autoplay>)이 바로 재생되도록. 소리는 뷰어에서 버튼으로만 켠다.
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setCacheMode(WebSettings.LOAD_NO_CACHE);
        web.setBackgroundColor(Color.BLACK);
        web.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView v, String url) {
                // 앱이 이미 전체화면이라 뷰어의 [전체화면] 버튼은 숨긴다.
                v.evaluateJavascript("(function(){var b=document.getElementById('b-full');"
                        + "if(b)b.style.display='none';})()", null);
            }

            @Override
            public void onReceivedError(WebView v, WebResourceRequest r, WebResourceError e) {
                if (Build.VERSION.SDK_INT >= 21 && r.isForMainFrame()) lost("페이지를 열 수 없습니다");
            }

            @Override
            @SuppressWarnings("deprecation")
            public void onReceivedError(WebView v, int code, String desc, String url) {
                if (Build.VERSION.SDK_INT < 23) lost("페이지를 열 수 없습니다");
            }
        });
        web.setDownloadListener((url, ua, cd, mime, len) -> saveSnapshot(url));
        // WebRTC 데이터 채널로 받은 사진은 뷰어가 이 다리로 넘긴다 (blob: URL 은 앱이 못 받으므로)
        web.addJavascriptInterface(new Object() {
            @JavascriptInterface
            public String savePhoto(String name, String b64) {
                try {
                    byte[] jpg = Base64.decode(b64, Base64.DEFAULT);
                    String safe = name.replaceAll("[^A-Za-z0-9_.-]", "_");
                    if (!safe.endsWith(".jpg")) safe += ".jpg";
                    writePhoto(safe, jpg);
                    return "";
                } catch (Exception e) {
                    return e.getMessage() != null ? e.getMessage() : e.toString();
                }
            }
        }, "UsbCamApp");
        root.addView(web, new FrameLayout.LayoutParams(-1, -1));

        panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setGravity(Gravity.CENTER);
        panel.setBackgroundColor(0xF00B0D10);
        int p = dp(24);
        panel.setPadding(p, p, p, p);

        title = new TextView(this);
        title.setTextColor(0xFFE8ECF1);
        title.setTextSize(20);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        panel.addView(title);

        detail = new TextView(this);
        detail.setTextColor(0xFF8B95A3);
        detail.setTextSize(14);
        detail.setGravity(Gravity.CENTER);
        detail.setPadding(0, dp(10), 0, dp(22));
        panel.addView(detail);

        Button bRetry = button("다시 찾기", v -> discover("보드를 찾는 중…"));
        bApWifi = button("보드 Wi-Fi (RvCam) 에 연결", v -> connectAp(true));
        Button bManual = button("주소 직접 입력", v -> askAddress());
        panel.addView(bRetry);
        panel.addView(bApWifi);
        panel.addView(bManual);

        root.addView(panel, new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);
    }

    private Button button(String text, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(280), ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        b.setLayoutParams(lp);
        return b;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private void showPanel(String t, String d) {
        title.setText(t);
        detail.setText(d);
        panel.setVisibility(View.VISIBLE);
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
    }

    // ------------------------------------------------------------ discovery

    private void discover(String why) {
        if (searching) return;
        searching = true;
        showPanel(why, "보드 Wi-Fi(RvCam) 또는 같은 공유기에서 카메라 보드를 찾고 있습니다");
        bg.execute(() -> {
            String ip = findBoard();
            ui.post(() -> {
                searching = false;
                if (ip != null) {
                    open(ip);
                } else if (!triedAp && connectAp(false)) {
                    triedAp = true;
                    showPanel("보드 Wi-Fi 에 연결하는 중…",
                            "연결되면 자동으로 영상을 엽니다");
                    ui.postDelayed(() -> discover("보드를 찾는 중…"), 6000);
                } else {
                    showPanel("카메라 보드를 찾지 못했습니다",
                            "보드 전원이 켜져 있는지, 패드가 보드 Wi-Fi(RvCam) 나\n"
                            + "보드와 같은 공유기에 연결돼 있는지 확인하세요.\n"
                            + "5초 후 다시 찾습니다.");
                    ui.postDelayed(() -> { if (boardIp == null) discover("보드를 찾는 중…"); }, 5000);
                }
            });
        });
    }

    private void open(String ip) {
        boardIp = ip;
        healthFails = 0;
        prefs.edit().putString("last_ip", ip).apply();
        panel.setVisibility(View.GONE);
        web.loadUrl("http://" + ip + ":" + PORT + "/");
        toast("카메라 연결: " + ip + (Config.AP_IP.equals(ip) ? " (보드 Wi-Fi)" : ""));
    }

    private void lost(String why) {
        if (boardIp == null && searching) return;
        boardIp = null;
        web.stopLoading();
        web.loadUrl("about:blank");
        discover(why + " – 다시 찾는 중…");
    }

    /** 연결된 보드가 여전히 응답하는지 주기적으로 확인 (IP 가 바뀌면 다시 찾는다). */
    private final Runnable health = new Runnable() {
        @Override
        public void run() {
            final String ip = boardIp;
            if (ip != null && !searching) {
                bg.execute(() -> {
                    boolean ok = probe(ip, 1500);
                    ui.post(() -> {
                        if (!ip.equals(boardIp)) return;
                        healthFails = ok ? 0 : healthFails + 1;
                        if (healthFails >= 2) lost("연결이 끊겼습니다");
                    });
                });
            }
            ui.postDelayed(this, HEALTH_MS);
        }
    };

    private String findBoard() {
        LinkedHashSet<String> first = new LinkedHashSet<>();
        first.add(Config.AP_IP);
        String last = prefs.getString("last_ip", null);
        if (last != null) first.add(last);
        for (String ip : first) if (probe(ip, 800)) return ip;

        List<String> hosts = subnetHosts();
        hosts.removeAll(first);
        if (hosts.isEmpty()) return null;
        ExecutorService pool = Executors.newFixedThreadPool(48);
        ExecutorCompletionService<String> cs = new ExecutorCompletionService<>(pool);
        List<Future<String>> fs = new ArrayList<>();
        for (final String h : hosts) fs.add(cs.submit(() -> probe(h, 700) ? h : null));
        try {
            for (int i = 0; i < fs.size(); i++) {
                Future<String> f = cs.poll(6, TimeUnit.SECONDS);
                if (f == null) break;
                String r = f.get();
                if (r != null) return r;
            }
        } catch (Exception ignored) {
        } finally {
            pool.shutdownNow();
        }
        return null;
    }

    /** 패드가 붙어 있는 IPv4 /24 서브넷의 모든 호스트 (자기 자신 제외). */
    private List<String> subnetHosts() {
        List<String> out = new ArrayList<>();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress a = ia.getAddress();
                    if (!(a instanceof Inet4Address) || !a.isSiteLocalAddress()) continue;
                    byte[] q = a.getAddress();
                    String base = (q[0] & 255) + "." + (q[1] & 255) + "." + (q[2] & 255) + ".";
                    for (int i = 1; i < 255; i++) {
                        if (i == (q[3] & 255)) continue;
                        String h = base + i;
                        if (!out.contains(h)) out.add(h);
                    }
                }
            }
        } catch (Exception ignored) {
        }
        return out;
    }

    /** http://ip:8090/status.json 이 카메라 서버의 응답인지 확인. */
    static boolean probe(String ip, int timeoutMs) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL("http://" + ip + ":" + PORT + "/status.json").openConnection();
            c.setConnectTimeout(timeoutMs);
            c.setReadTimeout(timeoutMs);
            c.setUseCaches(false);
            if (c.getResponseCode() != 200) return false;
            byte[] buf = new byte[1024];
            InputStream in = c.getInputStream();
            int n = in.read(buf);
            String s = n > 0 ? new String(buf, 0, n, "UTF-8") : "";
            return s.contains("\"card\"") && s.contains("\"width\"");
        } catch (Exception e) {
            return false;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    // ------------------------------------------------------------ 보드 AP (RvCam-xxxx)

    /** Android 9 이하: 보드 AP 를 등록하고 접속한다. 이미 저장된 RvCam-xxxx 가 있으면 그걸 쓴다. */
    @SuppressWarnings("deprecation")
    private boolean connectAp(boolean userAsked) {
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wm == null) return false;
        if (Build.VERSION.SDK_INT >= 29) {
            if (userAsked) {
                toast("Wi-Fi 설정에서 '" + Config.AP_SSID + "' 에 연결하세요");
                startActivity(new android.content.Intent(android.provider.Settings.ACTION_WIFI_SETTINGS));
            }
            return false;
        }
        try {
            if (!wm.isWifiEnabled()) wm.setWifiEnabled(true);
            String quoted = "\"" + Config.AP_SSID + "\"";
            int id = -1;
            List<WifiConfiguration> known = wm.getConfiguredNetworks();
            if (known != null) {
                for (WifiConfiguration w : known)
                    if (quoted.equals(w.SSID)) { id = w.networkId; break; }
                // SSID 끝자리는 보드 MAC 마다 다르다 (RvCam-0b9b 등) - 저장된 RvCam-* 를 우선 사용
                if (id < 0)
                    for (WifiConfiguration w : known)
                        if (w.SSID != null && w.SSID.startsWith("\"" + Config.AP_SSID_PREFIX)) {
                            id = w.networkId;
                            break;
                        }
            }
            if (id < 0) {
                WifiConfiguration w = new WifiConfiguration();
                w.SSID = quoted;
                w.preSharedKey = "\"" + Config.AP_PASSWORD + "\"";
                w.allowedKeyManagement.set(WifiConfiguration.KeyMgmt.WPA_PSK);
                id = wm.addNetwork(w);
            }
            if (id < 0) {
                if (userAsked) toast("보드 Wi-Fi 를 추가하지 못했습니다");
                return false;
            }
            boolean ok = wm.enableNetwork(id, true) && wm.reconnect();
            if (userAsked) toast(ok ? "보드 Wi-Fi 에 연결하는 중…" : "보드 Wi-Fi 연결 실패");
            if (ok && userAsked) {
                triedAp = true;
                ui.postDelayed(() -> discover("보드를 찾는 중…"), 6000);
            }
            return ok;
        } catch (SecurityException e) {
            if (userAsked) toast("Wi-Fi 변경 권한이 없습니다");
            return false;
        }
    }

    /**
     * 보드 AP 에는 인터넷이 없어서 Android 가 기본 네트워크로 쓰지 않을 수 있다.
     * Wi-Fi 가 바뀔 때마다 앱 트래픽을 Wi-Fi 에 묶고 보드를 다시 찾는다.
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
            @Override
            public void onAvailable(Network n) {
                cm.bindProcessToNetwork(n);
                ui.postDelayed(() -> {
                    String ip = boardIp;
                    if (ip == null) discover("Wi-Fi 연결됨 – 보드를 찾는 중…");
                    else bg.execute(() -> { if (!probe(ip, 1200)) ui.post(() -> lost("네트워크가 바뀌었습니다")); });
                }, 1500);
            }

            @Override
            public void onLost(Network n) {
                cm.bindProcessToNetwork(null);
            }
        });
    }

    // ------------------------------------------------------------ snapshot

    private void saveSnapshot(String url) {
        if (Build.VERSION.SDK_INT >= 23 && Build.VERSION.SDK_INT <= 28
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                   != PackageManager.PERMISSION_GRANTED) {
            pendingDownload = url;
            requestPermissions(new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, 1);
            return;
        }
        // DownloadManager 는 시스템 프로세스라 인터넷 없는 보드 AP 를 안 탈 수 있다.
        // 앱 프로세스(Wi-Fi 에 묶여 있음)에서 직접 받아 저장한다.
        final String name = "UsbCam_" + new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date()) + ".jpg";
        bg.execute(() -> {
            HttpURLConnection c = null;
            try {
                File dir = new File(Environment.getExternalStoragePublicDirectory(
                        Environment.DIRECTORY_PICTURES), "RV1106USB");
                if (!dir.isDirectory() && !dir.mkdirs()) throw new Exception("폴더를 만들 수 없습니다");
                File f = new File(dir, name);
                c = (HttpURLConnection) new URL(url).openConnection();
                c.setConnectTimeout(3000);
                c.setReadTimeout(10000);
                if (c.getResponseCode() != 200) throw new Exception("HTTP " + c.getResponseCode());
                long n = 0;
                try (InputStream in = c.getInputStream(); FileOutputStream out = new FileOutputStream(f)) {
                    byte[] buf = new byte[64 * 1024];
                    for (int r; (r = in.read(buf)) > 0; ) { out.write(buf, 0, r); n += r; }
                }
                MediaScannerConnection.scanFile(this, new String[]{f.getAbsolutePath()},
                        new String[]{"image/jpeg"}, null);
                final long kb = n / 1024;
                ui.post(() -> toast("사진/RV1106USB/" + name + " (" + kb + " KB)"));
            } catch (Exception e) {
                ui.post(() -> toast("저장 실패: " + e.getMessage()));
            } finally {
                if (c != null) c.disconnect();
            }
        });
    }

    /** 사진/RV1106USB 에 저장하고 갤러리에 알린다. 어느 스레드에서든 호출 가능. */
    private void writePhoto(String name, byte[] jpg) throws Exception {
        if (Build.VERSION.SDK_INT >= 23 && Build.VERSION.SDK_INT <= 28
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                   != PackageManager.PERMISSION_GRANTED) {
            ui.post(() -> requestPermissions(new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, 2));
            throw new Exception("저장 권한이 필요합니다. 허용 후 다시 찍어 주세요");
        }
        File dir = new File(Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_PICTURES), "RV1106USB");
        if (!dir.isDirectory() && !dir.mkdirs()) throw new Exception("폴더를 만들 수 없습니다");
        File f = new File(dir, name);
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(jpg);
        }
        MediaScannerConnection.scanFile(this, new String[]{f.getAbsolutePath()},
                new String[]{"image/jpeg"}, null);
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        if (code == 1 && pendingDownload != null) {
            String u = pendingDownload;
            pendingDownload = null;
            if (res.length > 0 && res[0] == PackageManager.PERMISSION_GRANTED) saveSnapshot(u);
            else toast("저장 권한이 없어 스냅샷을 저장하지 못했습니다");
        }
    }

    // ------------------------------------------------------------ manual

    private void askAddress() {
        final EditText e = new EditText(this);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        e.setHint("예: 172.30.1.44");
        String last = prefs.getString("last_ip", null);
        if (last != null) e.setText(last);
        new AlertDialog.Builder(this)
                .setTitle("보드 주소")
                .setView(e)
                .setPositiveButton("열기", (d, w) -> {
                    final String ip = e.getText().toString().trim().replaceAll("^https?://", "").replaceAll("[:/].*$", "");
                    if (ip.isEmpty()) return;
                    showPanel("확인 중…", ip);
                    bg.execute(() -> {
                        boolean ok = probe(ip, 1500);
                        ui.post(() -> {
                            if (ok) open(ip);
                            else showPanel("응답이 없습니다", ip + ":" + PORT + " 에서 카메라 서버를 찾지 못했습니다");
                        });
                    });
                })
                .setNegativeButton("취소", null)
                .show();
    }
}
