package com.rv1106.usbcam;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/** 등록된 장치 목록 (최근 사용 순). 항목: id, name, model, fw, ip, port, ssid */
final class DeviceStore {

    private final SharedPreferences p;

    DeviceStore(Context ctx) {
        p = ctx.getSharedPreferences("tgoabc", Context.MODE_PRIVATE);
    }

    List<JSONObject> list() {
        List<JSONObject> out = new ArrayList<>();
        try {
            JSONArray a = new JSONArray(p.getString("devices", "[]"));
            for (int i = 0; i < a.length(); i++) out.add(a.getJSONObject(i));
        } catch (Exception ignored) { }
        return out;
    }

    /** 같은 id 를 갱신하고 맨 앞으로 옮긴다. */
    void save(JSONObject d) {
        List<JSONObject> l = list();
        String id = d.optString("id");
        JSONArray a = new JSONArray();
        a.put(d);
        for (JSONObject o : l) if (!id.equals(o.optString("id"))) a.put(o);
        p.edit().putString("devices", a.toString()).apply();
    }

    void remove(String id) {
        JSONArray a = new JSONArray();
        for (JSONObject o : list()) if (!id.equals(o.optString("id"))) a.put(o);
        p.edit().putString("devices", a.toString()).apply();
    }
}
