// tedge-onboard sets up a tiny-edge device for Cumulocity over a USB cable:
// it gives the device the computer's Wi-Fi network, registers it in
// Cumulocity through go-c8y-cli, and waits until it is connected.
//
// Run it with no arguments and it guides you through. Flags are for scripts.
package main

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math/big"
	"os"
	"runtime"
	"strings"
	"time"

	"golang.org/x/term"

	"github.com/thin-edge/tiny-edge/tools/onboard/internal/cumulocity"
	"github.com/thin-edge/tiny-edge/tools/onboard/internal/device"
	"github.com/thin-edge/tiny-edge/tools/onboard/internal/improv"
	"github.com/thin-edge/tiny-edge/tools/onboard/internal/ui"
	"github.com/thin-edge/tiny-edge/tools/onboard/internal/wifi"
)

var version = "dev"

type options struct {
	port         string
	wifiSSID     string
	wifiPassword bool // read it from standard input
	skipWiFi     bool
	reonboard    bool
	yes          bool
	json         bool
	name         string
	timeout      time.Duration
}

func main() {
	var o options
	flag.StringVar(&o.port, "port", "", "the board's serial port (found by itself when not given)")
	flag.StringVar(&o.wifiSSID, "wifi-ssid", "", "the Wi-Fi network for the board (default: the computer's)")
	flag.BoolVar(&o.wifiPassword, "wifi-password-stdin", false, "read the Wi-Fi password from standard input")
	flag.BoolVar(&o.skipWiFi, "skip-wifi", false, "leave the board's Wi-Fi as it is")
	flag.BoolVar(&o.reonboard, "reonboard", false, "set up a board that is already in Cumulocity again (it gets a new certificate)")
	flag.BoolVar(&o.yes, "yes", false, "ask no questions; fail where one would be asked")
	flag.BoolVar(&o.json, "json", false, "print the result as JSON (with --yes)")
	flag.StringVar(&o.name, "name", "", "the device's name in Cumulocity (default: the board's ID)")
	flag.DurationVar(&o.timeout, "timeout", 3*time.Minute, "how long to wait for the device to connect")
	showVersion := flag.Bool("version", false, "print the version")
	flag.Parse()
	if *showVersion {
		fmt.Println(version)
		return
	}

	var u ui.UI
	if o.yes || !term.IsTerminal(int(os.Stdin.Fd())) {
		u = &ui.Script{Out: os.Stderr}
	} else {
		u = ui.NewGuided()
	}
	code := run(u, o)
	pauseIfStartedByDoubleClick(u)
	os.Exit(code)
}

// result is what --json prints.
type result struct {
	Name     string `json:"name"`
	DeviceID string `json:"deviceId"`
	ID       string `json:"id"`
	Link     string `json:"link"`
	Status   string `json:"status"`
}

func run(u ui.UI, o options) int {
	u.Title("Set up a device for Cumulocity")
	f := &flow{u: u, o: o, c8y: &cumulocity.Client{Run: cumulocity.Exec{}},
		wifi: wifi.Detector{GOOS: runtime.GOOS, Run: wifi.Exec}}

	for {
		res, err := f.once()
		if errors.Is(err, ui.ErrCancelled) {
			u.Say("Stopped. Nothing more was changed.")
			return 130
		}
		if err != nil {
			var na *ui.NeedsAnswerError
			if errors.As(err, &na) {
				u.Problem("A question needs an answer: "+na.Question, na.Hint)
			}
			return 1
		}
		if o.json {
			_ = json.NewEncoder(os.Stdout).Encode(res)
		}
		if !u.Interactive() {
			return 0
		}
		another, err := u.Confirm("Set up another board?", "Plug it in first.", false, "")
		if err != nil || !another {
			return 0
		}
		f.o.port = ""
	}
}

type flow struct {
	u    ui.UI
	o    options
	c8y  *cumulocity.Client
	wifi wifi.Detector
}

// once sets up one board.
func (f *flow) once() (result, error) {
	session, err := f.cumulocity()
	if err != nil {
		return result{}, err
	}
	board, err := f.findBoard()
	if err != nil {
		return result{}, err
	}
	defer board.Client.Close()
	if err := f.checkBoard(board); err != nil {
		return result{}, err
	}

	existing, again, err := f.alreadySetUp(board)
	if err != nil {
		return result{}, err
	}

	ssid, password, err := f.chooseWiFi(board)
	if err != nil {
		return result{}, err
	}

	name := f.o.name
	if name == "" {
		name = board.Info.DeviceID
		if existing.Name != "" {
			name = existing.Name
		}
		name, err = f.u.Input("Name for the device in Cumulocity",
			"This is how it is listed in Device Management. You can change it later.",
			name, "--name")
		if err != nil {
			return result{}, err
		}
	}

	if f.u.Interactive() {
		summary := fmt.Sprintf("Board:       %s\nCumulocity:  %s\nName:        %s",
			board.Describe(), session.Host, name)
		if ssid != "" {
			summary += "\nWi-Fi:       " + ssid
		}
		ok, err := f.u.Confirm("Set up this device?", summary, true, "")
		if err != nil {
			return result{}, err
		}
		if !ok {
			return result{}, ui.ErrCancelled
		}
	}

	if ssid != "" {
		if err := f.sendWiFi(board, ssid, password); err != nil {
			return result{}, err
		}
	}
	return f.register(board, session, name, again)
}

// cumulocity makes sure go-c8y-cli is there and logged in.
func (f *flow) cumulocity() (cumulocity.Session, error) {
	for !cumulocity.Installed() {
		f.u.Problem("This tool uses go-c8y-cli to reach Cumulocity, and it is not installed.",
			cumulocity.InstallHelp(runtime.GOOS))
		if err := f.retry("Check again", "--yes cannot install it"); err != nil {
			return cumulocity.Session{}, err
		}
	}
	for {
		var s cumulocity.Session
		err := f.u.Step("Checking your Cumulocity login", func(context.Context) (string, error) {
			var err error
			s, err = f.c8y.Session()
			if err != nil {
				return "", err
			}
			return "Cumulocity: " + s.Host, nil
		})
		if err == nil {
			return s, nil
		}
		f.u.Problem("go-c8y-cli is not logged in to Cumulocity.",
			"Pick one of your saved logins, or create one, in the next step.")
		choice, err := f.u.Choose("How do you want to log in?", "", []ui.Option{
			{Label: "Choose or create a Cumulocity login (go-c8y-cli)", Value: "login"},
			{Label: "Quit", Value: "quit"},
		}, "log in with go-c8y-cli first: eval $(c8y sessions set)")
		if err != nil {
			return cumulocity.Session{}, err
		}
		if choice == "quit" {
			return cumulocity.Session{}, ui.ErrCancelled
		}
		if err := f.c8y.ChooseSession(); err != nil {
			f.u.Warn("The login did not finish.")
		}
	}
}

// findBoard finds the board to set up: the only one, the one the user
// picks, or the one plugged in next.
func (f *flow) findBoard() (*device.Board, error) {
	for {
		var boards []*device.Board
		var notTedge []string
		err := f.u.Step("Looking for a board on USB", func(ctx context.Context) (string, error) {
			ports := []string{f.o.port}
			if f.o.port == "" {
				var err error
				if ports, err = device.Candidates(); err != nil {
					return "", err
				}
			}
			for _, r := range device.ProbeAll(ctx, ports) {
				switch {
				case r.Board != nil:
					boards = append(boards, r.Board)
				case errors.Is(r.Err, device.ErrNotTedge):
					notTedge = append(notTedge, r.Port)
				}
			}
			switch len(boards) {
			case 0:
				return "", errors.New("no board found")
			case 1:
				return "Found " + boards[0].Describe(), nil
			default:
				return fmt.Sprintf("Found %d boards", len(boards)), nil
			}
		})
		if err == nil {
			if len(boards) == 1 {
				return boards[0], nil
			}
			return f.pickBoard(boards)
		}

		switch {
		case len(notTedge) > 0:
			f.u.Problem("A board is connected, but its firmware cannot be set up this way.",
				"It needs firmware built with the tiny-edge provisioning library. "+
					"Install that firmware first, then try again.")
		default:
			f.u.Problem("No board was found.",
				"Plug the board into this computer with a USB data cable "+
					"(some cables only charge), then try again.")
		}
		if err := f.retry("Look again", "--port names the board's port"); err != nil {
			return nil, err
		}
	}
}

func (f *flow) pickBoard(boards []*device.Board) (*device.Board, error) {
	for {
		opts := make([]ui.Option, 0, len(boards)+1)
		for i, b := range boards {
			opts = append(opts, ui.Option{Label: b.Describe(), Value: fmt.Sprint(i)})
		}
		opts = append(opts, ui.Option{Label: "Not sure which is which? Make one blink", Value: "identify"})
		choice, err := f.u.Choose("Which board do you want to set up?", "", opts, "--port")
		if err != nil {
			return nil, err
		}
		if choice != "identify" {
			var i int
			fmt.Sscan(choice, &i)
			for j, b := range boards {
				if j != i {
					b.Client.Close()
				}
			}
			return boards[i], nil
		}
		opts = opts[:len(opts)-1]
		which, err := f.u.Choose("Which board should identify itself?",
			"It blinks if it has a light for it, and writes a line to its log.", opts, "")
		if err != nil {
			return nil, err
		}
		var i int
		fmt.Sscan(which, &i)
		_ = boards[i].Client.Identify(context.Background())
		f.u.Say("Look at the boards now.")
	}
}

// checkBoard makes sure the tool and the board's firmware fit together.
func (f *flow) checkBoard(b *device.Board) error {
	if b.Info.ProtocolVersion > improv.ProtocolVersion {
		f.u.Problem("This board's firmware is newer than this tool.",
			"Download the latest tedge-onboard and run it again.")
		return errors.New("tool too old")
	}
	if b.Info.Auth != "c8y-ca" {
		f.u.Problem("This board's firmware connects with a Cumulocity bootstrap user, "+
			"not with a certificate, so it cannot be registered this way.",
			"Register it in Cumulocity's Device Management instead.")
		return errors.New("bootstrap firmware")
	}
	return nil
}

// alreadySetUp asks before setting up a board that Cumulocity already knows.
func (f *flow) alreadySetUp(b *device.Board) (existing cumulocity.Device, again bool, err error) {
	var found bool
	err = f.u.Step("Checking whether the device is already in Cumulocity", func(context.Context) (string, error) {
		var err error
		existing, found, err = f.c8y.FindDevice(b.Info.DeviceID)
		if err != nil {
			return "", err
		}
		if found {
			return fmt.Sprintf("The device is already in Cumulocity as %q", existing.Name), nil
		}
		return "The device is new to Cumulocity", nil
	})
	if err != nil {
		f.u.Problem("Cumulocity could not be asked about the device.",
			"Cumulocity said: "+err.Error()+"\nCheck your network and your Cumulocity user's permissions, then try again.")
		return existing, false, err
	}
	if !found && b.Info.State != "connected" {
		return existing, false, nil
	}
	if f.o.reonboard {
		return existing, true, nil
	}
	ok, err := f.u.Confirm("Set this device up again?",
		"It is already set up. Setting it up again gives it a new certificate; "+
			"its history in Cumulocity stays.", false, "--reonboard")
	if err != nil {
		return existing, false, err
	}
	if !ok {
		return existing, false, ui.ErrCancelled
	}
	return existing, true, nil
}

// chooseWiFi picks the network for the board and its password.
func (f *flow) chooseWiFi(b *device.Board) (ssid, password string, err error) {
	if f.o.skipWiFi {
		return "", "", nil
	}
	ssid = f.o.wifiSSID
	if ssid == "" && !f.u.Interactive() {
		current, ok := f.wifi.Current()
		if !ok {
			return "", "", &ui.NeedsAnswerError{Question: "Which Wi-Fi network?",
				Hint: "--wifi-ssid, or --skip-wifi"}
		}
		ssid = current
	}
	var scanned []improv.Network
	if ssid == "" {
		scanOK := false
		_ = f.u.Step("Asking the board which Wi-Fi networks it can see", func(ctx context.Context) (string, error) {
			nets, err := b.Client.ScanNetworks(ctx)
			if err != nil {
				// Older firmware cannot scan: offer this computer's
				// networks instead, without failing the step.
				return "The board cannot list networks; showing this computer's", nil
			}
			scanned, scanOK = nets, true
			return fmt.Sprintf("The board sees %d Wi-Fi networks", len(nets)), nil
		})
		current, _ := f.wifi.Current()
		opts := wifiOptions(scanned, scanOK, current, f.wifi.Known())
		opts = append(opts, ui.Option{Label: "Another network (type its name)", Value: choiceOther})
		if b.Info.State == "connected" || b.Info.State == "awaiting-registration" ||
			b.Info.State == "connecting" {
			opts = append(opts, ui.Option{Label: "Keep the board's current Wi-Fi", Value: choiceKeep})
		}
		ssid, err = f.u.Choose("Which Wi-Fi network should the board use?",
			"The board needs a network that reaches the internet.", opts, "")
		if err != nil {
			return "", "", err
		}
		switch ssid {
		case choiceKeep:
			return "", "", nil
		case choiceOther:
			if ssid, err = f.u.Input("Wi-Fi network name", "", "", ""); err != nil {
				return "", "", err
			}
		}
	}

	if openNetwork(scanned, ssid) {
		return ssid, "", nil
	}
	if f.o.wifiPassword {
		pw, err := io.ReadAll(os.Stdin)
		if err != nil {
			return "", "", err
		}
		return ssid, strings.TrimRight(string(pw), "\r\n"), nil
	}
	if f.u.Interactive() {
		f.u.Say(fmt.Sprintf("Getting the password for %q from this computer.", ssid))
		if runtime.GOOS == "darwin" {
			f.u.Say("macOS may ask you to allow this.")
		}
	}
	password, err = f.wifi.Password(ssid)
	if err == nil {
		return ssid, password, nil
	}
	password, err = f.u.Secret(fmt.Sprintf("Password for %q", ssid),
		"This computer did not give the password. Leave it empty for an open network.",
		"--wifi-password-stdin")
	return ssid, password, err
}

// sendWiFi gives the board the network, asking again if it cannot join.
func (f *flow) sendWiFi(b *device.Board, ssid, password string) error {
	for {
		err := f.u.Step(fmt.Sprintf("Connecting the board to %q", ssid), func(ctx context.Context) (string, error) {
			if err := b.Client.SetWiFi(ctx, ssid, password, nil); err != nil {
				return "", err
			}
			return fmt.Sprintf("The board is on %q", ssid), nil
		})
		if err == nil {
			return nil
		}
		if !improv.IsDeviceError(err, improv.ErrUnableConnect) {
			f.u.Problem("The board stopped answering.", boardAdvice(b))
			return err
		}
		f.u.Problem(fmt.Sprintf("The board could not join %q.", ssid),
			"The password may be wrong, or the network may be 5 GHz only or out of range.")
		if !f.u.Interactive() {
			return err
		}
		pw, err := f.u.Secret(fmt.Sprintf("Password for %q", ssid), "Enter it again to retry.", "")
		if err != nil {
			return err
		}
		password = pw
	}
}

// register registers the device, hands the board its details, and waits
// for it to connect.
func (f *flow) register(b *device.Board, s cumulocity.Session, name string, again bool) (result, error) {
	otp, err := oneTimePassword()
	if err != nil {
		return result{}, err
	}
	if err := f.u.Step("Registering the device in Cumulocity", func(context.Context) (string, error) {
		return "Registered in Cumulocity", f.c8y.Register(b.Info.DeviceID, name, otp)
	}); err != nil {
		f.u.Problem("Cumulocity did not accept the registration.",
			"Cumulocity said: "+err.Error()+"\nYour Cumulocity user needs permission to register devices.")
		return result{}, err
	}
	if err := f.u.Step("Giving the board its Cumulocity details", func(ctx context.Context) (string, error) {
		return "The board has its Cumulocity details", b.Client.Onboard(ctx, s.Host, otp, again)
	}); err != nil {
		f.u.Problem("The board did not take its Cumulocity details.", boardAdvice(b))
		return result{}, err
	}

	var dev cumulocity.Device
	err = f.u.Step("Waiting for the device to connect (this takes up to a minute)", func(ctx context.Context) (string, error) {
		ctx, cancel := context.WithTimeout(ctx, f.o.timeout)
		defer cancel()
		state := ""
		for {
			if info, err := b.Client.Info(ctx, 3*time.Second); err == nil {
				state = info.State
			}
			if state == "connected" {
				if d, found, err := f.c8y.FindDevice(b.Info.DeviceID); err == nil && found {
					dev = d
					return fmt.Sprintf("%q is connected", d.Name), nil
				}
			}
			select {
			case <-ctx.Done():
				return "", fmt.Errorf("still %s after %s", stateWords(state), f.o.timeout)
			case <-time.After(3 * time.Second):
			}
		}
	})
	if err != nil {
		f.u.Problem("The device did not connect in time.",
			"Check that the Wi-Fi network reaches the internet. The board keeps trying, "+
				"so it may still appear in Cumulocity in a few minutes.")
		return result{}, err
	}
	link := cumulocity.DeviceURL(s.Host, dev.ID)
	if f.u.Interactive() {
		f.u.Say("")
		f.u.Say("Open it in Cumulocity:")
		f.u.Say(link)
		f.u.Say("")
	}
	return result{Name: dev.Name, DeviceID: b.Info.DeviceID, ID: dev.ID, Link: link, Status: "connected"}, nil
}

// retry offers to try again; in script mode it fails.
func (f *flow) retry(label, hint string) error {
	choice, err := f.u.Choose("What now?", "", []ui.Option{
		{Label: label, Value: "retry"}, {Label: "Quit", Value: "quit"},
	}, hint)
	if err != nil {
		return err
	}
	if choice == "quit" {
		return ui.ErrCancelled
	}
	return nil
}

func boardAdvice(b *device.Board) string {
	advice := "Unplug the board, plug it in again, and run this tool again."
	if lines := b.Client.RecentLog(); len(lines) > 0 {
		advice += "\nThe board's last message: " + lines[len(lines)-1]
	}
	return advice
}

func stateWords(state string) string {
	switch state {
	case "waiting-network":
		return "waiting for the network"
	case "waiting-time":
		return "waiting for the time"
	case "awaiting-registration":
		return "waiting for its certificate"
	case "connecting":
		return "connecting"
	case "":
		return "not answering"
	default:
		return state
	}
}

// oneTimePassword makes the password the device enrolls with: 24
// characters from the system's random source.
func oneTimePassword() (string, error) {
	const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
	b := make([]byte, 24)
	for i := range b {
		n, err := rand.Int(rand.Reader, big.NewInt(int64(len(alphabet))))
		if err != nil {
			return "", err
		}
		b[i] = alphabet[n.Int64()]
	}
	return string(b), nil
}
