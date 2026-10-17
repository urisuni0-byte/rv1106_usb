// scannerd - TGO-ABC 두피 스캐너 장치 데몬 (RV1106)
//
// 연동 시나리오 (TGO-ABC 전용 무선 디바이스 연동 시나리오 20260812):
//
//	BLE (GATT)          장치 검색·Device Info·Wi-Fi 프로비저닝          ble.go, wifi.go
//	mDNS / DNS-SD       같은 LAN 에서 등록 장치 자동 검색 (_tgoabc._tcp)  mdns.go
//	HTTP                상태 확인, WebRTC 시그널링, JPEG 다운로드        http.go
//	WebRTC Media        H.264 실시간 미리보기 (RV1106 하드웨어 인코더)   camera.go, camhelper
//	WebRTC DataChannel  미리보기 시작/종료, 촬영 준비·시작·완료·실패      rtc.go, capture.go
package main

import (
	"encoding/json"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"
)

var version = "dev"

func main() {
	confPath := flag.String("c", "/opt/scanner/scanner.conf", "설정 파일")
	flag.Parse()
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)

	cfg, err := loadConfig(*confPath)
	if err != nil {
		log.Fatalf("설정: %v", err)
	}
	info := makeDeviceInfo(cfg)
	log.Printf("scannerd %s - %s (id %s, model %s, fw %s)", version, info.Name, info.ID, info.Model, info.FW)

	cam, err := NewCamera(cfg)
	if err != nil {
		log.Fatalf("카메라: %v", err)
	}
	lights := NewLights(cfg)
	lights.Set(LightOff)
	sensor := NewSensor(cfg)
	prov := NewProvisioner(cfg, info)

	srv := &Server{cfg: cfg, info: info, cam: cam, prov: prov, lights: lights, sensor: sensor}
	rtc, err := NewRTC(cam, Ctl{
		Status:       srv.status,
		StartCapture: func(t string) (string, error) { return srv.cap.Start(t) },
		SetLights:    lights.Set,
		ReadClimate:  srv.readClimate,
	})
	if err != nil {
		log.Fatalf("WebRTC: %v", err)
	}
	srv.rtc = rtc
	srv.cap = NewCapturer(cfg, cam, lights, sensor, func(ev CaptureEvent) { rtc.Broadcast(ev) })

	// 미리보기 상태는 미리보기를 요청한 앱에게, 촬영 가능 여부는 모두에게
	cam.onState = func(state string, info map[string]any, errText string) {
		rtc.BroadcastPreview(map[string]any{"type": "preview.state", "state": state, "info": info, "error": errText})
		if state == "started" && !srv.cap.Busy() {
			rtc.Broadcast(CaptureEvent{Type: "capture.state", State: CapReady})
		}
	}

	cam.onFocus = rtc.BroadcastFocus
	// 자동 초점 상태(맞추는 중/고정/수동, 렌즈 위치)는 미리보기를 보는 앱에게
	cam.onAF = func(data []byte) {
		m := map[string]any{}
		if json.Unmarshal(data, &m) == nil {
			m["type"] = "af"
			rtc.BroadcastPreview(m)
		}
	}

	WatchButton(cfg, func() {
		if _, err := srv.cap.Start("button"); err != nil {
			log.Printf("[hw] 버튼 무시: %v", err)
		}
	})

	if cfg.BLE {
		srv.ble = NewBLE(cfg, info, prov)
		go srv.ble.Run()
	}
	if cfg.MDNS {
		go RunMDNS(cfg, info)
	}
	go func() { // STA 상태를 주기적으로 맞춘다 (rvcam-net.sh 감시가 다시 붙이는 경우 등)
		for range time.Tick(5 * time.Second) {
			prov.Refresh()
		}
	}()

	addr := ":" + strconv.Itoa(cfg.HTTPPort)
	hs := &http.Server{Addr: addr, Handler: srv.Handler(), ReadHeaderTimeout: 10 * time.Second}
	go func() {
		log.Printf("HTTP %s", addr)
		if err := hs.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("HTTP: %v", err)
		}
	}()

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	log.Printf("종료")
	lights.Set(LightOff)
	if srv.ble != nil {
		srv.ble.Stop()
	}
	cam.Shutdown()
	hs.Close()
}
