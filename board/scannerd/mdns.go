package main

import (
	"log"
	"net"
	"sort"
	"strings"
	"time"

	"github.com/grandcat/zeroconf"
)

// RunMDNS 는 _tgoabc._tcp 서비스를 광고한다 (자동 재연결: 앱이 같은 LAN 에서 장치를 찾는다).
// STA 주소가 바뀌면(공유기 접속/재접속) 다시 등록해서 새 주소를 알린다.
func RunMDNS(cfg Config, info DeviceInfo) {
	txt := []string{
		"id=" + info.ID, "name=" + info.Name, "model=" + info.Model, "fw=" + info.FW, "api=/api",
	}
	var srv *zeroconf.Server
	last := ""
	for {
		ifs, key := multicastIfaces()
		if key != last {
			if srv != nil {
				srv.Shutdown()
				srv = nil
			}
			if len(ifs) > 0 {
				s, err := zeroconf.Register(info.Name, cfg.MDNSService, "local.", cfg.HTTPPort, txt, ifs)
				if err != nil {
					log.Printf("[mdns] 등록 실패: %v", err)
				} else {
					srv = s
					log.Printf("[mdns] %s.%s.local 포트 %d 광고 (%s)", info.Name, cfg.MDNSService, cfg.HTTPPort, key)
				}
			}
			last = key
			if srv == nil {
				last = "" // 다음 주기에 다시 시도
			}
		}
		time.Sleep(10 * time.Second)
	}
}

// IPv4 주소가 있는 멀티캐스트 인터페이스 (lo, usb0 제외)와 그 주소 목록 키
func multicastIfaces() ([]net.Interface, string) {
	all, _ := net.Interfaces()
	var out []net.Interface
	var keys []string
	for _, ifc := range all {
		if ifc.Flags&net.FlagUp == 0 || ifc.Flags&net.FlagMulticast == 0 || ifc.Flags&net.FlagLoopback != 0 {
			continue
		}
		if ifc.Name == "usb0" {
			continue
		}
		addrs, _ := ifc.Addrs()
		for _, a := range addrs {
			if n, ok := a.(*net.IPNet); ok && n.IP.To4() != nil {
				out = append(out, ifc)
				keys = append(keys, ifc.Name+"="+n.IP.String())
				break
			}
		}
	}
	sort.Strings(keys)
	return out, strings.Join(keys, ",")
}
