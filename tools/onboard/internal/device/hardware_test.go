//go:build hardware

package device

import (
	"context"
	"os"
	"testing"
	"time"

	"github.com/thin-edge/tiny-edge/tools/onboard/internal/improv"
)

// A board with the onboarding firmware, on the port in TEDGE_ONBOARD_PORT
// (or found by itself):
//
//	go test -tags hardware ./internal/device -run Hardware -v
func TestHardwareProbe(t *testing.T) {
	ctx := context.Background()
	ports := []string{os.Getenv("TEDGE_ONBOARD_PORT")}
	if ports[0] == "" {
		var err error
		if ports, err = Candidates(); err != nil || len(ports) == 0 {
			t.Skipf("no board: %v", err)
		}
	}
	for _, r := range ProbeAll(ctx, ports) {
		if r.Err != nil {
			t.Fatalf("%s: %v", r.Port, r.Err)
		}
		b := r.Board
		t.Logf("%s: %s", b.Port, b.Describe())
		t.Logf("info: %+v", b.Info)
		t.Logf("device: %+v", b.Device)
		if b.Info.DeviceID == "" || b.Info.ProtocolVersion != 1 {
			t.Fatalf("unexpected info %+v", b.Info)
		}
		start := time.Now()
		if err := b.Client.Identify(ctx); err != nil {
			t.Fatalf("identify: %v", err)
		}
		t.Logf("identify answered in %v", time.Since(start))
		for _, l := range b.Client.RecentLog() {
			t.Logf("log: %s", l)
		}
		b.Client.Close()
	}
}

// A network that does not exist: the board reports provisioning, then that
// it could not connect, and is ready again. Slow (the board tries for up
// to 30 s), so only with TEDGE_ONBOARD_WIFI_TEST=1.
func TestHardwareWiFiFailure(t *testing.T) {
	if os.Getenv("TEDGE_ONBOARD_WIFI_TEST") == "" {
		t.Skip("set TEDGE_ONBOARD_WIFI_TEST=1")
	}
	ports, err := Candidates()
	if err != nil || len(ports) == 0 {
		t.Skipf("no board: %v", err)
	}
	b, err := Probe(context.Background(), ports[0])
	if err != nil {
		t.Fatal(err)
	}
	defer b.Client.Close()
	var states []byte
	start := time.Now()
	err = b.Client.SetWiFi(context.Background(), "tedge-onboard-no-such-net", "wrong-password",
		func(s byte) { states = append(states, s) })
	t.Logf("after %v: err=%v states=%v", time.Since(start), err, states)
	if !improv.IsDeviceError(err, improv.ErrUnableConnect) {
		t.Fatalf("got %v", err)
	}
	for _, l := range b.Client.RecentLog() {
		t.Logf("log: %s", l)
	}
}
