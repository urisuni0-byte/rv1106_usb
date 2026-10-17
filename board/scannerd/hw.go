package main

import (
	"errors"
	"fmt"
	"log"
	"math"
	"math/rand"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/sys/unix"
)

// ------------------------------------------------------------------ GPIO (sysfs)

type gpio struct {
	num       int
	activeLow bool
}

func gpioPath(n int, f string) string { return fmt.Sprintf("/sys/class/gpio/gpio%d/%s", n, f) }

func openGPIO(num int, out, activeLow bool) (*gpio, error) {
	if num < 0 {
		return nil, nil
	}
	if _, err := os.Stat(gpioPath(num, "value")); err != nil {
		if err := os.WriteFile("/sys/class/gpio/export", []byte(strconv.Itoa(num)), 0); err != nil {
			return nil, fmt.Errorf("gpio%d export: %w", num, err)
		}
		time.Sleep(100 * time.Millisecond) // udev 가 권한을 잡을 시간
	}
	dir := "in"
	if out {
		dir = "low"
		if activeLow {
			dir = "high" // 꺼진 상태로 시작
		}
	}
	if err := os.WriteFile(gpioPath(num, "direction"), []byte(dir), 0); err != nil {
		return nil, fmt.Errorf("gpio%d direction: %w", num, err)
	}
	return &gpio{num: num, activeLow: activeLow}, nil
}

func (g *gpio) set(on bool) error {
	v := on != g.activeLow
	s := "0"
	if v {
		s = "1"
	}
	return os.WriteFile(gpioPath(g.num, "value"), []byte(s), 0)
}

func (g *gpio) get() (bool, error) {
	b, err := os.ReadFile(gpioPath(g.num, "value"))
	if err != nil {
		return false, err
	}
	return (strings.TrimSpace(string(b)) == "1") != g.activeLow, nil
}

// ------------------------------------------------------------------ 조명

type LightMode string

const (
	LightOff   LightMode = "off"
	LightWhite LightMode = "white"
	LightUV    LightMode = "uv"
)

type Lights struct {
	mu        sync.Mutex
	white, uv *gpio
	mode      LightMode
}

func NewLights(c Config) *Lights {
	l := &Lights{mode: LightOff}
	var err error
	if l.white, err = openGPIO(c.WhiteLEDGPIO, true, c.LEDActiveLow); err != nil {
		log.Printf("[hw] 일반광 LED: %v (시뮬레이션으로 동작)", err)
	}
	if l.uv, err = openGPIO(c.UVLEDGPIO, true, c.LEDActiveLow); err != nil {
		log.Printf("[hw] UV LED: %v (시뮬레이션으로 동작)", err)
	}
	return l
}

func (l *Lights) Set(m LightMode) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.white != nil {
		l.white.set(m == LightWhite)
	}
	if l.uv != nil {
		l.uv.set(m == LightUV)
	}
	if l.white == nil || l.uv == nil {
		log.Printf("[hw] 조명(시뮬레이션): %s", m)
	}
	l.mode = m
}

func (l *Lights) Mode() LightMode {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.mode
}

func (l *Lights) Simulated() bool { return l.white == nil || l.uv == nil }

// ------------------------------------------------------------------ 버튼

// WatchButton 은 버튼이 눌릴 때마다 press 를 부른다 (20ms 폴링, 디바운스 포함).
// GPIO 가 없으면 아무것도 하지 않는다 - 그때는 HTTP/DataChannel 로 버튼을 흉내 낸다.
func WatchButton(c Config, press func()) {
	g, err := openGPIO(c.ButtonGPIO, false, c.ButtonActiveLow)
	if err != nil {
		log.Printf("[hw] 버튼: %v", err)
	}
	if g == nil {
		log.Printf("[hw] 물리 버튼 없음 - POST /api/button 또는 DataChannel button.press 로 시뮬레이션")
		return
	}
	go func() {
		prev, stable := false, 0
		for {
			time.Sleep(20 * time.Millisecond)
			on, err := g.get()
			if err != nil {
				continue
			}
			if on == prev {
				stable = 0
				continue
			}
			if stable++; stable < 3 { // 60ms 동안 같은 값이어야 인정
				continue
			}
			prev, stable = on, 0
			if on {
				log.Printf("[hw] 물리 버튼 눌림")
				press()
			}
		}
	}()
}

// ------------------------------------------------------------------ 온·습도 센서

type Climate struct {
	Temperature float64 `json:"temperature"`
	Humidity    float64 `json:"humidity"`
	Simulated   bool    `json:"simulated,omitempty"`
}

type Sensor interface {
	Read() (Climate, error)
}

func NewSensor(c Config) Sensor {
	switch c.Sensor {
	case "sht3x", "sht30", "sht31":
		return &sht3x{bus: c.SensorI2CBus, addr: c.SensorI2CAddr}
	case "none":
		return nil
	default:
		return simSensor{}
	}
}

type simSensor struct{}

func (simSensor) Read() (Climate, error) {
	time.Sleep(30 * time.Millisecond)
	return Climate{
		Temperature: round1(24.0 + rand.Float64()*3),
		Humidity:    round1(40.0 + rand.Float64()*15),
		Simulated:   true,
	}, nil
}

// Sensirion SHT3x: single shot, high repeatability, clock stretching 없음 (0x2400)
type sht3x struct {
	bus, addr int
	mu        sync.Mutex
}

const i2cSlave = 0x0703

func (s *sht3x) Read() (Climate, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	f, err := os.OpenFile(fmt.Sprintf("/dev/i2c-%d", s.bus), os.O_RDWR, 0)
	if err != nil {
		return Climate{}, err
	}
	defer f.Close()
	if err := unix.IoctlSetInt(int(f.Fd()), i2cSlave, s.addr); err != nil {
		return Climate{}, fmt.Errorf("I2C_SLAVE 0x%x: %w", s.addr, err)
	}
	if _, err := f.Write([]byte{0x24, 0x00}); err != nil {
		return Climate{}, fmt.Errorf("SHT3x 측정 명령: %w", err)
	}
	time.Sleep(20 * time.Millisecond)
	b := make([]byte, 6)
	if _, err := f.Read(b); err != nil {
		return Climate{}, fmt.Errorf("SHT3x 읽기: %w", err)
	}
	if crc8(b[0:2]) != b[2] || crc8(b[3:5]) != b[5] {
		return Climate{}, errors.New("SHT3x CRC 오류")
	}
	t := float64(uint16(b[0])<<8|uint16(b[1]))*175/65535 - 45
	h := float64(uint16(b[3])<<8|uint16(b[4])) * 100 / 65535
	return Climate{Temperature: round1(t), Humidity: round1(h)}, nil
}

func crc8(b []byte) byte {
	c := byte(0xFF)
	for _, x := range b {
		c ^= x
		for i := 0; i < 8; i++ {
			if c&0x80 != 0 {
				c = c<<1 ^ 0x31
			} else {
				c <<= 1
			}
		}
	}
	return c
}

func round1(f float64) float64 { return math.Round(f*10) / 10 }
