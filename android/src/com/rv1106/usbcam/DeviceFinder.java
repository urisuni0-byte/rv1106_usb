package com.rv1106.usbcam;

import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Handler;
import android.os.Looper;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * 등록된 장치를 같은 LAN 에서 찾는다 (자동 재연결 시나리오).
 *   1) 마지막 주소에 GET /api/device - 같은 id 면 바로 성공
 *   2) mDNS / DNS-SD (_tgoabc._tcp) 검색 - TXT 의 id 가 같은 서비스
 * 먼저 찾은 쪽을 쓴다. 콜백은 메인 스레드.
 */
final class DeviceFinder {

    static final String SERVICE = "_tgoabc._tcp.";
    /** 장치 AP(RvCam-xxxx)에서 장치의 주소 (board/rvcam-net.sh) */
    static final String AP_IP = "192.168.4.1";

    interface Callback {
        void onFound(String ip, int port, JSONObject info);
        void onFail(String why);
    }

    private final NsdManager nsd;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ExecutorService bg = Executors.newCachedThreadPool();

    private NsdManager.DiscoveryListener disc;
    private final ArrayDeque<NsdServiceInfo> resolveQueue = new ArrayDeque<>();
    private boolean resolving;
    private boolean finished = true;
    private Callback cb;
    private String wantId;

    DeviceFinder(Context ctx) {
        nsd = (NsdManager) ctx.getApplicationContext().getSystemService(Context.NSD_SERVICE);
    }

    /** id 장치를 찾는다. lastIp 가 있으면 먼저 확인한다. */
    void find(String id, String lastIp, int lastPort, long timeoutMs, Callback c) {
        cancel();
        finished = false;
        cb = c;
        wantId = id;
        if (lastIp != null && !lastIp.isEmpty()) {
            final String ip = lastIp;
            final int port = lastPort;
            bg.execute(() -> {
                JSONObject info = probe(ip, port, 1500);
                if (info != null && id.equals(info.optString("id"))) ui.post(() -> succeed(ip, port, info));
            });
        }
        // 패드가 장치 AP(RvCam-xxxx)에 바로 붙어 있으면 장치는 언제나 AP_IP 다 (mDNS 가 안 될 때도 찾도록)
        if (!AP_IP.equals(lastIp)) {
            final int port = lastPort > 0 ? lastPort : 8090;
            bg.execute(() -> {
                JSONObject info = probe(AP_IP, port, 1500);
                if (info != null && id.equals(info.optString("id"))) ui.post(() -> succeed(AP_IP, port, info));
            });
        }
        startNsd();
        ui.postDelayed(timeout, timeoutMs);
    }

    private final Runnable timeout = () -> fail("같은 Wi-Fi 에서 장치를 찾지 못했습니다");

    void cancel() {
        ui.removeCallbacks(timeout);
        finished = true;
        stopNsd();
        resolveQueue.clear();
    }

    private void succeed(String ip, int port, JSONObject info) {
        if (finished) return;
        Callback c = cb;
        cancel();
        c.onFound(ip, port, info);
    }

    private void fail(String why) {
        if (finished) return;
        Callback c = cb;
        cancel();
        c.onFail(why);
    }

    // ------------------------------------------------------------------ mDNS

    private void startNsd() {
        if (nsd == null) return;
        disc = new NsdManager.DiscoveryListener() {
            @Override public void onStartDiscoveryFailed(String t, int e) { disc = null; }
            @Override public void onStopDiscoveryFailed(String t, int e) { }
            @Override public void onDiscoveryStarted(String t) { }
            @Override public void onDiscoveryStopped(String t) { }
            @Override public void onServiceLost(NsdServiceInfo s) { }
            @Override public void onServiceFound(NsdServiceInfo s) {
                ui.post(() -> { if (!finished) { resolveQueue.add(s); resolveNext(); } });
            }
        };
        try {
            nsd.discoverServices(SERVICE, NsdManager.PROTOCOL_DNS_SD, disc);
        } catch (Exception e) {
            disc = null;
        }
    }

    private void stopNsd() {
        if (disc != null) {
            try { nsd.stopServiceDiscovery(disc); } catch (Exception ignored) { }
            disc = null;
        }
    }

    // NsdManager 는 resolve 를 한 번에 하나만 받는다
    private void resolveNext() {
        if (resolving || resolveQueue.isEmpty() || finished) return;
        resolving = true;
        nsd.resolveService(resolveQueue.poll(), new NsdManager.ResolveListener() {
            @Override public void onResolveFailed(NsdServiceInfo s, int e) {
                ui.post(() -> { resolving = false; resolveNext(); });
            }
            @Override public void onServiceResolved(NsdServiceInfo s) {
                String id = txt(s, "id");
                final String ip = s.getHost() != null ? s.getHost().getHostAddress() : null;
                final int port = s.getPort();
                ui.post(() -> { resolving = false; resolveNext(); });
                if (ip == null) return;
                if (id != null && !id.equals(wantId)) return;
                // TXT 가 없거나 맞으면 HTTP 로 한 번 더 확인
                bg.execute(() -> {
                    JSONObject info = probe(ip, port, 2000);
                    if (info != null && wantId.equals(info.optString("id"))) ui.post(() -> succeed(ip, port, info));
                });
            }
        });
    }

    private static String txt(NsdServiceInfo s, String k) {
        try {
            Map<String, byte[]> a = s.getAttributes();
            byte[] v = a != null ? a.get(k) : null;
            return v != null ? new String(v, StandardCharsets.UTF_8) : null;
        } catch (Exception e) {
            return null;
        }
    }

    // ------------------------------------------------------------------ HTTP

    /** GET http://ip:port/api/device -> Device Info (실패하면 null). 백그라운드 스레드에서. */
    static JSONObject probe(String ip, int port, int timeoutMs) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL("http://" + ip + ":" + port + "/api/device").openConnection();
            c.setConnectTimeout(timeoutMs);
            c.setReadTimeout(timeoutMs);
            if (c.getResponseCode() != 200) return null;
            JSONObject js = new JSONObject(readAll(c.getInputStream()));
            return js.has("id") ? js : null;
        } catch (Exception e) {
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    static String readAll(InputStream in) throws java.io.IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        for (int r; (r = in.read(buf)) > 0; ) b.write(buf, 0, r);
        in.close();
        return new String(b.toByteArray(), StandardCharsets.UTF_8);
    }
}
