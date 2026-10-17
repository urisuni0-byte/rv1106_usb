package main

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

// 프로비저닝 상태 (BLE Provision Status 의 state)
const (
	ProvIdle       = "idle"       // 저장된 Wi-Fi 없음
	ProvConnecting = "connecting" // 접속 시도 중
	ProvConnected  = "connected"  // 접속됨 - ip/port 로 HTTP 접근 가능
	ProvFailed     = "failed"
)

type ProvStatus struct {
	State string `json:"state"`
	SSID  string `json:"ssid,omitempty"`
	IP    string `json:"ip,omitempty"`
	Port  int    `json:"port"`
	Name  string `json:"name"`
	ID    string `json:"id"`
	Error string `json:"error,omitempty"`
}

// Provisioner 는 Wi-Fi(STA) 설정을 저장하고 붙인다. 실제 접속은 보드의 rvcam-net.sh 가 한다
// (AP 를 항상 유지하면서 STA 를 붙이는 스크립트, /userdata/rvcam/sta.ssid|sta.psk 를 읽는다).
type Provisioner struct {
	cfg  Config
	info DeviceInfo

	mu        sync.Mutex
	st        ProvStatus
	listeners []func(ProvStatus)
	cancel    context.CancelFunc // 진행 중인 접속 시도
	gen       int
}

func NewProvisioner(cfg Config, info DeviceInfo) *Provisioner {
	p := &Provisioner{cfg: cfg, info: info}
	p.st = p.base(ProvIdle)
	p.Refresh()
	return p
}

func (p *Provisioner) base(state string) ProvStatus {
	return ProvStatus{State: state, Port: p.info.Port, Name: p.info.Name, ID: p.info.ID}
}

func (p *Provisioner) Status() ProvStatus {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.st
}

func (p *Provisioner) OnChange(f func(ProvStatus)) {
	p.mu.Lock()
	p.listeners = append(p.listeners, f)
	p.mu.Unlock()
}

func (p *Provisioner) set(st ProvStatus) {
	p.mu.Lock()
	changed := st != p.st
	p.st = st
	ls := append([]func(ProvStatus){}, p.listeners...)
	p.mu.Unlock()
	if changed {
		log.Printf("[wifi] %s ssid=%q ip=%s %s", st.State, st.SSID, st.IP, st.Error)
		for _, f := range ls {
			f(st)
		}
	}
}

func (p *Provisioner) readCfg(name string) string {
	b, _ := os.ReadFile(filepath.Join(p.cfg.NetCfgDir, name))
	return strings.TrimRight(string(b), "\r\n")
}

func (p *Provisioner) writeCfg(name, v string) error {
	if err := os.MkdirAll(p.cfg.NetCfgDir, 0700); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(p.cfg.NetCfgDir, name), []byte(v), 0600)
}

// Refresh 는 현재 STA 상태를 읽어 상태를 맞춘다 (접속 시도 중이 아닐 때만).
func (p *Provisioner) Refresh() {
	p.mu.Lock()
	busy := p.cancel != nil
	p.mu.Unlock()
	if busy {
		return
	}
	ssid := p.readCfg("sta.ssid")
	ip := ifaceIPv4(p.cfg.StaIf)
	st := p.base(ProvIdle)
	st.SSID = ssid
	switch {
	case ssid != "" && ip != "":
		st.State, st.IP = ProvConnected, ip
	case ssid != "":
		// 저장은 돼 있는데 아직(또는 더 이상) 붙지 않음 - 스크립트가 백그라운드로 붙는 중일 수 있다
		st.State = ProvConnecting
		if prev := p.Status(); prev.State == ProvFailed && prev.SSID == ssid {
			st = prev
		}
	}
	p.set(st)
}

func validateWiFi(ssid, psk string) error {
	if ssid == "" || len(ssid) > 32 {
		return errors.New("SSID 는 1~32 바이트여야 합니다")
	}
	if !utf8.ValidString(ssid) || strings.ContainsAny(ssid, "\r\n\x00") {
		return errors.New("SSID 에 쓸 수 없는 문자가 있습니다")
	}
	if psk != "" && (len(psk) < 8 || len(psk) > 63) {
		return errors.New("비밀번호는 8~63자여야 합니다 (개방형은 빈 값)")
	}
	if strings.ContainsAny(psk, "\r\n\x00") {
		return errors.New("비밀번호에 쓸 수 없는 문자가 있습니다")
	}
	return nil
}

// Apply 는 Wi-Fi 정보를 저장하고 백그라운드로 접속한다. 결과는 OnChange 로 알린다.
func (p *Provisioner) Apply(ssid, psk string) error {
	if err := validateWiFi(ssid, psk); err != nil {
		st := p.base(ProvFailed)
		st.SSID, st.Error = ssid, err.Error()
		p.set(st)
		return err
	}
	if _, err := os.Stat(p.cfg.NetScript); err != nil {
		return errors.New("네트워크 스크립트가 없습니다: " + p.cfg.NetScript)
	}
	if err := p.writeCfg("sta.ssid", ssid); err != nil {
		return err
	}
	if err := p.writeCfg("sta.psk", psk); err != nil {
		return err
	}

	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	p.mu.Lock()
	if p.cancel != nil {
		p.cancel()
	}
	p.cancel = cancel
	p.gen++
	gen := p.gen
	p.mu.Unlock()

	st := p.base(ProvConnecting)
	st.SSID = ssid
	p.set(st)

	go func() {
		defer func() {
			cancel()
			p.mu.Lock()
			if p.gen == gen {
				p.cancel = nil
			}
			p.mu.Unlock()
		}()
		out, err := exec.CommandContext(ctx, p.cfg.NetScript, "sta-restart").CombinedOutput()
		if ctx.Err() == context.Canceled {
			return // 새 요청이 들어와 대체됨
		}
		ip := p.staAddr()
		res := p.base(ProvConnected)
		res.SSID = ssid
		if ip != "" {
			res.IP = ip
		} else {
			res.State = ProvFailed
			res.Error = failureReason(out, err)
		}
		p.set(res)
	}()
	return nil
}

// staAddr 는 rvcam-net.sh status 의 sta_addr (없으면 인터페이스에서 직접 읽는다).
func (p *Provisioner) staAddr() string {
	out, err := exec.Command(p.cfg.NetScript, "status").Output()
	if err == nil {
		sc := bufio.NewScanner(bytes.NewReader(out))
		for sc.Scan() {
			if v, ok := strings.CutPrefix(sc.Text(), "sta_addr="); ok && v != "" {
				return v
			}
		}
	}
	return ifaceIPv4(p.cfg.StaIf)
}

func failureReason(out []byte, err error) string {
	s := string(out)
	switch {
	case strings.Contains(s, "접속 실패"):
		return "공유기에 접속하지 못했습니다 (SSID/비밀번호 확인)"
	case strings.Contains(s, "DHCP"):
		return "공유기에 붙었지만 IP 주소를 받지 못했습니다"
	case err != nil:
		return "Wi-Fi 접속 실패: " + err.Error()
	}
	return "Wi-Fi 접속 실패"
}
