package improv

import (
	"bytes"
	"testing"
)

// frameBytes builds a frame by hand, as the device's C code does.
func frameBytes(typ byte, data []byte) []byte {
	b := append([]byte("IMPROV"), 1, typ, byte(len(data)))
	b = append(b, data...)
	var sum byte
	for _, c := range b {
		sum += c
	}
	return append(b, sum, '\n')
}

func TestEncodeMatchesTheDevice(t *testing.T) {
	got := Encode(RPC(CmdTedgeInfo, nil))
	want := frameBytes(TypeRPC, []byte{CmdTedgeInfo, 0})
	if !bytes.Equal(got, want) {
		t.Fatalf("got % x, want % x", got, want)
	}
}

func TestDecoderBetweenLogLines(t *testing.T) {
	var d Decoder
	stream := []byte("[00:00:01.000] <inf> tedge: starting\r\n")
	stream = append(stream, frameBytes(TypeState, []byte{StateReady})...)
	stream = append(stream, []byte("uart:~$ \r\n<wrn> app: IMPRO not a frame\r\n")...)
	stream = append(stream, frameBytes(TypeResult, []byte{CmdTedgeInfo, 0})...)

	var frames []Frame
	var lines []string
	// A byte at a time, as a slow line delivers it.
	for _, c := range stream {
		f, l := d.Feed([]byte{c})
		frames = append(frames, f...)
		lines = append(lines, l...)
	}
	if len(frames) != 2 || frames[0].Type != TypeState || frames[1].Type != TypeResult {
		t.Fatalf("frames: %+v", frames)
	}
	if len(lines) != 3 || lines[0] != "[00:00:01.000] <inf> tedge: starting" {
		t.Fatalf("lines: %q", lines)
	}
}

func TestDecoderSkipsDamagedFrames(t *testing.T) {
	var d Decoder
	bad := frameBytes(TypeState, []byte{StateReady})
	bad[len(bad)-2] ^= 0xFF
	good := frameBytes(TypeError, []byte{ErrUnknownRPC})
	frames, _ := d.Feed(append(bad, good...))
	if len(frames) != 1 || frames[0].Type != TypeError {
		t.Fatalf("frames: %+v", frames)
	}
}

func TestDecoderFrameAfterPartialText(t *testing.T) {
	var d Decoder
	stream := append([]byte("uart:~$ "), frameBytes(TypeState, []byte{StateProvisioned})...)
	frames, _ := d.Feed(stream)
	if len(frames) != 1 || frames[0].Data[0] != StateProvisioned {
		t.Fatalf("frames: %+v", frames)
	}
}

func TestParseResult(t *testing.T) {
	data := append([]byte{CmdTedgeInfo, 0}, Strings("1", "tedge-abc", "c8y-ca", "", "stopped")...)
	data[1] = byte(len(data) - 2)
	cmd, strs, ok := ParseResult(data)
	if !ok || cmd != CmdTedgeInfo || len(strs) != 5 || strs[1] != "tedge-abc" || strs[3] != "" {
		t.Fatalf("got %v %q %v", cmd, strs, ok)
	}
	if _, _, ok := ParseResult([]byte{CmdTedgeInfo, 5, 1}); ok {
		t.Fatal("a short result parsed")
	}
}
