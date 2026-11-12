package improv

import (
	"context"
	"errors"
	"fmt"
	"io"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

// DeviceError is an error the device reported.
type DeviceError struct{ Code byte }

func (e *DeviceError) Error() string {
	switch e.Code {
	case ErrInvalidRPC:
		return "the device did not understand the request"
	case ErrUnknownRPC:
		return "the device does not support this request"
	case ErrUnableConnect:
		return "the device could not connect to the Wi-Fi network"
	case ErrNotAuthorized:
		return "the device refused the request"
	default:
		return fmt.Sprintf("the device reported error %d", e.Code)
	}
}

// IsDeviceError reports whether err is the device error code.
func IsDeviceError(err error, code byte) bool {
	var de *DeviceError
	return errors.As(err, &de) && de.Code == code
}

// ErrNoAnswer means the device sent no Improv answer in time.
var ErrNoAnswer = errors.New("the device did not answer")

// Client talks Improv to one device over a serial line.
type Client struct {
	rw     io.ReadWriteCloser
	frames chan Frame

	mu    sync.Mutex
	lines []string // the device's recent log lines
	err   error    // why reading stopped
}

const keepLines = 40

// NewClient starts reading from rw.
func NewClient(rw io.ReadWriteCloser) *Client {
	c := &Client{rw: rw, frames: make(chan Frame, 16)}
	go c.read()
	return c
}

func (c *Client) read() {
	var d Decoder
	buf := make([]byte, 512)
	for {
		n, err := c.rw.Read(buf)
		if n > 0 {
			frames, lines := d.Feed(buf[:n])
			c.mu.Lock()
			for _, l := range lines {
				if l = cleanLine(l); l != "" {
					c.lines = append(c.lines, l)
				}
			}
			if len(c.lines) > keepLines {
				c.lines = c.lines[len(c.lines)-keepLines:]
			}
			c.mu.Unlock()
			for _, f := range frames {
				select {
				case c.frames <- f:
				default: // nobody is waiting: drop it
				}
			}
		}
		if err != nil && !errors.Is(err, io.EOF) {
			c.mu.Lock()
			c.err = err
			c.mu.Unlock()
			close(c.frames)
			return
		}
	}
}

// Close closes the line.
func (c *Client) Close() error { return c.rw.Close() }

// RecentLog returns the device's last log lines.
func (c *Client) RecentLog() []string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([]string(nil), c.lines...)
}

// Wake asks the device's shell, if it has one, to lend the console to
// Improv ("tedge improv"). Firmware without a shell ignores the line.
func (c *Client) Wake() error {
	_, err := c.rw.Write([]byte("\r\ntedge improv\r\n"))
	return err
}

func (c *Client) drain() {
	for {
		select {
		case <-c.frames:
		default:
			return
		}
	}
}

// call sends an RPC command and waits for its result. onState, if not
// nil, sees each state the device reports meanwhile.
func (c *Client) call(ctx context.Context, cmd byte, payload []byte,
	timeout time.Duration, onState func(byte)) ([]string, error) {
	c.drain()
	if _, err := c.rw.Write(Encode(RPC(cmd, payload))); err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	for {
		select {
		case <-ctx.Done():
			if errors.Is(ctx.Err(), context.DeadlineExceeded) {
				return nil, ErrNoAnswer
			}
			return nil, ctx.Err()
		case f, ok := <-c.frames:
			if !ok {
				c.mu.Lock()
				defer c.mu.Unlock()
				return nil, c.err
			}
			switch f.Type {
			case TypeState:
				if onState != nil && len(f.Data) == 1 {
					onState(f.Data[0])
				}
			case TypeError:
				if len(f.Data) == 1 && f.Data[0] != ErrNone {
					return nil, &DeviceError{Code: f.Data[0]}
				}
			case TypeResult:
				rc, strs, ok := ParseResult(f.Data)
				if ok && rc == cmd {
					return strs, nil
				}
			}
		}
	}
}

// TedgeInfo is what the tiny-edge info command reports.
type TedgeInfo struct {
	ProtocolVersion int
	DeviceID        string
	Auth            string // "c8y-ca" or "bootstrap"
	Tenant          string // empty when none is set
	State           string // "awaiting-registration", "connected", ...
	DeviceType      string // empty from devices older than the field
}

// Info asks for the tiny-edge info.
func (c *Client) Info(ctx context.Context, timeout time.Duration) (TedgeInfo, error) {
	s, err := c.call(ctx, CmdTedgeInfo, nil, timeout, nil)
	if err != nil {
		return TedgeInfo{}, err
	}
	if len(s) < 5 {
		return TedgeInfo{}, fmt.Errorf("the device's answer was incomplete")
	}
	v, _ := strconv.Atoi(s[0])
	info := TedgeInfo{ProtocolVersion: v, DeviceID: s[1], Auth: s[2], Tenant: s[3], State: s[4]}
	if len(s) > 5 {
		info.DeviceType = s[5]
	}
	return info, nil
}

// DeviceInfo is what Improv's device info command reports.
type DeviceInfo struct {
	Firmware, Version, Chip, Name string
}

// DeviceInfo asks for Improv's device info.
func (c *Client) DeviceInfo(ctx context.Context) (DeviceInfo, error) {
	s, err := c.call(ctx, CmdDeviceInfo, nil, 3*time.Second, nil)
	if err != nil {
		return DeviceInfo{}, err
	}
	for len(s) < 4 {
		s = append(s, "")
	}
	return DeviceInfo{Firmware: s[0], Version: s[1], Chip: s[2], Name: s[3]}, nil
}

// SetWiFi sends a network and waits until the device has joined it.
func (c *Client) SetWiFi(ctx context.Context, ssid, password string, onState func(byte)) error {
	_, err := c.call(ctx, CmdWiFiSettings, Strings(ssid, password), 90*time.Second, onState)
	return err
}

// Onboard sends the tenant and one-time password.
func (c *Client) Onboard(ctx context.Context, tenant, otp string, reonboard bool) error {
	var flags byte
	if reonboard {
		flags |= FlagReonboard
	}
	payload := append([]byte{flags}, Strings(tenant, otp)...)
	_, err := c.call(ctx, CmdTedgeOnboard, payload, 30*time.Second, nil)
	return err
}

// Identify asks the device to make itself recognisable.
func (c *Client) Identify(ctx context.Context) error {
	_, err := c.call(ctx, CmdTedgeIdentify, nil, 3*time.Second, nil)
	return err
}

// Terminal control sequences a shell mixes into its output, and its prompt.
var ansi = regexp.MustCompile(`\x1b\[[0-9;?]*[A-Za-z]`)

// cleanLine strips terminal control sequences and shell prompts from a log
// line, so it can be shown to the user.
func cleanLine(l string) string {
	l = ansi.ReplaceAllString(l, "")
	for strings.HasPrefix(l, "uart:~$ ") {
		l = strings.TrimPrefix(l, "uart:~$ ")
	}
	return strings.TrimSpace(strings.TrimPrefix(l, "uart:~$"))
}

// Network is a Wi-Fi network the device can see.
type Network struct {
	SSID    string
	RSSI    int // dBm
	Secured bool
}

// ScanNetworks asks the device which Wi-Fi networks it can see: one result
// per network, strongest first, ended by an empty result.
func (c *Client) ScanNetworks(ctx context.Context) ([]Network, error) {
	c.drain()
	if _, err := c.rw.Write(Encode(RPC(CmdWiFiNetworks, nil))); err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	var nets []Network
	for {
		select {
		case <-ctx.Done():
			return nil, ErrNoAnswer
		case f, ok := <-c.frames:
			if !ok {
				return nil, io.ErrUnexpectedEOF
			}
			switch f.Type {
			case TypeError:
				if len(f.Data) == 1 && f.Data[0] != ErrNone {
					return nil, &DeviceError{Code: f.Data[0]}
				}
			case TypeResult:
				cmd, strs, ok := ParseResult(f.Data)
				if !ok || cmd != CmdWiFiNetworks {
					continue
				}
				if len(strs) == 0 {
					return nets, nil
				}
				n := Network{SSID: strs[0]}
				if len(strs) > 1 {
					n.RSSI, _ = strconv.Atoi(strs[1])
				}
				n.Secured = len(strs) < 3 || strs[2] == "YES"
				nets = append(nets, n)
			}
		}
	}
}
