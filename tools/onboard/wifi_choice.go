package main

import (
	"fmt"

	"github.com/thin-edge/tiny-edge/tools/onboard/internal/improv"
	"github.com/thin-edge/tiny-edge/tools/onboard/internal/ui"
)

// Values of the Wi-Fi list's special entries (an SSID cannot start with NUL).
const (
	choiceOther = "\x00other"
	choiceKeep  = "\x00keep"
)

// wifiOptions builds the Wi-Fi list, the most likely network first.
//
// With the board's scan: the networks it can see, strongest first, marking
// the computer's current network (when the system names it) and the
// networks the computer knows. When the system hides the current network,
// the strongest network the board sees that the computer knows comes first:
// that is almost always the one the computer is on. Without a scan: the
// computer's current and known networks.
func wifiOptions(scanned []improv.Network, scanOK bool, current string, known []string) []ui.Option {
	isKnown := map[string]bool{}
	for _, k := range known {
		isKnown[k] = true
	}
	var opts []ui.Option
	if !scanOK {
		if current != "" {
			opts = append(opts, ui.Option{Label: current + "  (this computer's network)", Value: current})
		}
		for _, k := range known {
			if k != current {
				opts = append(opts, ui.Option{Label: k, Value: k})
			}
		}
		return opts
	}

	first := -1
	for i, n := range scanned {
		if n.SSID == current && current != "" {
			first = i
			break
		}
	}
	if first < 0 {
		for i, n := range scanned {
			if isKnown[n.SSID] {
				first = i
				break
			}
		}
	}
	seenCurrent := false
	for i, n := range scanned {
		label := fmt.Sprintf("%s  (%s", n.SSID, signalWords(n.RSSI))
		switch {
		case n.SSID == current && current != "":
			label += ", this computer's network"
			seenCurrent = true
		case isKnown[n.SSID]:
			label += ", known to this computer"
		}
		if !n.Secured {
			label += ", open"
		}
		label += ")"
		opt := ui.Option{Label: label, Value: n.SSID}
		if i == first {
			opts = append([]ui.Option{opt}, opts...)
		} else {
			opts = append(opts, opt)
		}
	}
	if current != "" && !seenCurrent {
		opts = append(opts, ui.Option{
			Label: current + "  (this computer's network, but the board cannot see it: it may be 5 GHz or out of range)",
			Value: current,
		})
	}
	return opts
}

// signalWords describes a signal strength in dBm.
func signalWords(rssi int) string {
	switch {
	case rssi >= -60:
		return "strong signal"
	case rssi >= -72:
		return "good signal"
	default:
		return "weak signal"
	}
}

// openNetwork reports whether the board saw ssid as a network without a
// password.
func openNetwork(scanned []improv.Network, ssid string) bool {
	for _, n := range scanned {
		if n.SSID == ssid {
			return !n.Secured
		}
	}
	return false
}
