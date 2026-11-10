package wifi

import (
	"errors"
	"strings"
	"testing"
)

// fake answers commands from a table keyed by the command line.
func fake(answers map[string]string) Runner {
	return func(name string, args ...string) (string, error) {
		key := strings.Join(append([]string{name}, args...), " ")
		if out, ok := answers[key]; ok {
			return out, nil
		}
		return "", errors.New("exit status 1")
	}
}

const macPorts = "\nHardware Port: Ethernet\nDevice: en1\n\nHardware Port: Wi-Fi\nDevice: en0\nEthernet Address: aa\n"

func TestMac(t *testing.T) {
	d := Detector{GOOS: "darwin", Run: fake(map[string]string{
		"networksetup -listallhardwareports":                                        macPorts,
		"ipconfig getsummary en0":                                                   "<dictionary> {\n  BSSID : aa:bb\n  SSID : home-net\n}\n",
		"networksetup -listpreferredwirelessnetworks en0":                           "Preferred networks on en0:\n\thome-net\n\tcafe\n",
		"security find-generic-password -D AirPort network password -a home-net -w": "s3cret\n",
	})}
	if s, ok := d.Current(); !ok || s != "home-net" {
		t.Fatalf("current %q %v", s, ok)
	}
	if k := d.Known(); len(k) != 2 || k[1] != "cafe" {
		t.Fatalf("known %q", k)
	}
	if p, err := d.Password("home-net"); err != nil || p != "s3cret" {
		t.Fatalf("password %q %v", p, err)
	}
	if _, err := d.Password("cafe"); err != ErrNoPassword {
		t.Fatalf("got %v", err)
	}
}

func TestMacRedacted(t *testing.T) {
	d := Detector{GOOS: "darwin", Run: fake(map[string]string{
		"networksetup -listallhardwareports": macPorts,
		"ipconfig getsummary en0":            "  SSID : <redacted>\n",
	})}
	if _, ok := d.Current(); ok {
		t.Fatal("a redacted SSID was reported")
	}
}

func TestLinux(t *testing.T) {
	d := Detector{GOOS: "linux", Run: fake(map[string]string{
		"nmcli -t -f ACTIVE,SSID dev wifi":                                "no:other\nyes:lab\\:2g\n",
		"nmcli -t -f NAME,TYPE connection show":                           "lab\\:2g:802-11-wireless\nWired:802-3-ethernet\n",
		"nmcli -s -g 802-11-wireless-security.psk connection show lab:2g": "pw\n",
	})}
	if s, ok := d.Current(); !ok || s != "lab:2g" {
		t.Fatalf("current %q", s)
	}
	if k := d.Known(); len(k) != 1 {
		t.Fatalf("known %q", k)
	}
}

func TestWindows(t *testing.T) {
	d := Detector{GOOS: "windows", Run: fake(map[string]string{
		"netsh wlan show interfaces":                    "    Name                   : Wi-Fi\r\n    BSSID                  : aa:bb\r\n    SSID                   : office\r\n",
		"netsh wlan show profiles":                      "User profiles\r\n    All User Profile     : office\r\n    All User Profile     : home\r\n",
		"netsh wlan show profile name=office key=clear": "    Key Content            : pw123\r\n",
	})}
	if s, ok := d.Current(); !ok || s != "office" {
		t.Fatalf("current %q", s)
	}
	if k := d.Known(); len(k) != 2 {
		t.Fatalf("known %q", k)
	}
	if p, _ := d.Password("office"); p != "pw123" {
		t.Fatalf("password %q", p)
	}
}
