// Package wifi reads the computer's Wi-Fi networks and, with the user's
// approval where the system asks for it, their passwords: the macOS
// Keychain, NetworkManager on Linux, and the WLAN profiles on Windows.
package wifi

import (
	"bufio"
	"errors"
	"os/exec"
	"strings"
)

// Runner runs a command and returns its standard output.
type Runner func(name string, args ...string) (string, error)

// Exec runs real commands.
func Exec(name string, args ...string) (string, error) {
	out, err := exec.Command(name, args...).Output()
	return string(out), err
}

// Detector reads the networks of one operating system.
type Detector struct {
	GOOS string // runtime.GOOS
	Run  Runner
}

// ErrNoPassword means the system did not give the password (the user
// declined, or it is not stored).
var ErrNoPassword = errors.New("the password is not available")

// Current returns the network the computer is connected to, if the system
// says.
func (d Detector) Current() (string, bool) {
	switch d.GOOS {
	case "darwin":
		dev := d.macWiFiDevice()
		out, err := d.Run("ipconfig", "getsummary", dev)
		if err == nil {
			if ssid := macSummarySSID(out); ssid != "" {
				return ssid, true
			}
		}
	case "linux":
		out, err := d.Run("nmcli", "-t", "-f", "ACTIVE,SSID", "dev", "wifi")
		if err == nil {
			for _, l := range lines(out) {
				if f := nmcliFields(l); len(f) == 2 && f[0] == "yes" && f[1] != "" {
					return f[1], true
				}
			}
		}
	case "windows":
		out, err := d.Run("netsh", "wlan", "show", "interfaces")
		if err == nil {
			if ssid := netshValue(out, "SSID"); ssid != "" {
				return ssid, true
			}
		}
	}
	return "", false
}

// Known returns the networks the computer has joined before.
func (d Detector) Known() []string {
	var names []string
	switch d.GOOS {
	case "darwin":
		out, err := d.Run("networksetup", "-listpreferredwirelessnetworks", d.macWiFiDevice())
		if err == nil {
			for i, l := range lines(out) {
				if i == 0 && strings.HasPrefix(l, "Preferred networks") {
					continue
				}
				if n := strings.TrimSpace(l); n != "" {
					names = append(names, n)
				}
			}
		}
	case "linux":
		out, err := d.Run("nmcli", "-t", "-f", "NAME,TYPE", "connection", "show")
		if err == nil {
			for _, l := range lines(out) {
				if f := nmcliFields(l); len(f) == 2 && f[1] == "802-11-wireless" {
					names = append(names, f[0])
				}
			}
		}
	case "windows":
		out, err := d.Run("netsh", "wlan", "show", "profiles")
		if err == nil {
			for _, l := range lines(out) {
				if k, v, ok := strings.Cut(l, ":"); ok &&
					strings.Contains(k, "All User Profile") && strings.TrimSpace(v) != "" {
					names = append(names, strings.TrimSpace(v))
				}
			}
		}
	}
	return names
}

// Password returns a network's stored password. On macOS the Keychain asks
// the user to allow it.
func (d Detector) Password(ssid string) (string, error) {
	var out string
	var err error
	switch d.GOOS {
	case "darwin":
		out, err = d.Run("security", "find-generic-password", "-D",
			"AirPort network password", "-a", ssid, "-w")
		out = strings.TrimRight(out, "\r\n")
	case "linux":
		out, err = d.Run("nmcli", "-s", "-g", "802-11-wireless-security.psk",
			"connection", "show", ssid)
		out = strings.TrimRight(out, "\r\n")
	case "windows":
		out, err = d.Run("netsh", "wlan", "show", "profile", "name="+ssid, "key=clear")
		if err == nil {
			out = netshValue(out, "Key Content")
		}
	default:
		return "", ErrNoPassword
	}
	if err != nil || out == "" {
		return "", ErrNoPassword
	}
	return out, nil
}

// macWiFiDevice finds the Wi-Fi interface ("en0" on most Macs).
func (d Detector) macWiFiDevice() string {
	out, err := d.Run("networksetup", "-listallhardwareports")
	if err != nil {
		return "en0"
	}
	wifi := false
	for _, l := range lines(out) {
		if strings.HasPrefix(l, "Hardware Port:") {
			p := strings.TrimSpace(strings.TrimPrefix(l, "Hardware Port:"))
			wifi = p == "Wi-Fi" || p == "AirPort"
		} else if wifi && strings.HasPrefix(l, "Device:") {
			return strings.TrimSpace(strings.TrimPrefix(l, "Device:"))
		}
	}
	return "en0"
}

// macSummarySSID reads "SSID : name" from ipconfig getsummary. Recent macOS
// releases show "<redacted>" to programs without location access.
func macSummarySSID(out string) string {
	for _, l := range lines(out) {
		k, v, ok := strings.Cut(l, " : ")
		if ok && strings.TrimSpace(k) == "SSID" {
			v = strings.TrimSpace(v)
			if v != "" && v != "<redacted>" {
				return v
			}
		}
	}
	return ""
}

// netshValue reads "    Key    : value" from netsh output, matching the key
// exactly (so "SSID" does not match "BSSID").
func netshValue(out, key string) string {
	for _, l := range lines(out) {
		k, v, ok := strings.Cut(l, ":")
		if ok && strings.TrimSpace(k) == key {
			return strings.TrimSpace(v)
		}
	}
	return ""
}

// nmcliFields splits nmcli's terse output, where ':' in a value is "\:".
func nmcliFields(line string) []string {
	var fields []string
	var cur strings.Builder
	for i := 0; i < len(line); i++ {
		switch {
		case line[i] == '\\' && i+1 < len(line):
			i++
			cur.WriteByte(line[i])
		case line[i] == ':':
			fields = append(fields, cur.String())
			cur.Reset()
		default:
			cur.WriteByte(line[i])
		}
	}
	return append(fields, cur.String())
}

func lines(s string) []string {
	var out []string
	sc := bufio.NewScanner(strings.NewReader(s))
	for sc.Scan() {
		out = append(out, strings.TrimRight(sc.Text(), "\r"))
	}
	return out
}
