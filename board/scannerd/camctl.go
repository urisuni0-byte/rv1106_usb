package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// 카메라 컨트롤 (초점, 노출, 화이트밸런스, 밝기 …). camhelper 가 V4L2 컨트롤을 그대로 넘겨 주고,
// 앱은 그 목록으로 화면을 만든다. 사용자가 바꾼 값은 저장해 두었다가 camhelper 가 새로 뜨거나
// 카메라가 다시 연결되면(USB 컨트롤은 그때 초기화된다) 다시 넣는다.

type CamMenuItem struct {
	V    int64  `json:"v"`
	Name string `json:"name"`
}

type CamControl struct {
	ID       uint32        `json:"id"`
	Name     string        `json:"name"`
	Type     string        `json:"type"` // int | bool | menu
	Min      int64         `json:"min"`
	Max      int64         `json:"max"`
	Step     int64         `json:"step"`
	Default  int64         `json:"default"`
	Value    int64         `json:"value"`
	Inactive bool          `json:"inactive"` // 자동 모드가 켜져 있어 지금은 못 바꾸는 값
	ReadOnly bool          `json:"readonly"`
	Menu     []CamMenuItem `json:"menu,omitempty"`
}

type ctlReply struct {
	Seq      int          `json:"seq"`
	OK       bool         `json:"ok"`
	Error    string       `json:"error"`
	Controls []CamControl `json:"controls"`
}

type savedCtl struct {
	ID    uint32 `json:"id"`
	Value int64  `json:"value"`
}

var savedMu sync.Mutex

func (c *Camera) savedPath() string { return filepath.Join(c.cfg.DataDir, "camera_controls.json") }

// loadSaved 는 저장된 설정을 바꾼 순서대로 돌려준다 (자동 끄기 -> 수동 값 순서가 지켜진다).
func (c *Camera) loadSaved() []savedCtl {
	savedMu.Lock()
	defer savedMu.Unlock()
	var list []savedCtl
	if b, err := os.ReadFile(c.savedPath()); err == nil {
		json.Unmarshal(b, &list)
	}
	return list
}

func (c *Camera) storeSaved(list []savedCtl) {
	savedMu.Lock()
	defer savedMu.Unlock()
	if len(list) == 0 {
		os.Remove(c.savedPath())
		return
	}
	b, _ := json.Marshal(list)
	if err := writeFileAtomic(c.savedPath(), b); err != nil {
		log.Printf("[camera] 컨트롤 저장 실패: %v", err)
	}
}

// applySaved 는 저장된 값을 camhelper 에 넣는다. 응답은 기다리지 않는다 (seq 0 은 버려진다).
func (c *Camera) applySaved(h *helper) {
	list := c.loadSaved()
	for _, s := range list {
		h.send(fmt.Sprintf("ctrl 0 %d %d", s.ID, s.Value))
	}
	if len(list) > 0 {
		log.Printf("[camera] 저장된 컨트롤 %d개 적용", len(list))
	}
}

// ctlRequest 는 camhelper 에 컨트롤 명령을 보내고 같은 seq 의 응답을 기다린다.
func (c *Camera) ctlRequest(format string, args ...any) (*ctlReply, error) {
	c.opMu.Lock()
	err := c.ensureHelper()
	c.mu.Lock()
	h, ch := c.h, c.ctls
	c.mu.Unlock()
	c.opMu.Unlock()
	if err != nil {
		return nil, err
	}
	if h == nil {
		return nil, errors.New("camhelper 없음")
	}

	c.ctlMu.Lock()
	defer c.ctlMu.Unlock()
	c.ctlSeq++
	seq := c.ctlSeq
	h.send(fmt.Sprintf(format, append([]any{seq}, args...)...))
	t := time.After(5 * time.Second)
	for {
		select {
		case b := <-ch:
			var r ctlReply
			if json.Unmarshal(b, &r) != nil || r.Seq != seq {
				continue // 다른 요청(저장값 적용 등)의 응답
			}
			return &r, nil
		case <-h.done:
			return nil, fmt.Errorf("camhelper 종료: %v", h.err)
		case <-t:
			return nil, errors.New("카메라 컨트롤 응답 시간 초과")
		}
	}
}

// Controls 는 카메라 컨트롤 목록과 현재 값.
func (c *Camera) Controls() ([]CamControl, error) {
	r, err := c.ctlRequest("ctrls %d")
	if err != nil {
		return nil, err
	}
	if !r.OK {
		return r.Controls, errors.New(r.Error)
	}
	return r.Controls, nil
}

// SetControl 은 값 하나를 바꾸고 저장한다. 바뀐 뒤의 전체 목록을 돌려준다
// (자동 노출을 끄면 노출 시간이 활성화되는 것처럼 다른 컨트롤 상태도 바뀐다).
func (c *Camera) SetControl(id uint32, v int64) ([]CamControl, error) {
	r, err := c.ctlRequest("ctrl %d %d %d", id, v)
	if err != nil {
		return nil, err
	}
	if !r.OK {
		return r.Controls, errors.New(r.Error)
	}
	list := c.loadSaved()
	out := list[:0]
	for _, s := range list {
		if s.ID != id {
			out = append(out, s)
		}
	}
	for _, ctl := range r.Controls {
		if ctl.ID == id && ctl.Value == ctl.Default && !isAutoCtl(ctl) {
			c.storeSaved(out) // 기본값으로 돌린 수동 값은 저장할 필요가 없다
			return r.Controls, nil
		}
	}
	c.storeSaved(append(out, savedCtl{ID: id, Value: v}))
	return r.Controls, nil
}

// isAutoCtl: 자동/수동 전환 컨트롤 (먼저 넣어야 수동 값이 먹는다)
func isAutoCtl(ctl CamControl) bool {
	switch ctl.ID {
	case 0x009a0901, // exposure_auto
		0x0098090c, // white_balance_temperature_auto
		0x009a090c, // focus_auto
		0x00980912: // hue_auto
		return true
	}
	return false
}

// ResetControls 는 모든 컨트롤을 기본값으로 돌리고 저장된 설정을 지운다.
func (c *Camera) ResetControls() ([]CamControl, error) {
	ctls, err := c.Controls()
	if err != nil {
		return ctls, err
	}
	c.storeSaved(nil)
	// 자동 모드를 먼저 기본값(대개 켜짐)으로 돌린 뒤 나머지. 자동이 켜져 비활성이 된 수동 값은
	// 카메라가 받지 않으므로 건너뛴다 (자동이 알아서 정한다).
	for pass := 0; pass < 2; pass++ {
		if pass == 1 {
			if ctls, err = c.Controls(); err != nil {
				return ctls, err
			}
		}
		for _, ctl := range ctls {
			if ctl.ReadOnly || ctl.Inactive || ctl.Value == ctl.Default || isAutoCtl(ctl) != (pass == 0) {
				continue
			}
			if r, err := c.ctlRequest("ctrl %d %d %d", ctl.ID, ctl.Default); err == nil && !r.OK {
				log.Printf("[camera] 기본값 복원 실패 %s: %s", ctl.Name, r.Error)
			}
		}
	}
	return c.Controls()
}
