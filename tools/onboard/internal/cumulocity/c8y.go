// Package cumulocity reaches Cumulocity through go-c8y-cli (the c8y
// command), with the user's own session, so the tool needs no credentials.
package cumulocity

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"strings"
)

// Runner runs c8y with the given environment additions and returns its
// standard output. Interactive runs connect c8y to the terminal for its
// prompts, and still capture standard output.
type Runner interface {
	Run(env []string, args ...string) ([]byte, error)
	Interactive(env []string, args ...string) ([]byte, error)
}

// Exec runs the real c8y.
type Exec struct{}

// ExitError is a c8y run that failed. go-c8y-cli maps HTTP errors to
// exit codes: 4 is 404 (not found).
type ExitError struct {
	Code   int
	Stderr string
}

func (e *ExitError) Error() string {
	if e.Stderr != "" {
		return e.Stderr
	}
	return fmt.Sprintf("c8y exited with status %d", e.Code)
}

// exitNotFound is go-c8y-cli's exit code for HTTP 404.
const exitNotFound = 4

func isNotFound(err error) bool {
	var ee *ExitError
	return errors.As(err, &ee) && ee.Code == exitNotFound
}

func (Exec) Run(env []string, args ...string) ([]byte, error) {
	cmd := exec.Command("c8y", args...)
	cmd.Env = append(os.Environ(), env...)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		var xe *exec.ExitError
		if errors.As(err, &xe) {
			return out, &ExitError{Code: xe.ExitCode(), Stderr: lastLine(stderr.String())}
		}
		return out, err
	}
	return out, nil
}

// lastLine is the last non-empty line of c8y's error output: its message.
func lastLine(s string) string {
	lines := strings.Split(strings.TrimSpace(s), "\n")
	return strings.TrimSpace(lines[len(lines)-1])
}

func (Exec) Interactive(env []string, args ...string) ([]byte, error) {
	cmd := exec.Command("c8y", args...)
	cmd.Env = append(os.Environ(), env...)
	cmd.Stdin = os.Stdin
	cmd.Stderr = os.Stderr
	return cmd.Output()
}

// Installed reports whether c8y is on the PATH.
func Installed() bool {
	_, err := exec.LookPath("c8y")
	return err == nil
}

// InstallHelp says how to install go-c8y-cli on an operating system.
func InstallHelp(goos string) string {
	switch goos {
	case "darwin":
		return "Install it with Homebrew:  brew install go-c8y-cli"
	case "windows":
		return "Install it with Scoop or winget, as described at https://goc8ycli.netlify.app/docs/installation/"
	default:
		return "Install it as described at https://goc8ycli.netlify.app/docs/installation/"
	}
}

// Client calls c8y.
type Client struct {
	Run Runner
	env []string // the session the user picked in this run, if any
}

// Session is the Cumulocity login c8y uses.
type Session struct {
	Host   string // "example.cumulocity.com", without the scheme
	Tenant string
	User   string
}

// ErrNoSession means c8y has no working login.
var ErrNoSession = errors.New("go-c8y-cli is not logged in to Cumulocity")

// Session returns the active session, after checking that it works.
func (c *Client) Session() (Session, error) {
	out, err := c.Run.Run(c.env, "sessions", "get", "-o", "json")
	if err != nil {
		return Session{}, ErrNoSession
	}
	var s struct {
		Host     string `json:"host"`
		Tenant   string `json:"tenant"`
		Username string `json:"username"`
	}
	if json.Unmarshal(out, &s) != nil || s.Host == "" {
		return Session{}, ErrNoSession
	}
	if _, err := c.Run.Run(c.env, "currenttenant", "get", "-o", "json"); err != nil {
		return Session{}, ErrNoSession
	}
	host := strings.TrimPrefix(strings.TrimPrefix(s.Host, "https://"), "http://")
	return Session{Host: strings.TrimSuffix(host, "/"), Tenant: s.Tenant, User: s.Username}, nil
}

// ChooseSession runs go-c8y-cli's own session picker and login in the
// terminal, and uses the session the user picks for the rest of the run.
func (c *Client) ChooseSession() error {
	out, err := c.Run.Interactive(nil, "sessions", "set", "--shell", "bash")
	if err != nil {
		return err
	}
	env := ParseExports(out)
	if len(env) == 0 {
		return ErrNoSession
	}
	c.env = env
	return nil
}

// ParseExports reads the shell lines "export NAME='value'" that
// `c8y sessions set --shell bash` prints.
func ParseExports(out []byte) []string {
	var env []string
	sc := bufio.NewScanner(bytes.NewReader(out))
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if !strings.HasPrefix(line, "export ") {
			continue
		}
		name, value, ok := strings.Cut(strings.TrimPrefix(line, "export "), "=")
		if !ok || name == "" {
			continue
		}
		env = append(env, name+"="+unquote(value))
	}
	return env
}

// unquote undoes bash quoting of one word: 'single' (with '\” for a
// quote), "double" (with backslash escapes), or bare.
func unquote(v string) string {
	var b strings.Builder
	for i := 0; i < len(v); i++ {
		switch v[i] {
		case '\'':
			j := strings.IndexByte(v[i+1:], '\'')
			if j < 0 {
				b.WriteString(v[i+1:])
				return b.String()
			}
			b.WriteString(v[i+1 : i+1+j])
			i += j + 1
		case '"':
			for i++; i < len(v) && v[i] != '"'; i++ {
				if v[i] == '\\' && i+1 < len(v) {
					i++
				}
				b.WriteByte(v[i])
			}
		case '\\':
			if i+1 < len(v) {
				i++
				b.WriteByte(v[i])
			}
		default:
			b.WriteByte(v[i])
		}
	}
	return b.String()
}

// Device is a device in Cumulocity.
type Device struct {
	ID        string
	Name      string
	Available bool
}

// FindDevice looks a device up by the board's ID.
func (c *Client) FindDevice(externalID string) (Device, bool, error) {
	out, err := c.Run.Run(c.env, "identity", "get", "--name", externalID,
		"--type", "c8y_Serial", "--silentStatusCodes", "404", "-o", "json")
	if isNotFound(err) {
		return Device{}, false, nil
	}
	if err != nil {
		return Device{}, false, err
	}
	var id struct {
		ManagedObject struct {
			ID string `json:"id"`
		} `json:"managedObject"`
	}
	if len(bytes.TrimSpace(out)) == 0 || json.Unmarshal(out, &id) != nil ||
		id.ManagedObject.ID == "" {
		return Device{}, false, nil
	}
	d, err := c.Device(id.ManagedObject.ID)
	return d, err == nil, err
}

// Device reads a device by its ID in Cumulocity.
func (c *Client) Device(id string) (Device, error) {
	out, err := c.Run.Run(c.env, "inventory", "get", "--id", id, "-o", "json")
	if err != nil {
		return Device{}, err
	}
	var mo struct {
		ID           string `json:"id"`
		Name         string `json:"name"`
		Availability struct {
			Status string `json:"status"`
		} `json:"c8y_Availability"`
	}
	if err := json.Unmarshal(out, &mo); err != nil {
		return Device{}, err
	}
	return Device{ID: mo.ID, Name: mo.Name, Available: mo.Availability.Status == "AVAILABLE"}, nil
}

// Register registers the board's ID with a one-time password, so the board
// can enroll for a certificate.
func (c *Client) Register(externalID, name, otp string) error {
	_, err := c.Run.Run(c.env, "deviceregistration", "register-ca",
		"--id", externalID, "--name", name, "--one-time-password", otp,
		"--force", "-o", "json")
	return err
}

// DeviceURL is the device's page in Cumulocity's Device Management.
func DeviceURL(host, id string) string {
	return fmt.Sprintf("https://%s/apps/devicemanagement/index.html#/device/%s/device-info", host, id)
}
