// Package device finds boards with tiny-edge firmware on the computer's USB
// serial ports, by asking each one.
package device

import (
	"context"
	"errors"
	"fmt"
	"runtime"
	"sort"
	"strings"
	"sync"
	"time"

	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"

	"github.com/thin-edge/tiny-edge/tools/onboard/internal/improv"
)

// USB vendors of the USB-serial chips boards use.
var knownVendors = map[string]string{
	"303A": "Espressif USB",
	"1A86": "WCH USB-serial",
	"10C4": "Silicon Labs USB-serial",
	"0403": "FTDI USB-serial",
}

// Board is a device that answered as tiny-edge firmware.
type Board struct {
	Port   string
	Client *improv.Client
	Info   improv.TedgeInfo
	Device improv.DeviceInfo
}

// Candidates lists the serial ports that may have a board on them.
func Candidates() ([]string, error) {
	ports, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return nil, err
	}
	var names []string
	for _, p := range ports {
		if !p.IsUSB {
			continue
		}
		if _, ok := knownVendors[strings.ToUpper(p.VID)]; !ok {
			continue
		}
		// macOS lists each port twice; the "cu" one is for calling out.
		if runtime.GOOS == "darwin" && strings.HasPrefix(p.Name, "/dev/tty.") {
			continue
		}
		names = append(names, p.Name)
	}
	sort.Strings(names)
	return names, nil
}

// Open opens a port without resetting the board: DTR and RTS stay low from
// the start, which is what the auto-reset circuits of most boards need.
func Open(name string) (serial.Port, error) {
	return serial.Open(name, &serial.Mode{
		BaudRate:          115200,
		InitialStatusBits: &serial.ModemOutputBits{DTR: false, RTS: false},
	})
}

// ErrNotTedge means the port has a board, but its firmware does not answer
// tiny-edge's requests.
var ErrNotTedge = errors.New("the firmware does not support setting up this way")

// Probe asks the board on a port who it is. The returned board keeps the
// port open.
func Probe(ctx context.Context, name string) (*Board, error) {
	port, err := Open(name)
	if err != nil {
		return nil, err
	}
	c := improv.NewClient(port)
	info, err := wakeAndAsk(ctx, c)
	if err != nil {
		c.Close()
		if errors.Is(err, improv.ErrNoAnswer) || improv.IsDeviceError(err, improv.ErrUnknownRPC) {
			return nil, ErrNotTedge
		}
		return nil, err
	}
	b := &Board{Port: name, Client: c, Info: info}
	if di, err := c.DeviceInfo(ctx); err == nil {
		b.Device = di
	}
	return b, nil
}

func wakeAndAsk(ctx context.Context, c *improv.Client) (improv.TedgeInfo, error) {
	var last error
	for attempt := 0; attempt < 2; attempt++ {
		if err := c.Wake(); err != nil {
			return improv.TedgeInfo{}, err
		}
		time.Sleep(300 * time.Millisecond)
		info, err := c.Info(ctx, 2*time.Second)
		if err == nil {
			return info, nil
		}
		last = err
		if !errors.Is(err, improv.ErrNoAnswer) {
			break
		}
	}
	return improv.TedgeInfo{}, last
}

// Result of probing one port.
type Result struct {
	Port  string
	Board *Board
	Err   error
}

// ProbeAll asks every candidate port at once.
func ProbeAll(ctx context.Context, ports []string) []Result {
	results := make([]Result, len(ports))
	var wg sync.WaitGroup
	for i, p := range ports {
		wg.Add(1)
		go func(i int, p string) {
			defer wg.Done()
			b, err := Probe(ctx, p)
			results[i] = Result{Port: p, Board: b, Err: err}
		}(i, p)
	}
	wg.Wait()
	return results
}

// ChipName turns "esp32s3" into "ESP32-S3".
func ChipName(chip string) string {
	c := strings.ToUpper(chip)
	if strings.HasPrefix(c, "ESP32") && len(c) > 5 && c[5] != '-' {
		return "ESP32-" + c[5:]
	}
	if c == "" || c == "UNKNOWN" {
		return "Board"
	}
	return c
}

// Describe says what a board is, in plain words.
func (b *Board) Describe() string {
	what := ChipName(b.Device.Chip)
	// Firmware without a name or version of its own reports the RTOS's
	// defaults ("zephyr", "0.0.0"), which tell the user nothing; the
	// board's ID does.
	if b.Device.Firmware != "" && b.Device.Version != "" && b.Device.Version != "0.0.0" {
		what += fmt.Sprintf(" running %s %s", b.Device.Firmware, b.Device.Version)
	} else if b.Info.DeviceID != "" {
		what += " " + b.Info.DeviceID
	}
	return what + ", " + b.Status()
}

// Status says where the board stands with Cumulocity.
func (b *Board) Status() string {
	switch b.Info.State {
	case "connected":
		return "connected to " + b.Info.Tenant
	case "awaiting-registration", "connecting":
		if b.Info.Tenant != "" {
			return "not set up yet (waiting for " + b.Info.Tenant + ")"
		}
		return "not set up yet"
	case "waiting-network":
		return "not on Wi-Fi yet"
	default:
		return "not set up yet"
	}
}
