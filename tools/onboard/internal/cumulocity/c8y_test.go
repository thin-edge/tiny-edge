package cumulocity

import (
	"errors"
	"strings"
	"testing"
)

type fakeRunner struct {
	answers map[string]string
	calls   []string
}

func (f *fakeRunner) Run(env []string, args ...string) ([]byte, error) {
	key := strings.Join(args, " ")
	f.calls = append(f.calls, key)
	if strings.HasPrefix(key, "identity get --name missing") {
		return nil, &ExitError{Code: exitNotFound, Stderr: "404 Not Found"}
	}
	if strings.HasPrefix(key, "identity get --name forbidden") {
		return nil, &ExitError{Code: 3, Stderr: "403 Forbidden"}
	}
	for prefix, out := range f.answers {
		if strings.HasPrefix(key, prefix) {
			return []byte(out), nil
		}
	}
	return nil, errors.New("exit status 1")
}

func (f *fakeRunner) Interactive(env []string, args ...string) ([]byte, error) {
	return f.Run(env, args...)
}

func TestSession(t *testing.T) {
	r := &fakeRunner{answers: map[string]string{
		"sessions get":      `{"host":"https://example.eu-latest.cumulocity.com","tenant":"t1","username":"me"}`,
		"currenttenant get": `{"name":"t1"}`,
	}}
	s, err := (&Client{Run: r}).Session()
	if err != nil || s.Host != "example.eu-latest.cumulocity.com" {
		t.Fatalf("%+v %v", s, err)
	}
}

func TestNoSession(t *testing.T) {
	r := &fakeRunner{answers: map[string]string{
		"sessions get": `{"host":"https://example.com"}`,
	}}
	if _, err := (&Client{Run: r}).Session(); err != ErrNoSession {
		t.Fatalf("got %v", err)
	}
}

func TestFindDevice(t *testing.T) {
	r := &fakeRunner{answers: map[string]string{
		"identity get --name tedge-1": `{"managedObject":{"id":"42"}}`,
		"inventory get --id 42":       `{"id":"42","name":"Pump 7","c8y_Availability":{"status":"AVAILABLE"}}`,
		"identity get --name tedge-2": ``,
	}}
	c := &Client{Run: r}
	d, found, err := c.FindDevice("tedge-1")
	if err != nil || !found || d.Name != "Pump 7" || !d.Available {
		t.Fatalf("%+v %v %v", d, found, err)
	}
	if _, found, err := c.FindDevice("tedge-2"); found || err != nil {
		t.Fatalf("found %v %v", found, err)
	}
}

func TestParseExports(t *testing.T) {
	out := []byte("export C8Y_HOST='https://example.com'\nexport C8Y_TOKEN='a'\\''b'\nexport C8Y_X=\"q\\\"r\"\necho hi\n")
	env := ParseExports(out)
	want := []string{"C8Y_HOST=https://example.com", "C8Y_TOKEN=a'b", `C8Y_X=q"r`}
	if strings.Join(env, "|") != strings.Join(want, "|") {
		t.Fatalf("got %q", env)
	}
}

func TestFindDeviceExitCodes(t *testing.T) {
	c := &Client{Run: &fakeRunner{}}
	// go-c8y-cli exits with 4 when the identity does not exist.
	if _, found, err := c.FindDevice("missing"); found || err != nil {
		t.Fatalf("not found: found=%v err=%v", found, err)
	}
	// Any other failure is a failure, with c8y's message.
	if _, _, err := c.FindDevice("forbidden"); err == nil || err.Error() != "403 Forbidden" {
		t.Fatalf("forbidden: %v", err)
	}
}

func TestRegisterWithTheDeviceType(t *testing.T) {
	r := &fakeRunner{answers: map[string]string{"deviceregistration register-ca": `{}`}}
	c := &Client{Run: r}
	if err := c.Register("tedge-1", "Pump 7", "otp", "thin-edge.io-esp-idf-agent"); err != nil {
		t.Fatal(err)
	}
	if err := c.Register("tedge-2", "Pump 8", "otp", ""); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(r.calls[0], "--type thin-edge.io-esp-idf-agent") {
		t.Fatalf("no device type: %q", r.calls[0])
	}
	if strings.Contains(r.calls[1], "--type") {
		t.Fatalf("a type although the device reported none: %q", r.calls[1])
	}
}
