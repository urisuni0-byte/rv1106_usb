package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"log"
	"sync"
	"time"

	"github.com/pion/ice/v4"
	"github.com/pion/rtcp"
	"github.com/pion/webrtc/v4"
)

// Session 은 앱 하나와의 WebRTC 연결.
type Session struct {
	ID      string
	pc      *webrtc.PeerConnection
	created time.Time

	mu         sync.Mutex
	dc         *webrtc.DataChannel
	previewing bool
	focus      bool // 초점 맞춤 보조 (선명도 수신)
	localCands []webrtc.ICECandidateInit
	closed     bool
}

// Ctl 은 DataChannel 메시지 처리기가 쓰는 장치 기능.
type Ctl struct {
	Status       func() any
	StartCapture func(trigger string) (string, error)
	SetLights    func(LightMode)
	ReadClimate  func() (*Climate, error)
}

type RTC struct {
	api *webrtc.API
	cam *Camera
	ctl Ctl

	mu       sync.Mutex
	sessions map[string]*Session
}

func NewRTC(cam *Camera, ctl Ctl) (*RTC, error) {
	m := &webrtc.MediaEngine{}
	// H.264 Baseline 만 내보낸다 (하드웨어 인코더 출력과 같은 프로파일)
	for i, fmtp := range []string{
		"level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
		"level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f",
	} {
		if err := m.RegisterCodec(webrtc.RTPCodecParameters{
			RTPCodecCapability: webrtc.RTPCodecCapability{
				MimeType: webrtc.MimeTypeH264, ClockRate: 90000, SDPFmtpLine: fmtp,
				RTCPFeedback: []webrtc.RTCPFeedback{{Type: "nack"}, {Type: "nack", Parameter: "pli"}, {Type: "ccm", Parameter: "fir"}},
			},
			PayloadType: webrtc.PayloadType(102 + 2*i),
		}, webrtc.RTPCodecTypeVideo); err != nil {
			return nil, err
		}
	}
	se := webrtc.SettingEngine{}
	// 앱(Chrome WebView)은 자기 주소를 xxx.local 로 숨긴다. 보드는 그 이름을 풀 필요 없이
	// 앱이 보내 오는 연결 확인에서 주소를 배운다(peer-reflexive). 5353 포트는 mDNS 광고 몫이다.
	se.SetICEMulticastDNSMode(ice.MulticastDNSModeDisabled)
	se.SetNetworkTypes([]webrtc.NetworkType{webrtc.NetworkTypeUDP4, webrtc.NetworkTypeTCP4})
	se.SetInterfaceFilter(func(name string) bool { return name != "lo" && name != "usb0" })
	api := webrtc.NewAPI(webrtc.WithMediaEngine(m), webrtc.WithSettingEngine(se))
	return &RTC{api: api, cam: cam, ctl: ctl, sessions: map[string]*Session{}}, nil
}

func newID() string {
	b := make([]byte, 8)
	rand.Read(b)
	return hex.EncodeToString(b)
}

// Offer 는 앱의 SDP offer 를 받아 answer 를 만든다 (HTTP POST /api/webrtc/offer).
// 보드 쪽 ICE 후보는 answer SDP 에 모두 담아서 준다 (LAN 이라 수집이 금방 끝난다).
func (r *RTC) Offer(offerSDP string) (*Session, string, error) {
	pc, err := r.api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		return nil, "", err
	}
	s := &Session{ID: newID(), pc: pc, created: time.Now()}

	sender, err := pc.AddTrack(r.cam.Track)
	if err != nil {
		pc.Close()
		return nil, "", err
	}
	go func() { // PLI/FIR 가 오면 키프레임을 다시 만든다
		for {
			pkts, _, err := sender.ReadRTCP()
			if err != nil {
				return
			}
			for _, p := range pkts {
				switch p.(type) {
				case *rtcp.PictureLossIndication, *rtcp.FullIntraRequest:
					r.cam.RequestIDR()
				}
			}
		}
	}()

	pc.OnICECandidate(func(c *webrtc.ICECandidate) {
		if c == nil {
			return
		}
		s.mu.Lock()
		s.localCands = append(s.localCands, c.ToJSON())
		s.mu.Unlock()
	})
	pc.OnDataChannel(func(dc *webrtc.DataChannel) {
		if dc.Label() != "control" {
			return
		}
		s.mu.Lock()
		s.dc = dc
		s.mu.Unlock()
		dc.OnOpen(func() {
			log.Printf("[rtc] %s 제어 채널 열림", s.ID)
			r.send(s, map[string]any{"type": "hello", "status": r.ctl.Status()})
		})
		dc.OnMessage(func(m webrtc.DataChannelMessage) { r.handle(s, m.Data) })
	})
	pc.OnConnectionStateChange(func(st webrtc.PeerConnectionState) {
		log.Printf("[rtc] %s 연결 상태 %s", s.ID, st)
		switch st {
		case webrtc.PeerConnectionStateConnected:
			r.cam.RequestIDR()
		case webrtc.PeerConnectionStateFailed, webrtc.PeerConnectionStateClosed:
			r.Close(s.ID)
		case webrtc.PeerConnectionStateDisconnected:
			// 잠깐 끊긴 것일 수 있다. 15초 안에 돌아오지 않으면 정리한다.
			time.AfterFunc(15*time.Second, func() {
				if pc.ConnectionState() == webrtc.PeerConnectionStateDisconnected {
					r.Close(s.ID)
				}
			})
		}
	})

	if err := pc.SetRemoteDescription(webrtc.SessionDescription{Type: webrtc.SDPTypeOffer, SDP: offerSDP}); err != nil {
		pc.Close()
		return nil, "", err
	}
	answer, err := pc.CreateAnswer(nil)
	if err != nil {
		pc.Close()
		return nil, "", err
	}
	gathered := webrtc.GatheringCompletePromise(pc)
	if err := pc.SetLocalDescription(answer); err != nil {
		pc.Close()
		return nil, "", err
	}
	select {
	case <-gathered:
	case <-time.After(3 * time.Second):
	}

	r.mu.Lock()
	r.sessions[s.ID] = s
	r.mu.Unlock()
	r.reap()
	log.Printf("[rtc] 세션 %s 생성", s.ID)
	return s, pc.LocalDescription().SDP, nil
}

// AddCandidate 는 앱이 trickle 로 보내는 ICE 후보 (HTTP POST /api/webrtc/candidate).
func (r *RTC) AddCandidate(id string, c webrtc.ICECandidateInit) error {
	s := r.get(id)
	if s == nil {
		return errors.New("세션 없음")
	}
	return s.pc.AddICECandidate(c)
}

// LocalCandidates 는 보드 쪽 ICE 후보 (HTTP GET /api/webrtc/candidates).
func (r *RTC) LocalCandidates(id string) ([]webrtc.ICECandidateInit, error) {
	s := r.get(id)
	if s == nil {
		return nil, errors.New("세션 없음")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]webrtc.ICECandidateInit(nil), s.localCands...), nil
}

func (r *RTC) get(id string) *Session {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.sessions[id]
}

func (r *RTC) Count() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.sessions)
}

func (r *RTC) Close(id string) {
	r.mu.Lock()
	s := r.sessions[id]
	delete(r.sessions, id)
	r.mu.Unlock()
	if s == nil {
		return
	}
	s.mu.Lock()
	was, focus := s.previewing, s.focus
	s.previewing, s.focus = false, false
	s.closed = true
	s.mu.Unlock()
	if was {
		r.cam.Release()
	}
	if focus {
		r.updateFocus()
	}
	s.pc.Close()
	log.Printf("[rtc] 세션 %s 종료", id)
}

// reap 은 연결되지 못한 채 남은 세션을 정리한다.
func (r *RTC) reap() {
	r.mu.Lock()
	var dead []string
	for id, s := range r.sessions {
		st := s.pc.ConnectionState()
		if time.Since(s.created) > 30*time.Second && st != webrtc.PeerConnectionStateConnected {
			dead = append(dead, id)
		}
	}
	r.mu.Unlock()
	for _, id := range dead {
		r.Close(id)
	}
}

func (r *RTC) send(s *Session, v any) {
	s.mu.Lock()
	dc := s.dc
	s.mu.Unlock()
	if dc == nil || dc.ReadyState() != webrtc.DataChannelStateOpen {
		return
	}
	b, _ := json.Marshal(v)
	dc.SendText(string(b))
}

// Broadcast 는 연결된 모든 앱에 메시지를 보낸다.
func (r *RTC) Broadcast(v any) {
	r.mu.Lock()
	list := make([]*Session, 0, len(r.sessions))
	for _, s := range r.sessions {
		list = append(list, s)
	}
	r.mu.Unlock()
	for _, s := range list {
		r.send(s, v)
	}
}

// BroadcastPreview 는 미리보기를 요청한 세션에게만 미리보기 상태를 알린다.
func (r *RTC) BroadcastPreview(v any) {
	r.mu.Lock()
	var list []*Session
	for _, s := range r.sessions {
		s.mu.Lock()
		if s.previewing {
			list = append(list, s)
		}
		s.mu.Unlock()
	}
	r.mu.Unlock()
	for _, s := range list {
		r.send(s, v)
	}
}

// focusSessions 는 초점 맞춤 보조를 켠 세션들.
func (r *RTC) focusSessions() []*Session {
	r.mu.Lock()
	defer r.mu.Unlock()
	var list []*Session
	for _, s := range r.sessions {
		s.mu.Lock()
		if s.focus {
			list = append(list, s)
		}
		s.mu.Unlock()
	}
	return list
}

// updateFocus 는 초점 맞춤 보조를 켠 세션이 하나라도 있으면 카메라 쪽 측정을 켠다.
func (r *RTC) updateFocus() { r.cam.SetFocus(len(r.focusSessions()) > 0) }

// BroadcastFocus 는 camhelper 의 선명도('F')를 초점 맞춤 보조를 켠 세션에게 보낸다.
func (r *RTC) BroadcastFocus(data []byte) {
	list := r.focusSessions()
	if len(list) == 0 {
		return
	}
	m := map[string]any{}
	if json.Unmarshal(data, &m) != nil {
		return
	}
	m["type"] = "focus"
	for _, s := range list {
		r.send(s, m)
	}
}

// ------------------------------------------------------------- DataChannel 메시지

type ctlMsg struct {
	Type  string `json:"type"`
	Req   any    `json:"req,omitempty"` // 앱이 붙인 요청 번호 - 응답의 re 로 돌려준다
	White *bool  `json:"white,omitempty"`
	UV    *bool  `json:"uv,omitempty"`
	ID    uint32 `json:"id,omitempty"`    // camera.control.set
	Value *int64 `json:"value,omitempty"` // camera.control.set
}

func (r *RTC) handle(s *Session, data []byte) {
	var m ctlMsg
	if err := json.Unmarshal(data, &m); err != nil {
		r.send(s, map[string]any{"type": "error", "error": "JSON 형식 오류"})
		return
	}
	reply := func(v map[string]any) {
		v["re"] = m.Req
		if _, ok := v["type"]; !ok {
			v["type"] = m.Type + ".result"
		}
		r.send(s, v)
	}

	switch m.Type {
	case "ping":
		reply(map[string]any{"type": "pong", "time": time.Now().UnixMilli()})

	case "status.get":
		reply(map[string]any{"type": "status", "status": r.ctl.Status()})

	case "preview.start": // 실시간 미리보기 시작 요청
		s.mu.Lock()
		was := s.previewing
		s.previewing = true
		s.mu.Unlock()
		if !was {
			r.cam.Acquire()
		} else {
			r.cam.RequestIDR()
		}
		st := r.cam.Status()
		state := "starting"
		if st.Running {
			state = "started"
		}
		reply(map[string]any{"type": "preview.state", "state": state, "info": st.Info, "error": st.Error})

	case "preview.stop": // 실시간 미리보기 종료 요청 -> 종료 응답
		s.mu.Lock()
		was := s.previewing
		s.previewing = false
		s.mu.Unlock()
		if was {
			r.cam.Release()
		}
		reply(map[string]any{"type": "preview.state", "state": "stopped"})

	case "focus.start", "focus.stop": // 초점 맞춤 보조: 미리보기 가운데 선명도를 약 5번/초 'focus' 로 보낸다
		s.mu.Lock()
		s.focus = m.Type == "focus.start"
		s.mu.Unlock()
		r.updateFocus()
		reply(map[string]any{"type": "focus.state", "on": m.Type == "focus.start"})

	case "capture.start", "button.press": // 앱에서 촬영 / 물리 버튼 흉내
		trig := "app"
		if m.Type == "button.press" {
			trig = "button"
		}
		id, err := r.ctl.StartCapture(trig)
		if err != nil {
			reply(map[string]any{"ok": false, "error": err.Error()})
			return
		}
		reply(map[string]any{"ok": true, "capture_id": id})

	case "light.set":
		mode := LightOff
		if m.White != nil && *m.White {
			mode = LightWhite
		} else if m.UV != nil && *m.UV {
			mode = LightUV
		}
		r.ctl.SetLights(mode)
		reply(map[string]any{"ok": true, "mode": mode})

	case "climate.read":
		c, err := r.ctl.ReadClimate()
		if err != nil {
			reply(map[string]any{"type": "climate", "ok": false, "error": err.Error()})
			return
		}
		reply(map[string]any{"type": "climate", "ok": true, "climate": c})

	case "camera.controls.get", "camera.control.set", "camera.controls.reset":
		// 카메라 응답을 기다리므로 DataChannel 처리 루프를 막지 않게 따로 돈다
		go func() {
			var ctls []CamControl
			var err error
			switch m.Type {
			case "camera.controls.get":
				ctls, err = r.cam.Controls()
			case "camera.control.set":
				if m.ID == 0 || m.Value == nil {
					err = errors.New("id 와 value 가 필요합니다")
				} else {
					ctls, err = r.cam.SetControl(m.ID, *m.Value)
				}
			default:
				ctls, err = r.cam.ResetControls()
			}
			out := map[string]any{"type": "camera.controls", "ok": err == nil, "controls": ctls}
			if err != nil {
				out["error"] = err.Error()
			}
			reply(out)
			if err == nil && m.Type != "camera.controls.get" { // 다른 앱 화면도 맞춘다
				r.Broadcast(map[string]any{"type": "camera.controls", "ok": true, "controls": ctls})
			}
		}()

	default:
		reply(map[string]any{"type": "error", "error": "알 수 없는 메시지: " + m.Type})
	}
}
