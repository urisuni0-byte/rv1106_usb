package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"sync"
	"time"
)

// 촬영 상태 (DataChannel capture.state 의 state 값)
const (
	CapReady     = "ready"     // 촬영 가능
	CapStarted   = "started"   // 버튼 입력 -> 촬영 시작
	CapCompleted = "completed" // 이미지·온습도 준비됨 (HTTP 로 받기)
	CapFailed    = "failed"
)

type CaptureImage struct {
	Kind  string `json:"kind"` // white | uv
	URL   string `json:"url"`
	Bytes int    `json:"bytes"`
}

type CaptureMeta struct {
	ID              string         `json:"capture_id"`
	Time            time.Time      `json:"time"`
	Trigger         string         `json:"trigger"` // button | app
	Climate         *Climate       `json:"climate,omitempty"`
	Images          []CaptureImage `json:"images"`
	BundleURL       string         `json:"bundle_url"`
	LightsSimulated bool           `json:"lights_simulated,omitempty"`
}

// CaptureEvent 는 DataChannel 로 나가는 capture.state 메시지.
type CaptureEvent struct {
	Type    string       `json:"type"` // "capture.state"
	State   string       `json:"state"`
	ID      string       `json:"capture_id,omitempty"`
	Trigger string       `json:"trigger,omitempty"`
	Step    string       `json:"step,omitempty"`
	Steps   []string     `json:"steps,omitempty"` // 이번 촬영의 단계 (시작 알림에만)
	Error   string       `json:"error,omitempty"`
	Result  *CaptureMeta `json:"result,omitempty"`
}

type Capturer struct {
	cfg    Config
	cam    *Camera
	lights *Lights
	sensor Sensor
	notify func(CaptureEvent)

	mu      sync.Mutex
	running bool
	last    *CaptureMeta
}

var captureIDRe = regexp.MustCompile(`^[0-9]{8}-[0-9]{6}-[0-9]{3}$`)

func NewCapturer(cfg Config, cam *Camera, lights *Lights, sensor Sensor, notify func(CaptureEvent)) *Capturer {
	os.MkdirAll(cfg.DataDir, 0755)
	c := &Capturer{cfg: cfg, cam: cam, lights: lights, sensor: sensor, notify: notify}
	if list := c.List(); len(list) > 0 {
		c.last = &list[0]
	}
	return c
}

func (c *Capturer) Busy() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.running
}

func (c *Capturer) Last() *CaptureMeta {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.last
}

// Start 는 촬영을 백그라운드로 시작하고 촬영 ID 를 돌려준다.
func (c *Capturer) Start(trigger string) (string, error) {
	c.mu.Lock()
	if c.running {
		c.mu.Unlock()
		return "", errors.New("이미 촬영 중입니다")
	}
	c.running = true
	c.mu.Unlock()

	now := time.Now()
	id := now.Format("20060102-150405") + fmt.Sprintf("-%03d", now.Nanosecond()/1e6)
	go c.run(id, now, trigger)
	return id, nil
}

func (c *Capturer) run(id string, now time.Time, trigger string) {
	defer func() {
		c.lights.Set(LightOff)
		c.mu.Lock()
		c.running = false
		c.mu.Unlock()
		c.notify(CaptureEvent{Type: "capture.state", State: CapReady})
	}()
	log.Printf("[capture] %s 시작 (%s)", id, trigger)
	steps := []string{"white"}
	if c.cfg.CaptureUV {
		steps = append(steps, "uv")
	}
	if c.sensor != nil {
		steps = append(steps, "climate")
	}
	steps = append(steps, "save")
	ev := func(step string) {
		e := CaptureEvent{Type: "capture.state", State: CapStarted, ID: id, Trigger: trigger, Step: step}
		if step == "" {
			e.Steps = steps
		}
		c.notify(e)
	}
	ev("")

	// 일반광 1장 (CAPTURE_UV=1 이면 이어서 UV광 1장)
	var white, uv []byte
	err := c.cam.Photo(func(snap func() ([]byte, error)) error {
		var err error
		ev("white")
		c.lights.Set(LightWhite)
		if white, err = snap(); err != nil {
			return fmt.Errorf("일반광 촬영: %w", err)
		}
		if c.cfg.CaptureUV {
			ev("uv")
			c.lights.Set(LightUV)
			if uv, err = snap(); err != nil {
				return fmt.Errorf("UV광 촬영: %w", err)
			}
		}
		c.lights.Set(LightOff)
		return nil
	})
	if err != nil {
		c.fail(id, trigger, err)
		return
	}

	meta := CaptureMeta{ID: id, Time: now, Trigger: trigger, LightsSimulated: c.lights.Simulated()}
	if c.sensor != nil {
		ev("climate")
		if cl, err := c.sensor.Read(); err != nil {
			log.Printf("[capture] 온·습도 측정 실패: %v", err)
		} else {
			meta.Climate = &cl
		}
	}

	ev("save")
	dir := filepath.Join(c.cfg.DataDir, id)
	if err := os.MkdirAll(dir, 0755); err != nil {
		c.fail(id, trigger, err)
		return
	}
	for _, im := range []struct {
		kind string
		data []byte
	}{{"white", white}, {"uv", uv}} {
		if im.data == nil {
			continue
		}
		if err := writeFileAtomic(filepath.Join(dir, im.kind+".jpg"), im.data); err != nil {
			c.fail(id, trigger, err)
			return
		}
		meta.Images = append(meta.Images, CaptureImage{
			Kind: im.kind, URL: "/api/captures/" + id + "/" + im.kind + ".jpg", Bytes: len(im.data)})
	}
	meta.BundleURL = "/api/captures/" + id + "/bundle"
	js, _ := json.MarshalIndent(meta, "", "  ")
	if err := writeFileAtomic(filepath.Join(dir, "meta.json"), js); err != nil {
		c.fail(id, trigger, err)
		return
	}
	c.prune()

	c.mu.Lock()
	c.last = &meta
	c.mu.Unlock()
	log.Printf("[capture] %s 완료: 일반광 %d KB, UV %d KB, %+v", id, len(white)/1024, len(uv)/1024, meta.Climate)
	c.notify(CaptureEvent{Type: "capture.state", State: CapCompleted, ID: id, Trigger: trigger, Result: &meta})
}

func (c *Capturer) fail(id, trigger string, err error) {
	log.Printf("[capture] %s 실패: %v", id, err)
	c.notify(CaptureEvent{Type: "capture.state", State: CapFailed, ID: id, Trigger: trigger, Error: err.Error()})
}

func writeFileAtomic(path string, b []byte) error {
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, b, 0644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// List 는 최신순 촬영 목록.
func (c *Capturer) List() []CaptureMeta {
	ents, _ := os.ReadDir(c.cfg.DataDir)
	var out []CaptureMeta
	for _, e := range ents {
		if !e.IsDir() || !captureIDRe.MatchString(e.Name()) {
			continue
		}
		if m, err := c.Get(e.Name()); err == nil {
			out = append(out, *m)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID > out[j].ID })
	return out
}

func (c *Capturer) Get(id string) (*CaptureMeta, error) {
	if !captureIDRe.MatchString(id) {
		return nil, os.ErrNotExist
	}
	b, err := os.ReadFile(filepath.Join(c.cfg.DataDir, id, "meta.json"))
	if err != nil {
		return nil, err
	}
	var m CaptureMeta
	if err := json.Unmarshal(b, &m); err != nil {
		return nil, err
	}
	return &m, nil
}

func (c *Capturer) ImagePath(id, kind string) (string, bool) {
	if !captureIDRe.MatchString(id) || (kind != "white" && kind != "uv") {
		return "", false
	}
	return filepath.Join(c.cfg.DataDir, id, kind+".jpg"), true
}

// prune 은 오래된 촬영을 지워 /userdata 가 차지 않게 한다.
func (c *Capturer) prune() {
	list := c.List()
	for i := c.cfg.CaptureKeep; i < len(list); i++ {
		os.RemoveAll(filepath.Join(c.cfg.DataDir, list[i].ID))
	}
}
