package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"os/exec"
	"sync"
	"time"

	"github.com/go-ble/ble"
	"github.com/go-ble/ble/linux"
)

// GATT 서비스 (앱 BleProvisioner.java 와 같은 값)
var (
	uuidService    = ble.MustParse("e5a10001-7b4c-4c2d-9f3e-5447f041bc00")
	uuidDeviceInfo = ble.MustParse("e5a10002-7b4c-4c2d-9f3e-5447f041bc00") // read: DeviceInfo JSON
	uuidWiFiConfig = ble.MustParse("e5a10003-7b4c-4c2d-9f3e-5447f041bc00") // write: {"ssid","password"}
	uuidProvStatus = ble.MustParse("e5a10004-7b4c-4c2d-9f3e-5447f041bc00") // read/notify: ProvStatus JSON
)

type BLE struct {
	cfg  Config
	info DeviceInfo
	prov *Provisioner

	mu        sync.Mutex
	notifiers map[ble.Notifier]struct{}
	state     string
	dev       *linux.Device
}

func NewBLE(cfg Config, info DeviceInfo, prov *Provisioner) *BLE {
	b := &BLE{cfg: cfg, info: info, prov: prov, notifiers: map[ble.Notifier]struct{}{}, state: "off"}
	prov.OnChange(b.pushStatus)
	return b
}

func (b *BLE) State() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.state
}

func (b *BLE) setState(s string) {
	b.mu.Lock()
	b.state = s
	b.mu.Unlock()
}

// Run 은 HCI 장치를 열고 광고를 계속한다. 실패하면 5초, 이후 30초마다 다시 시도하고,
// 두 번 연속 실패하면 BLE_RESET 으로 블루투스 컨트롤러를 다시 붙인다.
func (b *BLE) Run() {
	for fails := 1; ; fails++ {
		err := b.runOnce()
		b.setState("error: " + err.Error())
		wait := 30 * time.Second
		if fails == 1 {
			wait = 5 * time.Second
		}
		log.Printf("[ble] %v - %v 뒤 다시 시도", err, wait)
		if fails >= 2 && b.cfg.BLEReset != "" {
			log.Printf("[ble] 컨트롤러 다시 붙이기 (BLE_RESET)")
			if out, err := exec.Command("/bin/sh", "-c", b.cfg.BLEReset).CombinedOutput(); err != nil {
				log.Printf("[ble] BLE_RESET 실패: %v %s", err, out)
			}
		}
		time.Sleep(wait)
	}
}

// Stop 은 종료할 때 HCI 를 닫는다 (닫지 않고 죽으면 컨트롤러가 user channel 상태로 남는다).
func (b *BLE) Stop() {
	b.mu.Lock()
	dev := b.dev
	b.dev = nil
	b.mu.Unlock()
	if dev != nil {
		dev.Stop()
	}
}

// pickHCI 는 쓸 HCI 번호. BLE_HCI 가 음수면 /sys/class/bluetooth 의 첫 장치 (hciattach 를
// 다시 하면 hci1 처럼 번호가 바뀔 수 있다).
func (b *BLE) pickHCI() (int, bool) {
	if b.cfg.BLEHCI >= 0 {
		_, err := os.Stat(fmt.Sprintf("/sys/class/bluetooth/hci%d", b.cfg.BLEHCI))
		return b.cfg.BLEHCI, err == nil
	}
	ents, _ := os.ReadDir("/sys/class/bluetooth")
	best := -1
	for _, e := range ents {
		var n int
		if _, err := fmt.Sscanf(e.Name(), "hci%d", &n); err == nil && (best < 0 || n < best) {
			best = n
		}
	}
	return best, best >= 0
}

func (b *BLE) runOnce() error {
	hci, ok := b.pickHCI()
	if !ok && b.cfg.BLEInit != "" {
		log.Printf("[ble] HCI 없음 - BLE_INIT 실행: %s", b.cfg.BLEInit)
		out, err := exec.Command("/bin/sh", "-c", b.cfg.BLEInit).CombinedOutput()
		if err != nil {
			log.Printf("[ble] BLE_INIT 실패: %v %s", err, out)
		}
		time.Sleep(2 * time.Second)
		hci, ok = b.pickHCI()
	}
	if !ok {
		return fmt.Errorf("HCI 장치 없음 (블루투스 컨트롤러가 올라오지 않음)")
	}
	// 다른 블루투스 데몬이 장치를 쥐고 있으면 HCI user channel 을 열 수 없다
	exec.Command("killall", "bluetoothd").Run()

	dev, err := linux.NewDeviceWithName(b.info.Name, ble.OptDeviceID(hci))
	if err != nil {
		return fmt.Errorf("hci%d 열기 실패: %w", hci, err)
	}
	b.mu.Lock()
	b.dev = dev
	b.mu.Unlock()
	defer b.Stop()

	svc := ble.NewService(uuidService)

	ci := svc.NewCharacteristic(uuidDeviceInfo)
	ci.HandleRead(ble.ReadHandlerFunc(func(req ble.Request, rsp ble.ResponseWriter) {
		js, _ := json.Marshal(b.info)
		writeFrom(rsp, js, req.Offset())
	}))

	cw := svc.NewCharacteristic(uuidWiFiConfig)
	cw.HandleWrite(ble.WriteHandlerFunc(func(req ble.Request, rsp ble.ResponseWriter) {
		var m struct {
			SSID     string `json:"ssid"`
			Password string `json:"password"`
		}
		if err := json.Unmarshal(req.Data(), &m); err != nil {
			log.Printf("[ble] Wi-Fi 설정 JSON 오류: %v", err)
			rsp.SetStatus(ble.ErrInvalAttrValueLen)
			return
		}
		log.Printf("[ble] Wi-Fi 프로비저닝 요청 ssid=%q", m.SSID)
		if err := b.prov.Apply(m.SSID, m.Password); err != nil {
			log.Printf("[ble] 프로비저닝 거부: %v", err) // 실패 상태는 notify 로 간다
		}
	}))

	cs := svc.NewCharacteristic(uuidProvStatus)
	cs.HandleRead(ble.ReadHandlerFunc(func(req ble.Request, rsp ble.ResponseWriter) {
		js, _ := json.Marshal(b.prov.Status())
		writeFrom(rsp, js, req.Offset())
	}))
	cs.HandleNotify(ble.NotifyHandlerFunc(func(req ble.Request, n ble.Notifier) {
		b.mu.Lock()
		b.notifiers[n] = struct{}{}
		b.mu.Unlock()
		log.Printf("[ble] 상태 알림 구독 (%s)", req.Conn().RemoteAddr())
		js, _ := json.Marshal(b.prov.Status())
		n.Write(js)
		<-n.Context().Done()
		b.mu.Lock()
		delete(b.notifiers, n)
		b.mu.Unlock()
	}))

	if err := dev.AddService(svc); err != nil {
		return fmt.Errorf("GATT 서비스 등록 실패: %w", err)
	}
	b.setState("advertising")
	log.Printf("[ble] 광고 시작: %s (%s)", b.info.Name, uuidService)
	// HCI 가 살아 있는 동안 막힌다. 연결이 끊기면 go-ble 이 광고를 다시 켠다.
	err = dev.AdvertiseNameAndServices(context.Background(), b.info.Name, uuidService)
	return fmt.Errorf("광고 중단: %v", err)
}

// 긴 값은 Read Blob 으로 나눠 읽힌다
func writeFrom(rsp ble.ResponseWriter, b []byte, off int) {
	if off >= len(b) {
		return
	}
	rsp.Write(b[off:])
}

func (b *BLE) pushStatus(st ProvStatus) {
	js, _ := json.Marshal(st)
	b.mu.Lock()
	ns := make([]ble.Notifier, 0, len(b.notifiers))
	for n := range b.notifiers {
		ns = append(ns, n)
	}
	b.mu.Unlock()
	for _, n := range ns {
		if len(js) > n.Cap() {
			// MTU 가 작으면 알림 하나에 다 안 들어간다. 앱은 알림을 받으면 값을 다시 읽는다.
			n.Write([]byte(`{"state":"` + st.State + `"}`))
			continue
		}
		n.Write(js)
	}
}
