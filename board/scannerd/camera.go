package main

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"strconv"
	"sync"
	"syscall"
	"time"

	"github.com/pion/webrtc/v4"
	"github.com/pion/webrtc/v4/pkg/media"
)

// record 는 camhelper stdout 의 레코드 하나 (camhelper.c 머리말 참고).
type record struct {
	typ  byte
	pts  uint64
	data []byte
}

// helper 는 상주하는 camhelper run 프로세스. stdin 명령으로 모드를 바꾼다.
type helper struct {
	cmd   *exec.Cmd
	stdin io.WriteCloser
	mu    sync.Mutex
	done  chan struct{}
	err   error
}

func (h *helper) send(cmd string) {
	h.mu.Lock()
	io.WriteString(h.stdin, cmd+"\n")
	h.mu.Unlock()
}

// stop 은 quit 을 보내고, 3초 안에 안 끝나면 죽인다.
func (h *helper) stop() {
	h.send("quit")
	h.stdin.Close()
	select {
	case <-h.done:
	case <-time.After(3 * time.Second):
		h.cmd.Process.Kill()
		<-h.done
	}
}

// Camera 는 카메라 하나를 미리보기(YUYV -> H.264)와 사진(MJPEG) 모드 사이에서 바꿔 쓴다.
// 촬영하는 동안에도 미리보기를 보는 앱이 있으면 camhelper 가 사진 스트림을 디코딩·축소해
// H.264 로 계속 보낸다 (약 5fps). 모드를 바꾸는 순간에만 카메라가 1초 남짓 멈춘다.
type Camera struct {
	cfg   Config
	Track *webrtc.TrackLocalStaticSample

	opMu    sync.Mutex // 모드 전환을 한 번에 하나씩
	mu      sync.Mutex
	refs    int
	busy    bool
	h       *helper
	mode    string         // camhelper 가 마지막으로 알린 모드: idle | preview | photo
	info    map[string]any // 마지막 'I' 레코드
	lastErr string
	events  chan record // 'I', 'E' (모드 전환 대기용)
	jpegs   chan []byte // 'J' (snap 대기용)
	ctls    chan []byte // 'C' (컨트롤 응답)
	onState func(state string, info map[string]any, err string)
	onFocus func(data []byte) // 'F' 선명도 JSON (초점 맞춤 보조)
	onAF    func(data []byte) // 'A' 자동 초점 상태 JSON (MIPI 카메라 렌즈 VCM)
	focus   bool              // 초점 맞춤 보조를 켠 앱이 있다

	ctlMu  sync.Mutex // 컨트롤 요청을 한 번에 하나씩
	ctlSeq int
}

func NewCamera(cfg Config) (*Camera, error) {
	track, err := webrtc.NewTrackLocalStaticSample(
		webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264, ClockRate: 90000,
			SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"},
		"video", "tgoabc")
	if err != nil {
		return nil, err
	}
	return &Camera{cfg: cfg, Track: track}, nil
}

func (c *Camera) emit(state string, info map[string]any, err string) {
	if c.onState != nil {
		c.onState(state, info, err)
	}
}

// Acquire / Release: 미리보기를 원하는 세션 수.
func (c *Camera) Acquire() {
	c.mu.Lock()
	c.refs++
	c.mu.Unlock()
	c.reconcile()
	c.RequestIDR()
}

func (c *Camera) Release() {
	c.mu.Lock()
	if c.refs > 0 {
		c.refs--
	}
	c.mu.Unlock()
	c.reconcile()
}

// Shutdown 은 종료 시 camhelper 를 정리한다.
func (c *Camera) Shutdown() {
	c.opMu.Lock()
	defer c.opMu.Unlock()
	c.mu.Lock()
	h := c.h
	c.h, c.refs = nil, 0
	c.mu.Unlock()
	if h != nil {
		h.stop()
	}
}

func (c *Camera) RequestIDR() {
	c.mu.Lock()
	h := c.h
	c.mu.Unlock()
	if h != nil {
		h.send("idr")
	}
}

// SetFocus 는 초점 맞춤 보조(선명도 측정)를 켜고 끈다. camhelper 가 새로 떠도 이어진다.
func (c *Camera) SetFocus(on bool) {
	c.mu.Lock()
	changed := c.focus != on
	c.focus = on
	h := c.h
	c.mu.Unlock()
	if changed && h != nil {
		h.send(focusCmd(on))
	}
}

func focusCmd(on bool) string {
	if on {
		return "focus on"
	}
	return "focus off"
}

type PreviewStatus struct {
	Running bool           `json:"running"`
	Viewers int            `json:"viewers"`
	Busy    bool           `json:"capturing"`
	Mode    string         `json:"mode"`
	Info    map[string]any `json:"info,omitempty"`
	Error   string         `json:"error,omitempty"`
}

func previewing(info map[string]any) bool {
	p, _ := info["preview"].(bool)
	return p
}

func (c *Camera) Status() PreviewStatus {
	c.mu.Lock()
	defer c.mu.Unlock()
	return PreviewStatus{Running: c.h != nil && previewing(c.info), Viewers: c.refs, Busy: c.busy,
		Mode: c.mode, Info: viewerInfo(c.info), Error: c.lastErr}
}

// viewerInfo 는 앱에 보여 줄 미리보기 정보 (사진 모드에서는 축소 미리보기 크기)
func viewerInfo(info map[string]any) map[string]any {
	if info == nil {
		return nil
	}
	m := map[string]any{"mode": info["mode"], "camera_format": info["camera_format"],
		"width": info["width"], "height": info["height"], "fps": info["fps"], "kbps": info["kbps"]}
	if info["mode"] == "photo" {
		m["width"], m["height"] = info["preview_width"], info["preview_height"]
		m["fps"] = 5 // 소프트웨어 JPEG 디코딩 속도 (실측 약 6fps)
		m["source"] = "photo"
		m["photo_width"], m["photo_height"] = info["width"], info["height"]
	}
	return m
}

func (c *Camera) startHelper() (*helper, error) {
	args := []string{"run",
		"-W", strconv.Itoa(c.cfg.PreviewWidth), "-H", strconv.Itoa(c.cfg.PreviewHeight),
		"-f", strconv.Itoa(c.cfg.PreviewFPS), "-b", strconv.Itoa(c.cfg.PreviewKbps),
		"-g", strconv.Itoa(c.cfg.PreviewFPS),
		"-X", strconv.Itoa(c.cfg.PhotoWidth), "-Y", strconv.Itoa(c.cfg.PhotoHeight),
		"-s", strconv.Itoa(c.cfg.PhotoSkip)}
	if c.cfg.CameraDevice != "" {
		args = append(args, "-d", c.cfg.CameraDevice)
	}
	if c.cfg.AFRange != "" {
		args = append(args, "-A", c.cfg.AFRange)
	}
	if c.cfg.AFPriority != "" {
		args = append(args, "-P", c.cfg.AFPriority)
	}
	if c.cfg.AFMode != "" {
		args = append(args, "-M", c.cfg.AFMode)
	}
	closeOnExecAll()
	cmd := exec.Command(c.cfg.CamHelper, args...)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	cmd.Stderr = log.Writer()
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	h := &helper{cmd: cmd, stdin: stdin, done: make(chan struct{})}
	events, jpegs, ctls := make(chan record, 8), make(chan []byte, 1), make(chan []byte, 4)
	c.mu.Lock()
	c.events, c.jpegs, c.ctls = events, jpegs, ctls
	c.mu.Unlock()
	go c.pump(h, stdout, events, jpegs, ctls)
	return h, nil
}

// closeOnExecAll 은 열려 있는 fd 에 모두 close-on-exec 를 건다. go-ble 의 HCI 소켓은
// CLOEXEC 없이 열려서, 그대로 두면 camhelper 가 물려받아 scannerd 가 재시작해도 HCI 를 놓지 않는다.
func closeOnExecAll() {
	ents, _ := os.ReadDir("/proc/self/fd")
	for _, e := range ents {
		if fd, err := strconv.Atoi(e.Name()); err == nil && fd > 2 {
			syscall.CloseOnExec(fd)
		}
	}
}

// pump 는 camhelper 출력을 나눠 보낸다: H.264 -> WebRTC 트랙, JPEG -> snap, 정보/오류 -> 모드 전환.
func (c *Camera) pump(h *helper, stdout io.Reader, events chan record, jpegs chan []byte, ctls chan []byte) {
	defer close(h.done)
	r := bufio.NewReaderSize(stdout, 256<<10)
	frame := time.Second / time.Duration(max(c.cfg.PreviewFPS, 1))
	var last uint64
	var hdr [13]byte
	for {
		if _, err := io.ReadFull(r, hdr[:]); err != nil {
			break
		}
		n := binary.BigEndian.Uint32(hdr[1:5])
		if n > 32<<20 {
			h.err = fmt.Errorf("camhelper 레코드 길이 이상: %d", n)
			break
		}
		buf := make([]byte, n)
		if _, err := io.ReadFull(r, buf); err != nil {
			break
		}
		rec := record{typ: hdr[0], pts: binary.BigEndian.Uint64(hdr[5:13]), data: buf}
		switch rec.typ {
		case 'H':
			d := frame
			if last != 0 && rec.pts > last {
				d = time.Duration(rec.pts-last) * time.Microsecond
			}
			if rec.pts == last {
				d = 0 // 같은 프레임의 다음 조각
			}
			last = rec.pts
			c.Track.WriteSample(media.Sample{Data: rec.data, Duration: d})
		case 'F':
			if c.onFocus != nil {
				c.onFocus(rec.data)
			}
		case 'A':
			if c.onAF != nil {
				c.onAF(rec.data)
			}
		case 'J':
			select {
			case jpegs <- fixMJPEG(rec.data):
			default:
			}
		case 'I':
			var m map[string]any
			json.Unmarshal(rec.data, &m)
			mode, _ := m["mode"].(string)
			c.mu.Lock()
			c.mode, c.info, c.lastErr = mode, m, ""
			c.mu.Unlock()
			if previewing(m) {
				c.emit("started", viewerInfo(m), "")
			}
			select {
			case events <- rec:
			default:
			}
		case 'C':
			var r struct {
				Reopened bool `json:"reopened"`
			}
			json.Unmarshal(rec.data, &r)
			if r.Reopened { // 카메라가 다시 연결됨 -> 컨트롤이 초기화됐으니 저장된 값을 다시 넣는다
				log.Printf("[camera] 카메라 다시 연결됨 - 저장된 컨트롤 적용")
				go c.applySaved(h)
				break
			}
			select {
			case ctls <- rec.data:
			default:
			}
		case 'E':
			h.err = errors.New(string(rec.data))
			log.Printf("[camera] camhelper 오류: %s", rec.data)
			c.mu.Lock()
			c.lastErr = h.err.Error()
			c.mu.Unlock()
			select {
			case events <- rec:
			default:
			}
		}
	}
	h.cmd.Wait()

	c.mu.Lock()
	unexpected := c.h == h
	if unexpected {
		c.h, c.mode, c.info = nil, "", nil
		if h.err == nil {
			h.err = errors.New("camhelper 가 종료됨")
		}
		c.lastErr = h.err.Error()
	}
	c.mu.Unlock()
	if unexpected {
		log.Printf("[camera] camhelper 종료 (%v) - 2초 뒤 다시 시작", h.err)
		c.emit("failed", nil, h.err.Error())
		time.Sleep(2 * time.Second)
		c.reconcile()
	}
}

// waitMode 는 want 모드의 'I' 가 올 때까지 기다린다 (want 가 비면 아무 'I').
func waitMode(h *helper, ev chan record, want string) error {
	t := time.After(8 * time.Second)
	for {
		select {
		case rec := <-ev:
			if rec.typ == 'E' {
				return errors.New(string(rec.data))
			}
			var m map[string]any
			json.Unmarshal(rec.data, &m)
			if want == "" || m["mode"] == want {
				return nil
			}
		case <-h.done:
			return fmt.Errorf("camhelper 종료: %v", h.err)
		case <-t:
			return fmt.Errorf("카메라 %s 모드 전환 시간 초과", want)
		}
	}
}

// setMode 는 camhelper 에 모드 명령을 보내고 기다린다. opMu 를 잡고 부른다.
func (c *Camera) setMode(cmd, want string) error {
	c.mu.Lock()
	h, ev := c.h, c.events
	cur, info := c.mode, c.info
	c.mu.Unlock()
	if h == nil {
		return errors.New("camhelper 없음")
	}
	if cur == want && (want != "photo" || previewing(info) == (cmd == "photo")) {
		return nil
	}
	for len(ev) > 0 { // 지난 알림 버리기
		<-ev
	}
	h.send(cmd)
	return waitMode(h, ev, want)
}

// ensureHelper 는 camhelper 를 띄우고 첫 알림(idle)을 기다린다. opMu 를 잡고 부른다.
func (c *Camera) ensureHelper() error {
	c.mu.Lock()
	running := c.h != nil
	c.mu.Unlock()
	if running {
		return nil
	}
	h, err := c.startHelper()
	if err != nil {
		return err
	}
	c.mu.Lock()
	c.h = h
	ev := c.events
	c.mu.Unlock()
	if err := waitMode(h, ev, ""); err != nil {
		c.mu.Lock()
		if c.h == h {
			c.h = nil
		}
		c.mu.Unlock()
		h.stop()
		return err
	}
	c.applySaved(h)
	c.mu.Lock()
	focus := c.focus
	c.mu.Unlock()
	if focus {
		h.send(focusCmd(true))
	}
	return nil
}

// reconcile 은 원하는 상태(미리보기를 보는 앱 수)에 맞춰 모드를 바꾼다.
// 촬영 중에는 Photo 가 모드를 쥐고 있으므로 아무것도 하지 않는다.
func (c *Camera) reconcile() {
	c.opMu.Lock()
	defer c.opMu.Unlock()
	c.mu.Lock()
	refs, busy, h := c.refs, c.busy, c.h
	c.mu.Unlock()
	if busy {
		return
	}
	if refs == 0 {
		// 프로세스와 인코더는 살려 두고 카메라만 멈춘다 (다음 미리보기가 빨리 뜬다)
		if h != nil && c.setMode("idle", "idle") == nil {
			log.Printf("[camera] 미리보기 중지")
			c.emit("stopped", nil, "")
		}
		return
	}
	err := c.ensureHelper()
	if err == nil {
		err = c.setMode("preview", "preview")
	}
	if err != nil {
		c.mu.Lock()
		c.lastErr = err.Error()
		c.mu.Unlock()
		log.Printf("[camera] 미리보기 시작 실패: %v", err)
		c.emit("failed", nil, err.Error())
	}
}

// Photo 는 사진 모드로 바꾼 뒤 fn 에 snap 함수를 넘긴다. 미리보기를 보는 앱이 있으면 사진 모드에서도
// 미리보기가 이어진다. fn 이 끝나면 원래 모드로 돌아간다.
func (c *Camera) Photo(fn func(snap func() ([]byte, error)) error) error {
	c.mu.Lock()
	if c.busy {
		c.mu.Unlock()
		return errors.New("이미 촬영 중입니다")
	}
	c.busy = true
	withPreview := c.refs > 0
	c.mu.Unlock()
	defer func() {
		c.mu.Lock()
		c.busy = false
		c.mu.Unlock()
		c.reconcile()
	}()

	c.opMu.Lock()
	err := c.ensureHelper()
	if err == nil {
		cmd := "photo nopreview"
		if withPreview {
			cmd = "photo"
		}
		err = c.setMode(cmd, "photo")
	}
	c.mu.Lock()
	h, jpegs, info := c.h, c.jpegs, c.info
	c.mu.Unlock()
	c.opMu.Unlock()
	if err != nil {
		return err
	}
	log.Printf("[camera] 사진 모드 %v", info)
	select { // 이전 촬영에서 남은 JPEG 버리기
	case <-jpegs:
	default:
	}

	snap := func() ([]byte, error) {
		h.send("snap")
		select {
		case j := <-jpegs:
			return j, nil
		case <-h.done:
			return nil, fmt.Errorf("camhelper 종료: %v", h.err)
		case <-time.After(10 * time.Second):
			return nil, errors.New("사진 프레임 시간 초과")
		}
	}
	return fn(snap)
}
