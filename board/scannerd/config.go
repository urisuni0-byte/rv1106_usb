package main

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
)

// Config 는 scanner.conf (셸 형식 KEY=VALUE) 에서 읽는다. init 스크립트도 같은 파일을 쓴다.
type Config struct {
	NamePrefix  string
	Model       string
	FWVersion   string
	HTTPPort    int
	DataDir     string
	CaptureKeep int

	CamHelper     string
	CameraDevice  string
	AFRange       string // MIPI 렌즈 AF 탐색 범위 "최소-최대" (VCM 0~1023, 비우면 전체)
	AFPriority    string // MIPI 렌즈 AF: far = 먼 쪽(두피) 우선, best = 가장 선명한 곳
	AFMode        string // MIPI 렌즈 AF: continuous = 미리보기 중 맞춰 두고 촬영은 바로, snap = 촬영 때마다
	PreviewWidth  int
	PreviewHeight int
	PreviewFPS    int
	PreviewKbps   int
	PhotoWidth    int
	PhotoHeight   int
	PhotoSkip     int
	CaptureUV     bool // 촬영 버튼에 UV광 촬영도 포함 (기본: 일반광 1장만)

	WhiteLEDGPIO    int
	UVLEDGPIO       int
	LEDActiveLow    bool
	ButtonGPIO      int
	ButtonActiveLow bool

	Sensor        string // sim | sht3x | none
	SensorI2CBus  int
	SensorI2CAddr int

	BLE      bool
	BLEHCI   int
	BLEInit  string // HCI 장치가 없을 때 먼저 실행할 셸 명령 (예: hciattach ...)
	BLEReset string // HCI 를 계속 못 열 때 컨트롤러를 다시 붙이는 셸 명령

	MDNS        bool
	MDNSService string

	NetScript string
	NetCfgDir string
	StaIf     string
	ApIf      string
}

func defaultConfig() Config {
	return Config{
		NamePrefix:  "TGOABC",
		Model:       "TGO-ABC-S1",
		FWVersion:   "0.1.0",
		HTTPPort:    8090,
		DataDir:     "/userdata/scanner",
		CaptureKeep: 20,

		CamHelper:     "/opt/scanner/camhelper",
		PreviewWidth:  640,
		PreviewHeight: 480,
		PreviewFPS:    20,
		PreviewKbps:   2000,
		PhotoWidth:    2592,
		PhotoHeight:   1944,
		PhotoSkip:     4,

		WhiteLEDGPIO:    -1,
		UVLEDGPIO:       -1,
		ButtonGPIO:      -1,
		ButtonActiveLow: true,

		Sensor:        "sim",
		SensorI2CBus:  3,
		SensorI2CAddr: 0x44,

		BLE:    true,
		BLEHCI: -1,

		MDNS:        true,
		MDNSService: "_tgoabc._tcp",

		NetScript: "/usr/bin/rvcam-net.sh",
		NetCfgDir: "/userdata/rvcam",
		StaIf:     "wlan0",
		ApIf:      "wlan1",
	}
}

func loadConfig(path string) (Config, error) {
	c := defaultConfig()
	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return c, nil
		}
		return c, err
	}
	defer f.Close()

	sc := bufio.NewScanner(f)
	line := 0
	for sc.Scan() {
		line++
		s := strings.TrimSpace(strings.TrimSuffix(sc.Text(), "\r"))
		if s == "" || strings.HasPrefix(s, "#") {
			continue
		}
		k, v, ok := strings.Cut(s, "=")
		if !ok {
			continue
		}
		k = strings.TrimSpace(k)
		v = strings.TrimSpace(v)
		if i := strings.Index(v, " #"); i >= 0 && !strings.HasPrefix(v, "\"") && !strings.HasPrefix(v, "'") {
			v = strings.TrimSpace(v[:i])
		}
		v = strings.Trim(v, "\"'")
		if err := c.set(k, v); err != nil {
			return c, fmt.Errorf("%s:%d: %s: %v", path, line, k, err)
		}
	}
	return c, sc.Err()
}

func (c *Config) set(k, v string) error {
	str := map[string]*string{
		"NAME_PREFIX": &c.NamePrefix, "MODEL": &c.Model, "FW_VERSION": &c.FWVersion,
		"DATA_DIR": &c.DataDir, "CAMHELPER": &c.CamHelper, "CAMERA_DEVICE": &c.CameraDevice, "AF_RANGE": &c.AFRange, "AF_PRIORITY": &c.AFPriority, "AF_MODE": &c.AFMode,
		"SENSOR": &c.Sensor, "BLE_INIT": &c.BLEInit, "BLE_RESET": &c.BLEReset, "MDNS_SERVICE": &c.MDNSService,
		"NET_SCRIPT": &c.NetScript, "NET_CFG_DIR": &c.NetCfgDir, "STA_IF": &c.StaIf, "AP_IF": &c.ApIf,
	}
	num := map[string]*int{
		"HTTP_PORT": &c.HTTPPort, "CAPTURE_KEEP": &c.CaptureKeep,
		"PREVIEW_WIDTH": &c.PreviewWidth, "PREVIEW_HEIGHT": &c.PreviewHeight, "PREVIEW_FPS": &c.PreviewFPS,
		"PREVIEW_KBPS": &c.PreviewKbps, "PHOTO_WIDTH": &c.PhotoWidth, "PHOTO_HEIGHT": &c.PhotoHeight,
		"PHOTO_SKIP": &c.PhotoSkip, "WHITE_LED_GPIO": &c.WhiteLEDGPIO, "UV_LED_GPIO": &c.UVLEDGPIO,
		"BUTTON_GPIO": &c.ButtonGPIO, "SENSOR_I2C_BUS": &c.SensorI2CBus, "SENSOR_I2C_ADDR": &c.SensorI2CAddr,
		"BLE_HCI": &c.BLEHCI,
	}
	flag := map[string]*bool{
		"LED_ACTIVE_LOW": &c.LEDActiveLow, "BUTTON_ACTIVE_LOW": &c.ButtonActiveLow,
		"BLE": &c.BLE, "MDNS": &c.MDNS, "CAPTURE_UV": &c.CaptureUV,
	}
	if p, ok := str[k]; ok {
		*p = v
		return nil
	}
	if p, ok := num[k]; ok {
		n, err := strconv.ParseInt(v, 0, 32) // 0x44 같은 16진수도 받는다
		if err != nil {
			return err
		}
		*p = int(n)
		return nil
	}
	if p, ok := flag[k]; ok {
		*p = v == "1" || strings.EqualFold(v, "true") || strings.EqualFold(v, "yes")
		return nil
	}
	return nil // 모르는 키는 init 스크립트 몫일 수 있으므로 무시
}

// DeviceInfo 는 BLE Device Info, mDNS TXT, HTTP /api/device 가 함께 쓴다.
type DeviceInfo struct {
	Name  string `json:"name"`
	ID    string `json:"id"`
	Model string `json:"model"`
	FW    string `json:"fw"`
	MAC   string `json:"mac"`
	Port  int    `json:"port"`
}

func makeDeviceInfo(c Config) DeviceInfo {
	mac := ifaceMAC(c.StaIf)
	hex := strings.ToUpper(strings.ReplaceAll(mac, ":", ""))
	if hex == "" {
		hex = "000000000000"
	}
	return DeviceInfo{
		Name:  c.NamePrefix + "-" + hex[len(hex)-4:],
		ID:    hex,
		Model: c.Model,
		FW:    c.FWVersion,
		MAC:   mac,
		Port:  c.HTTPPort,
	}
}

func ifaceMAC(name string) string {
	if ifc, err := net.InterfaceByName(name); err == nil && len(ifc.HardwareAddr) == 6 {
		return ifc.HardwareAddr.String()
	}
	b, err := os.ReadFile("/sys/class/net/" + name + "/address")
	if err == nil {
		return strings.TrimSpace(string(b))
	}
	return ""
}

func ifaceIPv4(name string) string {
	ifc, err := net.InterfaceByName(name)
	if err != nil {
		return ""
	}
	addrs, _ := ifc.Addrs()
	for _, a := range addrs {
		if n, ok := a.(*net.IPNet); ok && n.IP.To4() != nil {
			return n.IP.String()
		}
	}
	return ""
}
