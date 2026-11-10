// Package improv speaks Improv Wi-Fi over a serial line
// (https://www.improv-wifi.com), with the tiny-edge commands that the
// device-side provisioning library adds (include/tedge/provisioning.h).
package improv

import "bytes"

// Frame types.
const (
	TypeState  = 0x01
	TypeError  = 0x02
	TypeRPC    = 0x03
	TypeResult = 0x04
)

// States a device reports.
const (
	StateReady        = 0x02
	StateProvisioning = 0x03
	StateProvisioned  = 0x04
)

// Errors a device reports.
const (
	ErrNone          = 0x00
	ErrInvalidRPC    = 0x01
	ErrUnknownRPC    = 0x02
	ErrUnableConnect = 0x03
	ErrNotAuthorized = 0x04
	ErrUnknown       = 0xFF
)

// Commands: Improv's own, and tiny-edge's.
const (
	CmdWiFiSettings  = 0x01
	CmdCurrentState  = 0x02
	CmdDeviceInfo    = 0x03
	CmdWiFiNetworks  = 0x04
	CmdTedgeInfo     = 0xE0
	CmdTedgeOnboard  = 0xE1
	CmdTedgeIdentify = 0xE2
)

// FlagReonboard in the onboard command sets up a device that is already
// connected (it gets a new certificate).
const FlagReonboard = 0x01

// ProtocolVersion is the version of the tiny-edge commands this tool knows.
const ProtocolVersion = 1

var header = []byte("IMPROV")

const version = 1

// Frame is one Improv Serial frame.
type Frame struct {
	Type byte
	Data []byte
}

// Encode returns the bytes of a frame, ending with a newline.
func Encode(f Frame) []byte {
	var b bytes.Buffer
	b.Write(header)
	b.WriteByte(version)
	b.WriteByte(f.Type)
	b.WriteByte(byte(len(f.Data)))
	b.Write(f.Data)
	var sum byte
	for _, c := range b.Bytes() {
		sum += c
	}
	b.WriteByte(sum)
	b.WriteByte('\n')
	return b.Bytes()
}

// RPC builds an RPC command frame: [command][length][payload].
func RPC(cmd byte, payload []byte) Frame {
	data := append([]byte{cmd, byte(len(payload))}, payload...)
	return Frame{Type: TypeRPC, Data: data}
}

// Strings encodes length-prefixed strings, as Improv payloads carry them.
func Strings(s ...string) []byte {
	var b []byte
	for _, v := range s {
		b = append(b, byte(len(v)))
		b = append(b, v...)
	}
	return b
}

// ParseResult splits an RPC result into its command and strings.
func ParseResult(data []byte) (cmd byte, strs []string, ok bool) {
	if len(data) < 2 || int(data[1])+2 != len(data) {
		return 0, nil, false
	}
	cmd = data[0]
	for pos := 2; pos < len(data); {
		n := int(data[pos])
		pos++
		if pos+n > len(data) {
			return 0, nil, false
		}
		strs = append(strs, string(data[pos:pos+n]))
		pos += n
	}
	return cmd, strs, true
}

// Decoder finds frames in a byte stream that also carries other text (the
// device's log), and collects that text by line.
type Decoder struct {
	buf []byte
}

// Feed adds received bytes and returns the frames and text lines they
// complete.
func (d *Decoder) Feed(p []byte) (frames []Frame, lines []string) {
	d.buf = append(d.buf, p...)
	for {
		i := bytes.Index(d.buf, header)
		if i < 0 {
			// Keep a tail that may be the start of a header.
			keep := len(header) - 1
			if keep > len(d.buf) {
				keep = len(d.buf)
			}
			lines = append(lines, d.takeLines(len(d.buf)-keep)...)
			return frames, lines
		}
		lines = append(lines, d.takeLines(i)...)
		// Text before the header without a newline of its own is cut
		// off by the frame; drop it, so the buffer starts at the header.
		if j := bytes.Index(d.buf, header); j > 0 {
			d.buf = d.buf[j:]
		}
		if len(d.buf) < len(header)+3 {
			return frames, lines
		}
		n := int(d.buf[len(header)+2])
		total := len(header) + 3 + n + 1
		if d.buf[len(header)] != version {
			d.buf = d.buf[1:]
			continue
		}
		if len(d.buf) < total {
			return frames, lines
		}
		var sum byte
		for _, c := range d.buf[:total-1] {
			sum += c
		}
		if sum != d.buf[total-1] {
			d.buf = d.buf[1:]
			continue
		}
		data := append([]byte(nil), d.buf[len(header)+3:total-1]...)
		frames = append(frames, Frame{Type: d.buf[len(header)+1], Data: data})
		d.buf = d.buf[total:]
		if len(d.buf) > 0 && d.buf[0] == '\n' {
			d.buf = d.buf[1:]
		}
	}
}

// takeLines removes the first n bytes of the buffer, returning the
// complete lines among them; a partial last line stays for later.
func (d *Decoder) takeLines(n int) []string {
	if n <= 0 {
		return nil
	}
	chunk := d.buf[:n]
	last := bytes.LastIndexByte(chunk, '\n')
	var lines []string
	if last >= 0 {
		for _, l := range bytes.Split(chunk[:last], []byte("\n")) {
			l = bytes.TrimRight(l, "\r")
			if len(l) > 0 {
				lines = append(lines, string(l))
			}
		}
		d.buf = d.buf[last+1:]
		return lines
	}
	// No newline yet: keep everything, unless it grows without end.
	if len(d.buf) > 4096 {
		d.buf = d.buf[n:]
	}
	return nil
}
