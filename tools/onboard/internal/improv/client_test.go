package improv

import (
	"context"
	"io"
	"testing"
	"time"
)

// fakeDevice answers like the provisioning library, with log noise.
type fakeDevice struct {
	toTool   *io.PipeWriter
	fromTool *io.PipeReader
}

type pipeRW struct {
	io.Reader
	io.Writer
}

func (p pipeRW) Close() error { return nil }

func newFake(t *testing.T, answer func(cmd byte, payload []byte) [][]byte) *Client {
	toolR, devW := io.Pipe()
	devR, toolW := io.Pipe()
	go func() {
		var d Decoder
		buf := make([]byte, 256)
		for {
			n, err := devR.Read(buf)
			if err != nil {
				return
			}
			frames, _ := d.Feed(buf[:n])
			for _, f := range frames {
				devW.Write([]byte("<inf> tedge: a log line\r\n"))
				for _, out := range answer(f.Data[0], f.Data[2:]) {
					devW.Write(out)
				}
			}
		}
	}()
	return NewClient(pipeRW{Reader: toolR, Writer: toolW})
}

func result(cmd byte, strs ...string) []byte {
	data := append([]byte{cmd, 0}, Strings(strs...)...)
	data[1] = byte(len(data) - 2)
	return Encode(Frame{Type: TypeResult, Data: data})
}

func TestInfo(t *testing.T) {
	c := newFake(t, func(cmd byte, _ []byte) [][]byte {
		return [][]byte{result(cmd, "1", "tedge-d405927b33d0", "c8y-ca", "", "awaiting-registration")}
	})
	info, err := c.Info(context.Background(), time.Second)
	if err != nil || info.DeviceID != "tedge-d405927b33d0" || info.State != "awaiting-registration" {
		t.Fatalf("%+v %v", info, err)
	}
	if lines := c.RecentLog(); len(lines) != 1 {
		t.Fatalf("log: %q", lines)
	}
}

func TestWiFiStatesAndError(t *testing.T) {
	c := newFake(t, func(cmd byte, p []byte) [][]byte {
		return [][]byte{
			Encode(Frame{Type: TypeState, Data: []byte{StateProvisioning}}),
			Encode(Frame{Type: TypeError, Data: []byte{ErrUnableConnect}}),
		}
	})
	var states []byte
	err := c.SetWiFi(context.Background(), "net", "pw", func(s byte) { states = append(states, s) })
	if !IsDeviceError(err, ErrUnableConnect) || len(states) != 1 {
		t.Fatalf("%v %v", err, states)
	}
}

func TestNoAnswer(t *testing.T) {
	c := newFake(t, func(byte, []byte) [][]byte { return nil })
	if _, err := c.Info(context.Background(), 100*time.Millisecond); err != ErrNoAnswer {
		t.Fatalf("got %v", err)
	}
}
