package main

import (
	"testing"

	"github.com/thin-edge/tiny-edge/tools/onboard/internal/improv"
)

func TestWiFiOptionsHiddenCurrent(t *testing.T) {
	// macOS hides the current network: the strongest one the computer
	// knows comes first.
	scanned := []improv.Network{
		{SSID: "neighbour", RSSI: -45, Secured: true},
		{SSID: "home", RSSI: -58, Secured: true},
		{SSID: "cafe", RSSI: -80, Secured: false},
	}
	opts := wifiOptions(scanned, true, "", []string{"office", "home"})
	if opts[0].Value != "home" || opts[0].Label != "home  (strong signal, known to this computer)" {
		t.Fatalf("first: %+v", opts[0])
	}
	if len(opts) != 3 || opts[2].Label != "cafe  (weak signal, open)" {
		t.Fatalf("opts: %+v", opts)
	}
}

func TestWiFiOptionsNamedCurrent(t *testing.T) {
	scanned := []improv.Network{{SSID: "a", RSSI: -40, Secured: true}, {SSID: "home", RSSI: -66, Secured: true}}
	opts := wifiOptions(scanned, true, "home", []string{"home"})
	if opts[0].Value != "home" || opts[0].Label != "home  (good signal, this computer's network)" {
		t.Fatalf("first: %+v", opts[0])
	}
}

func TestWiFiOptionsCurrentOutOfReach(t *testing.T) {
	scanned := []improv.Network{{SSID: "a", RSSI: -40, Secured: true}}
	opts := wifiOptions(scanned, true, "home-5g", nil)
	if opts[len(opts)-1].Value != "home-5g" {
		t.Fatalf("opts: %+v", opts)
	}
}

func TestWiFiOptionsWithoutScan(t *testing.T) {
	opts := wifiOptions(nil, false, "home", []string{"home", "office"})
	if len(opts) != 2 || opts[0].Value != "home" || opts[1].Value != "office" {
		t.Fatalf("opts: %+v", opts)
	}
}
