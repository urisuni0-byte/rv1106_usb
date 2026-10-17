// fakecam - camhelper 대신 쓰는 시험용 카메라 (카메라가 없는 PC/안드로이드 adb shell 용)
//
// camhelper run 과 같은 stdin 명령(preview / photo / photo nopreview / snap / idle / idr / quit)을 받고
// 같은 레코드(I/H/J/E/C)를 낸다. 컨트롤(ctrls / ctrl)은 흔한 UVC 컨트롤 몇 개를 흉내 낸다.
// focus on|off 면 렌즈를 천천히 돌리는 것처럼 오르내리는 선명도('F')를 낸다. FAKECAM_DIR 에 미리 만든 파일을 둔다:
//   preview.h264  H.264 Baseline Annex-B, AUD 포함 (ffmpeg ... -x264-params aud=1 -f h264)
//   white.jpg, uv.jpg
//
//	cd tools/fakecam && go build -o fakecam main.go   (scanner.conf 의 CAMHELPER 로 지정)
package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math"
	"math/rand"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

type ctl struct {
	ID       uint32 `json:"id"`
	Name     string `json:"name"`
	Type     string `json:"type"`
	Min      int    `json:"min"`
	Max      int    `json:"max"`
	Step     int    `json:"step"`
	Default  int    `json:"default"`
	Value    int    `json:"value"`
	Inactive bool   `json:"inactive"`
	ReadOnly bool   `json:"readonly"`
	Menu     []any  `json:"menu,omitempty"`
}

var ctls = []*ctl{
	{ID: 0x00980900, Name: "Brightness", Type: "int", Min: -64, Max: 64, Step: 1},
	{ID: 0x00980901, Name: "Contrast", Type: "int", Min: 0, Max: 95, Step: 1, Default: 32, Value: 32},
	{ID: 0x00980902, Name: "Saturation", Type: "int", Min: 0, Max: 100, Step: 1, Default: 64, Value: 64},
	{ID: 0x0098091b, Name: "Sharpness", Type: "int", Min: 0, Max: 7, Step: 1, Default: 2, Value: 2},
	{ID: 0x0098090c, Name: "White Balance Temperature, Auto", Type: "bool", Max: 1, Step: 1, Default: 1, Value: 1},
	{ID: 0x0098091a, Name: "White Balance Temperature", Type: "int", Min: 2800, Max: 6500, Step: 1, Default: 4600, Value: 4600},
	{ID: 0x00980918, Name: "Power Line Frequency", Type: "menu", Max: 2, Step: 1, Default: 1, Value: 1,
		Menu: []any{map[string]any{"v": 0, "name": "Disabled"}, map[string]any{"v": 1, "name": "50 Hz"}, map[string]any{"v": 2, "name": "60 Hz"}}},
	{ID: 0x009a0901, Name: "Exposure, Auto", Type: "menu", Min: 1, Max: 3, Step: 1, Default: 3, Value: 3,
		Menu: []any{map[string]any{"v": 1, "name": "Manual Mode"}, map[string]any{"v": 3, "name": "Aperture Priority Mode"}}},
	{ID: 0x009a0902, Name: "Exposure (Absolute)", Type: "int", Min: 3, Max: 2047, Step: 1, Default: 166, Value: 166},
	{ID: 0x009a090a, Name: "Focus (absolute)", Type: "int", Min: 0, Max: 1023, Step: 1, Default: 68, Value: 68},
	{ID: 0x009a090c, Name: "Focus, Auto", Type: "bool", Max: 1, Step: 1, Default: 1, Value: 1},
}

func emitCtls(seq int, errText string) {
	for _, c := range ctls { // 자동이 켜져 있으면 수동 값은 비활성
		switch c.ID {
		case 0x0098091a:
			c.Inactive = ctls[4].Value == 1
		case 0x009a0902:
			c.Inactive = ctls[7].Value != 1
		case 0x009a090a:
			c.Inactive = ctls[10].Value == 1
		}
	}
	b, _ := json.Marshal(map[string]any{"seq": seq, "ok": errText == "", "error": errText, "controls": ctls})
	emit('C', b)
}

var (
	out = bufio.NewWriterSize(os.Stdout, 1<<20)
	t0  = time.Now()
)

func emit(t byte, data []byte) {
	var h [13]byte
	h[0] = t
	binary.BigEndian.PutUint32(h[1:], uint32(len(data)))
	binary.BigEndian.PutUint64(h[5:], uint64(time.Since(t0).Microseconds()))
	out.Write(h[:])
	out.Write(data)
	if out.Flush() != nil {
		os.Exit(0)
	}
}

func arg(name, def string) string {
	for i := 2; i+1 < len(os.Args); i++ {
		if os.Args[i] == name {
			return os.Args[i+1]
		}
	}
	return def
}

func main() {
	if len(os.Args) < 2 || os.Args[1] != "run" {
		fmt.Println("fakecam run")
		return
	}
	dir := os.Getenv("FAKECAM_DIR")
	if dir == "" {
		dir = filepath.Dir(os.Args[0])
	}
	data, err := os.ReadFile(filepath.Join(dir, "preview.h264"))
	if err != nil {
		emit('E', []byte(err.Error()))
		os.Exit(1)
	}
	aud := []byte{0, 0, 0, 1, 9}
	var aus [][]byte
	for len(data) > 0 {
		i := bytes.Index(data[1:], aud)
		if i < 0 {
			aus = append(aus, data)
			break
		}
		aus = append(aus, data[:i+1])
		data = data[i+1:]
	}
	fps, _ := strconv.Atoi(arg("-f", "20"))
	pw, ph := arg("-W", "640"), arg("-H", "480")
	xw, xh := arg("-X", "2592"), arg("-Y", "1944")

	lines := make(chan string, 8)
	go func() {
		sc := bufio.NewScanner(os.Stdin)
		for sc.Scan() {
			lines <- sc.Text()
		}
		close(lines)
	}()

	mode, preview, n, shots := "idle", false, 0, 0
	info := func() {
		w, h, f := pw, ph, fps
		if mode == "photo" {
			w, h, f = xw, xh, 10
		}
		emit('I', []byte(fmt.Sprintf(`{"mode":%q,"device":"fake","width":%s,"height":%s,"fps":%d,`+
			`"camera_format":"FAKE","preview":%t,"preview_width":%s,"preview_height":%s,"kbps":2000}`,
			mode, w, h, f, preview, pw, ph)))
	}
	info()
	tick := time.NewTicker(time.Second / time.Duration(max(fps, 1)))
	slow := 0
	focus, lastF := false, time.Now()
	for {
		select {
		case l, ok := <-lines:
			if !ok || l == "quit" {
				return
			}
			if f := strings.Fields(l); len(f) > 0 && (f[0] == "ctrls" || f[0] == "ctrl") {
				seq, _ := strconv.Atoi(f[1])
				if f[0] == "ctrl" && len(f) == 4 {
					id, _ := strconv.ParseUint(f[2], 10, 32)
					v, _ := strconv.Atoi(f[3])
					found := false
					for _, c := range ctls {
						if c.ID == uint32(id) {
							c.Value, found = max(c.Min, min(c.Max, v)), true
						}
					}
					if !found {
						emitCtls(seq, "설정 실패: 없는 컨트롤")
						continue
					}
				}
				emitCtls(seq, "")
				continue
			}
			switch l {
			case "focus on", "focus off":
				focus = l == "focus on"
			case "preview", "photo", "photo nopreview", "idle":
				time.Sleep(300 * time.Millisecond) // 카메라 모드 전환 흉내
				mode, preview = l, l != "idle" && l != "photo nopreview"
				if l != "idle" && l != "preview" {
					mode = "photo"
				}
				n = 0 // 새 스트림은 키프레임부터
				info()
			case "snap":
				name := "white.jpg"
				if shots%2 == 1 {
					name = "uv.jpg"
				}
				shots++
				b, err := os.ReadFile(filepath.Join(dir, name))
				if err != nil {
					emit('E', []byte(err.Error()))
					continue
				}
				time.Sleep(400 * time.Millisecond)
				emit('J', b)
			}
		case <-tick.C:
			if !preview {
				continue
			}
			if mode == "photo" { // 사진 모드 미리보기는 약 5fps
				if slow++; slow%4 != 0 {
					continue
				}
			}
			emit('H', aus[n%len(aus)])
			n++
			if focus && time.Since(lastF) >= 200*time.Millisecond {
				lastF = time.Now()
				// 10초 주기로 초점이 맞았다 벗어나는 흉내 (가우스 봉우리 + 잡음)
				d := math.Mod(time.Since(t0).Seconds(), 10) - 5
				score := 40 + 900*math.Exp(-d*d/2) + rand.Float64()*20
				emit('F', []byte(fmt.Sprintf(`{"score":%.1f,"brightness":120,"frames":%d,"source":%q,"roi":0.5}`,
					score, max(fps/5, 1), map[bool]string{true: "photo", false: "preview"}[mode == "photo"])))
			}
		}
	}
}
