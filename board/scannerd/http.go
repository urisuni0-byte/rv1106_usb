package main

import (
	"embed"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"net/textproto"
	"os"
	"strings"

	"github.com/pion/webrtc/v4"
)

//go:embed web/viewer.html
var webFS embed.FS

type Server struct {
	cfg    Config
	info   DeviceInfo
	rtc    *RTC
	cam    *Camera
	cap    *Capturer
	prov   *Provisioner
	lights *Lights
	sensor Sensor
	ble    *BLE
}

func (s *Server) status() any {
	st := map[string]any{
		"device":  s.info,
		"wifi":    s.prov.Status(),
		"preview": s.cam.Status(),
		"capture": map[string]any{"busy": s.cap.Busy(), "last": s.cap.Last()},
		"lights":  s.lights.Mode(),
		"rtc":     map[string]any{"sessions": s.rtc.Count()},
		"ble":     "disabled",
	}
	if s.ble != nil {
		st["ble"] = s.ble.State()
	}
	return st
}

func (s *Server) readClimate() (*Climate, error) {
	if s.sensor == nil {
		return nil, fmt.Errorf("온·습도 센서 없음")
	}
	c, err := s.sensor.Read()
	if err != nil {
		return nil, err
	}
	return &c, nil
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, code int, err error) {
	writeJSON(w, code, map[string]any{"ok": false, "error": err.Error()})
}

func readJSON(r *http.Request, v any) error {
	b, err := io.ReadAll(io.LimitReader(r.Body, 256<<10))
	if err != nil {
		return err
	}
	return json.Unmarshal(b, v)
}

// cors: 앱이 asset 페이지에서 직접 부를 수도 있다
func cors(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS")
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		h.ServeHTTP(w, r)
	})
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()

	mux.HandleFunc("GET /{$}", func(w http.ResponseWriter, r *http.Request) {
		b, _ := webFS.ReadFile("web/viewer.html")
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-cache")
		w.Write(b)
	})

	// 장치 상태 / 정보
	mux.HandleFunc("GET /api/device", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, s.info)
	})
	mux.HandleFunc("GET /api/status", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, s.status())
	})

	// WebRTC 시그널링 (SDP Offer/Answer, ICE 후보 교환)
	mux.HandleFunc("POST /api/webrtc/offer", func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			Type string `json:"type"`
			SDP  string `json:"sdp"`
		}
		if err := readJSON(r, &req); err != nil || req.SDP == "" {
			writeErr(w, 400, fmt.Errorf("offer SDP 가 필요합니다"))
			return
		}
		sess, answer, err := s.rtc.Offer(req.SDP)
		if err != nil {
			writeErr(w, 500, err)
			return
		}
		writeJSON(w, 200, map[string]any{"session_id": sess.ID, "type": "answer", "sdp": answer})
	})
	mux.HandleFunc("POST /api/webrtc/candidate", func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			SessionID string                  `json:"session_id"`
			Candidate webrtc.ICECandidateInit `json:"candidate"`
		}
		if err := readJSON(r, &req); err != nil {
			writeErr(w, 400, err)
			return
		}
		if req.Candidate.Candidate == "" { // 수집 끝 표시
			writeJSON(w, 200, map[string]any{"ok": true})
			return
		}
		if err := s.rtc.AddCandidate(req.SessionID, req.Candidate); err != nil {
			writeErr(w, 404, err)
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true})
	})
	mux.HandleFunc("GET /api/webrtc/candidates", func(w http.ResponseWriter, r *http.Request) {
		c, err := s.rtc.LocalCandidates(r.URL.Query().Get("session_id"))
		if err != nil {
			writeErr(w, 404, err)
			return
		}
		writeJSON(w, 200, map[string]any{"candidates": c})
	})
	mux.HandleFunc("DELETE /api/webrtc/session", func(w http.ResponseWriter, r *http.Request) {
		s.rtc.Close(r.URL.Query().Get("session_id"))
		writeJSON(w, 200, map[string]any{"ok": true})
	})

	// 촬영
	mux.HandleFunc("POST /api/capture", func(w http.ResponseWriter, r *http.Request) {
		id, err := s.cap.Start("app")
		if err != nil {
			writeErr(w, 409, err)
			return
		}
		writeJSON(w, 202, map[string]any{"ok": true, "capture_id": id})
	})
	mux.HandleFunc("POST /api/button", func(w http.ResponseWriter, r *http.Request) { // 물리 버튼 시뮬레이션
		id, err := s.cap.Start("button")
		if err != nil {
			writeErr(w, 409, err)
			return
		}
		writeJSON(w, 202, map[string]any{"ok": true, "capture_id": id})
	})
	mux.HandleFunc("GET /api/captures", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]any{"captures": s.cap.List()})
	})
	mux.HandleFunc("GET /api/captures/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := r.PathValue("id")
		if id == "latest" {
			if l := s.cap.Last(); l != nil {
				id = l.ID
			}
		}
		m, err := s.cap.Get(id)
		if err != nil {
			writeErr(w, 404, fmt.Errorf("촬영 없음: %s", id))
			return
		}
		writeJSON(w, 200, m)
	})
	mux.HandleFunc("GET /api/captures/{id}/{file}", func(w http.ResponseWriter, r *http.Request) {
		id, file := r.PathValue("id"), r.PathValue("file")
		if file == "bundle" {
			s.bundle(w, id)
			return
		}
		p, ok := s.cap.ImagePath(id, strings.TrimSuffix(file, ".jpg"))
		if !ok || !strings.HasSuffix(file, ".jpg") {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "image/jpeg")
		w.Header().Set("Cache-Control", "max-age=86400, immutable")
		http.ServeFile(w, r, p)
	})

	// 카메라 컨트롤 (초점·노출·화이트밸런스·화질)
	ctlReply := func(w http.ResponseWriter, ctls []CamControl, err error) {
		if err != nil {
			writeJSON(w, 409, map[string]any{"ok": false, "error": err.Error(), "controls": ctls})
			return
		}
		s.rtc.Broadcast(map[string]any{"type": "camera.controls", "ok": true, "controls": ctls})
		writeJSON(w, 200, map[string]any{"ok": true, "controls": ctls})
	}
	mux.HandleFunc("GET /api/camera/controls", func(w http.ResponseWriter, r *http.Request) {
		ctls, err := s.cam.Controls()
		if err != nil {
			writeJSON(w, 503, map[string]any{"ok": false, "error": err.Error(), "controls": ctls})
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true, "controls": ctls})
	})
	mux.HandleFunc("POST /api/camera/controls", func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			ID    uint32 `json:"id"`
			Value *int64 `json:"value"`
		}
		if err := readJSON(r, &req); err != nil || req.ID == 0 || req.Value == nil {
			writeErr(w, 400, fmt.Errorf(`{"id":<컨트롤 id>,"value":<값>} 이 필요합니다`))
			return
		}
		ctls, err := s.cam.SetControl(req.ID, *req.Value)
		ctlReply(w, ctls, err)
	})
	mux.HandleFunc("POST /api/camera/controls/reset", func(w http.ResponseWriter, r *http.Request) {
		ctls, err := s.cam.ResetControls()
		ctlReply(w, ctls, err)
	})

	// 조명·센서 (점검용)
	mux.HandleFunc("GET /api/climate", func(w http.ResponseWriter, r *http.Request) {
		c, err := s.readClimate()
		if err != nil {
			writeErr(w, 503, err)
			return
		}
		writeJSON(w, 200, c)
	})
	mux.HandleFunc("POST /api/light", func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			Mode LightMode `json:"mode"`
		}
		if err := readJSON(r, &req); err != nil {
			writeErr(w, 400, err)
			return
		}
		switch req.Mode {
		case LightOff, LightWhite, LightUV:
			s.lights.Set(req.Mode)
			writeJSON(w, 200, map[string]any{"ok": true, "mode": req.Mode})
		default:
			writeErr(w, 400, fmt.Errorf("mode 는 off|white|uv"))
		}
	})

	return cors(mux)
}

// bundle 은 온도/습도 + JPEG(일반광/UV광) 을 한 번에 돌려준다 (multipart/mixed).
//
//	part 1: application/json  (CaptureMeta)
//	part 2: image/jpeg  name="white"
//	part 3: image/jpeg  name="uv"   (CAPTURE_UV=1 일 때만)
func (s *Server) bundle(w http.ResponseWriter, id string) {
	if id == "latest" {
		if l := s.cap.Last(); l != nil {
			id = l.ID
		}
	}
	m, err := s.cap.Get(id)
	if err != nil {
		writeErr(w, 404, fmt.Errorf("촬영 없음: %s", id))
		return
	}
	mw := multipart.NewWriter(w)
	w.Header().Set("Content-Type", "multipart/mixed; boundary="+mw.Boundary())
	js, _ := json.Marshal(m)
	part, _ := mw.CreatePart(textproto.MIMEHeader{
		"Content-Type":        {"application/json; charset=utf-8"},
		"Content-Disposition": {`inline; name="meta"`},
	})
	part.Write(js)
	for _, kind := range []string{"white", "uv"} {
		p, _ := s.cap.ImagePath(id, kind)
		f, err := os.Open(p)
		if err != nil {
			continue
		}
		st, _ := f.Stat()
		hdr := textproto.MIMEHeader{
			"Content-Type":        {"image/jpeg"},
			"Content-Disposition": {fmt.Sprintf(`attachment; name="%s"; filename="%s_%s.jpg"`, kind, id, kind)},
		}
		if st != nil {
			hdr.Set("Content-Length", fmt.Sprint(st.Size()))
		}
		part, _ := mw.CreatePart(hdr)
		io.Copy(part, f)
		f.Close()
	}
	mw.Close()
}
