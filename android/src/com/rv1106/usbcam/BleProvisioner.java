package com.rv1106.usbcam;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;

import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Collections;
import java.util.UUID;

/**
 * BLE(GATT) 로 장치를 찾고, Device Info 를 읽고, Wi-Fi 정보를 넘긴다.
 * 장치 쪽은 board/scannerd/ble.go 와 같은 UUID 를 쓴다.
 *
 *   e5a10001 서비스
 *   e5a10002 Device Info      read    {"name","id","model","fw","mac","port"}
 *   e5a10003 Wi-Fi Config     write   {"ssid","password"}
 *   e5a10004 Provision Status read/notify {"state":"idle|connecting|connected|failed","ip","port",...}
 *
 * 콜백은 모두 메인 스레드에서 부른다.
 */
final class BleProvisioner {

    static final UUID SVC = UUID.fromString("e5a10001-7b4c-4c2d-9f3e-5447f041bc00");
    static final UUID INFO = UUID.fromString("e5a10002-7b4c-4c2d-9f3e-5447f041bc00");
    static final UUID WIFI = UUID.fromString("e5a10003-7b4c-4c2d-9f3e-5447f041bc00");
    static final UUID STAT = UUID.fromString("e5a10004-7b4c-4c2d-9f3e-5447f041bc00");
    static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    interface Listener {
        void onFound(BluetoothDevice d, String name, int rssi);
        void onReady(JSONObject info);          // 연결 + Device Info 읽기 완료
        void onStatus(JSONObject status);       // 프로비저닝 상태 (notify)
        void onError(String msg);
        void onDisconnected();
    }

    private final Context ctx;
    private final Listener l;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final BluetoothAdapter adapter;

    private BluetoothLeScanner scanner;
    private ScanCallback scanCb;
    private BluetoothGatt gatt;
    private boolean ready;

    // GATT 요청은 한 번에 하나씩만 보낼 수 있다
    private final ArrayDeque<Runnable> ops = new ArrayDeque<>();
    private boolean opBusy;

    BleProvisioner(Context ctx, Listener l) {
        this.ctx = ctx.getApplicationContext();
        this.l = l;
        BluetoothManager bm = (BluetoothManager) ctx.getSystemService(Context.BLUETOOTH_SERVICE);
        adapter = bm != null ? bm.getAdapter() : null;
    }

    boolean available() { return adapter != null; }
    boolean enabled() { return adapter != null && adapter.isEnabled(); }

    // ------------------------------------------------------------------ 검색

    void startScan() {
        stopScan();
        if (!enabled()) { l.onError("블루투스가 꺼져 있습니다"); return; }
        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) { l.onError("BLE 검색을 시작할 수 없습니다"); return; }
        scanCb = new ScanCallback() {
            @Override public void onScanResult(int type, ScanResult r) {
                String name = r.getScanRecord() != null ? r.getScanRecord().getDeviceName() : null;
                if (name == null) name = r.getDevice().getName();
                final String n = name != null ? name : r.getDevice().getAddress();
                final BluetoothDevice d = r.getDevice();
                final int rssi = r.getRssi();
                ui.post(() -> l.onFound(d, n, rssi));
            }
            @Override public void onScanFailed(int code) {
                ui.post(() -> l.onError("BLE 검색 실패 (" + code + ")"));
            }
        };
        ScanFilter f = new ScanFilter.Builder().setServiceUuid(new ParcelUuid(SVC)).build();
        ScanSettings s = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        try {
            scanner.startScan(Collections.singletonList(f), s, scanCb);
        } catch (SecurityException e) {
            l.onError("블루투스 권한이 없습니다");
        }
    }

    void stopScan() {
        if (scanner != null && scanCb != null) {
            try { scanner.stopScan(scanCb); } catch (Exception ignored) { }
        }
        scanCb = null;
    }

    // ------------------------------------------------------------------ 연결

    void connect(BluetoothDevice d) {
        stopScan();
        close();
        ready = false;
        gatt = Build.VERSION.SDK_INT >= 23
                ? d.connectGatt(ctx, false, cb, BluetoothDevice.TRANSPORT_LE)
                : d.connectGatt(ctx, false, cb);
        ui.postDelayed(connectTimeout, 15000);
    }

    private final Runnable connectTimeout = () -> {
        if (!ready) { BleProvisioner.this.l.onError("장치 연결 시간 초과"); close(); }
    };

    /** Wi-Fi Provisioning 요청 (SSID/PW). 결과는 onStatus 로 온다. */
    boolean provision(String ssid, String password) {
        if (gatt == null || !ready) return false;
        final BluetoothGattCharacteristic c = ch(WIFI);
        if (c == null) return false;
        String js;
        try {
            js = new JSONObject().put("ssid", ssid).put("password", password).toString();
        } catch (Exception e) { return false; }
        final byte[] v = js.getBytes(StandardCharsets.UTF_8);
        enqueue(() -> {
            c.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            c.setValue(v);
            if (!gatt.writeCharacteristic(c)) { done(); ui.post(() -> l.onError("Wi-Fi 정보 전송 실패")); }
        });
        return true;
    }

    void readStatus() {
        final BluetoothGattCharacteristic c = ch(STAT);
        if (c != null) enqueue(() -> { if (!gatt.readCharacteristic(c)) done(); });
    }

    void close() {
        ui.removeCallbacks(connectTimeout);
        ops.clear();
        opBusy = false;
        if (gatt != null) {
            try { gatt.disconnect(); gatt.close(); } catch (Exception ignored) { }
            gatt = null;
        }
        ready = false;
    }

    private BluetoothGattCharacteristic ch(UUID u) {
        if (gatt == null) return null;
        BluetoothGattService s = gatt.getService(SVC);
        return s != null ? s.getCharacteristic(u) : null;
    }

    private void enqueue(Runnable r) {
        ui.post(() -> { ops.add(r); next(); });
    }

    private void next() {
        if (opBusy || ops.isEmpty() || gatt == null) return;
        opBusy = true;
        ops.poll().run();
    }

    private void done() {
        ui.post(() -> { opBusy = false; next(); });
    }

    private static JSONObject parse(byte[] v) {
        try { return new JSONObject(new String(v, StandardCharsets.UTF_8)); }
        catch (Exception e) { return null; }
    }

    private final BluetoothGattCallback cb = new BluetoothGattCallback() {
        @Override public void onConnectionStateChange(BluetoothGatt g, int status, int state) {
            if (state == BluetoothProfile.STATE_CONNECTED) {
                // Device Info / 상태 JSON 이 한 패킷에 들어가도록 MTU 를 키운다 (안 되면 Read Blob 으로 나눠 읽힌다)
                if (!g.requestMtu(247)) g.discoverServices();
            } else if (state == BluetoothProfile.STATE_DISCONNECTED) {
                ui.post(() -> {
                    boolean was = gatt != null;
                    close();
                    if (was) l.onDisconnected();
                });
            }
        }

        @Override public void onMtuChanged(BluetoothGatt g, int mtu, int status) {
            g.discoverServices();
        }

        @Override public void onServicesDiscovered(BluetoothGatt g, int status) {
            BluetoothGattCharacteristic st = ch(STAT), info = ch(INFO);
            if (status != BluetoothGatt.GATT_SUCCESS || st == null || info == null) {
                ui.post(() -> { l.onError("스캐너 서비스를 찾지 못했습니다"); close(); });
                return;
            }
            // 1) 상태 알림 구독  2) Device Info 요청
            enqueue(() -> {
                g.setCharacteristicNotification(st, true);
                BluetoothGattDescriptor d = st.getDescriptor(CCCD);
                if (d == null) { done(); return; }
                d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                if (!g.writeDescriptor(d)) done();
            });
            enqueue(() -> { if (!g.readCharacteristic(info)) done(); });
        }

        @Override public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            done();
        }

        @Override public void onCharacteristicRead(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            final JSONObject js = status == BluetoothGatt.GATT_SUCCESS ? parse(c.getValue()) : null;
            if (INFO.equals(c.getUuid())) {
                ui.post(() -> {
                    ui.removeCallbacks(connectTimeout);
                    if (js == null) { l.onError("Device Info 를 읽지 못했습니다"); close(); return; }
                    ready = true;
                    l.onReady(js);
                });
            } else if (STAT.equals(c.getUuid()) && js != null) {
                ui.post(() -> l.onStatus(js));
            }
            done();
        }

        @Override public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) ui.post(() -> l.onError("Wi-Fi 정보 전송 실패 (" + status + ")"));
            done();
        }

        @Override public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            if (!STAT.equals(c.getUuid())) return;
            final JSONObject js = parse(c.getValue());
            if (js == null) return;
            // MTU 가 작으면 장치가 {"state":..} 만 보낸다 -> 전체 값을 다시 읽는다
            if (!js.has("id")) readStatus();
            else ui.post(() -> l.onStatus(js));
        }
    };
}
