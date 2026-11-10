## Context

Onboarding a tiny-edge device needs three things on the device: Wi-Fi
credentials, the tenant host, and a one-time password that the tenant knows
for the device's external ID. Today they come from the build (Kconfig), the
application, or the Lab Provisioner, which hands them over in an encrypted
bundle after the device joins a provisioning network.

The client deliberately knows nothing about how they arrive
(`tedge-c8y-onboarding`: "The client SHALL remain unaware of how the
application obtained the password"). It offers `tedge_set_c8y_url()` and
`tedge_set_enroll_otp()`, both called before `tedge_start()`.
Wi-Fi is the application's job on every port.

microcontroller-devices already ships an Improv Wi-Fi BLE provisioner, as a
separate MCUboot image (`apps/wifi-provisioner/src/improv.c`). It answers
the Improv RPC "send Wi-Fi settings" and "identify" over a GATT service, and
it does not hand anything to the client.

The developer at a desk has the board on USB, the computer on the Wi-Fi the
board should join, and go-c8y-cli logged in to the tenant.

## Goals / Non-Goals

**Goals:**

- Onboard one board from a computer with one command and no browser, in
  about a minute.
- Reuse the computer's Wi-Fi network and go-c8y-cli session, with the
  user's consent for the Wi-Fi password.
- One device-side RPC layer for every transport (serial now, BLE next) and
  every port.
- Keep the client (`core/`) unchanged and unaware of provisioning.
- Stay compatible with standard Improv clients for Wi-Fi, so ESP Web Tools
  and Improv phone apps can at least provision Wi-Fi.

**Non-Goals:**

- Fleet provisioning, trust bootstrapping without a person present, and
  anything the Lab Provisioner does.
- Provisioning over Wi-Fi (SoftAP, a web page on the device).
- Configuring the device beyond Wi-Fi and Cumulocity.

## Decisions

### 1. Improv Wi-Fi as the protocol, with two tiny-edge commands

Improv is an open protocol with a serial and a BLE transport that share
their RPC commands. It is already used by this project (the BLE
provisioner), by ESP Web Tools, and by phone apps. Its RPC packet is
`[command][length][data][checksum]`, the checksum being the low byte of
the sum of the preceding bytes. Strings are length-prefixed.

The library answers:

| Command | Name | Transport | Notes |
|---|---|---|---|
| 0x01 | Send Wi-Fi settings | both | Improv standard |
| 0x02 | Request current state | serial | Improv standard (0x02 is "identify" on BLE) |
| 0x02 | Identify | BLE | Improv standard; the application's hook, if any |
| 0x03 | Request device info | serial | Improv standard: firmware name and version, chip, device name |
| 0xE0 | tiny-edge info | both | External ID, auth mode, tenant if set, client state |
| 0xE1 | tiny-edge onboard | both | Tenant host and one-time password |
| 0xE2 | tiny-edge identify | serial | Blink or log, to tell boards apart (BLE uses 0x02) |

The tiny-edge commands are numbered high, away from the commands Improv
has assigned (0x01 to 0x05 so far). A standard Improv client never sends
them; a device without the library answers "unknown RPC" (0x02 error), which
the tool reports as "this firmware does not support tiny-edge onboarding".

*Alternatives*: a line protocol on the shell (`tedge onboard <url> <otp>`)
is simpler to type but has no framing, no checksum, and would need parsing
of free-form output; it also does nothing for BLE. A protocol of our own
(CBOR, protobuf) costs a parser per side for no gain over Improv's
length-prefixed strings.

### 2. The library sits beside the client, not in it

`provisioning/` is a library at the repository root with its own public
header (`include/tedge/provisioning.h`). It is portable C under the same
rules as `core/` (checked by `check-portability.py`): it uses the public
`tedge_*` API and the port interface, never an RTOS API. It owns the RPC
layer and the Improv state machine, and calls:

- the application's Wi-Fi hook (`connect(ssid, password)`), because joining
  and storing Wi-Fi is the application's job;
- `tedge_set_c8y_url()` and `tedge_set_enroll_otp()` for the onboarding
  command.

Both setters must be called before `tedge_start()`. When the client is
already running (it usually is, waiting for registration), the library
stops it with `tedge_stop()`, applies the values, and starts it again with
`tedge_start()`. The public API is unchanged; a task verifies that a
stop-start cycle is clean on every port.

*Alternatives*: putting Improv in `core/` would break the
`tedge-c8y-onboarding` requirement and make every build carry it. Leaving it
in microcontroller-devices would leave every other tiny-edge user to write
it again.

### 3. Transports are thin and port-supplied

The RPC layer takes complete RPC packets and returns result packets. Each
transport wraps them:

- **Serial**: Improv Serial framing (`IMPROV`, version 1, type, length,
  payload, checksum). The framing code is portable and part of the library;
  the port supplies "bytes arrived" and "write these bytes as one unit".
- **BLE** (phase 2): the Improv GATT service. The GATT table and
  advertising are RTOS code, so they go in the port.

On Zephyr, the console is usually the shell's UART. The glue uses the
shell's bypass mode: a shell command, `tedge improv`, hands the console to
the library until onboarding finishes or nothing valid arrives for 60
seconds, then gives it back. The tool sends that command first; without a
shell the line is plain bytes that the frame parser skips. A build without
the shell feeds the library from the console UART directly, which also makes
the device answer ESP Web Tools with no command first.

*Alternatives*: scanning the shell's input for `IMPROV` would need a hook
the Zephyr shell does not have. A second UART would need a second cable.

### 4. The tool is Go and drives go-c8y-cli

`tools/onboard/` is a Go module producing one static binary per platform.
Go matches go-c8y-cli, has serial (`go.bug.st/serial`) and BLE
(`tinygo.org/x/bluetooth`) libraries for macOS, Linux and Windows, and
needs no runtime on the user's computer.

The tool runs `c8y` as a subprocess rather than calling the REST API
itself, so it uses whatever session the user has active (including
encrypted sessions and SSO tokens), and needs no credentials of its own.
The calls are:

1. `c8y currenttenant get` to learn the tenant host and check the session.
2. `c8y identity get --name <external ID>` to find an existing device.
3. `c8y deviceregistration register-ca --id <external ID>
   --one-time-password <password> --force`.
4. `c8y identity get` again until the device exists, then
   `c8y inventory get` until it reports availability.

The one-time password is 24 characters from `[A-Za-z0-9]`, from the OS
random source. It is passed to `c8y` as an argument and to the device over
the serial line, and is never printed.

*Alternatives*: Python with pyserial is quicker to write but needs a Python
environment; a go-c8y-cli extension in shell cannot open a serial port
portably. The tool can be packaged as a go-c8y-cli extension later, since
extensions may be binaries.

### 5. Wi-Fi credentials come from the OS, with consent

| OS | Network | Password |
|---|---|---|
| macOS | `ipconfig getsummary <if>` | `security find-generic-password -D "AirPort network password" -a <SSID> -w` (the Keychain asks the user) |
| Linux | `nmcli -t -f active,ssid dev wifi` | `nmcli -s -g 802-11-wireless-security.psk connection show <name>` |
| Windows | `netsh wlan show interfaces` | `netsh wlan show profile name=<SSID> key=clear` |

The tool proposes the current network and lets the user pick another the
computer knows. Where the OS will not give the password, it asks for it
with hidden input. The password is never printed or logged. For scripts,
`--wifi-ssid`, `--wifi-password-stdin` and `--skip-wifi` replace the
questions.

### 6. The tool finds the device by asking each port

It lists serial ports with their USB vendor IDs, keeps those of known
USB-serial chips (Espressif USB-Serial-JTAG `303A`, WCH `1A86`, Silicon Labs
`10C4`, FTDI `0403`), opens each with DTR and RTS low so no board resets,
and asks it for its tiny-edge info. A port that answers is a candidate,
described by what it reports (model, firmware, whether it is in Cumulocity)
rather than by its device path. One candidate is used directly; several are
offered as a list, with "make this board blink" to tell them apart; none
means waiting for the user to plug one in. Asking is what makes the choice
safe: a port that does not answer is never sent credentials.

### 7. Guided by default, scriptable on request

The tool is written for someone who has never used a terminal. With no
arguments it runs as a short guided conversation: each step shows a
spinner and then a one-line result, and the only input is choosing from a
list (with the likely answer selected) or confirming once. It never asks
the user to type a port, an ID, a URL or a password it can find. Messages
avoid protocol terms ("board ID" for external ID, "set up" for onboard),
and every failure says what to do next and offers to retry instead of
exiting.

The prompts use `charmbracelet/huh`, which draws lists and hidden password
inputs on macOS, Linux and Windows terminals and has an accessible mode for
screen readers. Logins to Cumulocity stay go-c8y-cli's: the tool runs
`c8y sessions login` or the session picker in the same terminal and picks
up from there.

Started by double-click, the tool runs in a terminal window that closes
when it exits, so it waits for Enter before exiting.

For scripts and CI, `--yes` turns every question into a failure with a
message, and `--json` prints the result.

*Alternatives*: a graphical app would be friendlier still, but a
cross-platform GUI with serial and Keychain access is a much larger
project; the guided terminal flow covers the need first, and its steps can
sit behind a GUI later. A browser flow (Web Serial) cannot read the
computer's Wi-Fi password or use go-c8y-cli.

## Risks / Trade-offs

- **Log lines can interleave with a frame** on a shared console → the
  device writes each frame as one unit through the transport's write call
  (on Zephyr, under the shell's output lock), and the tool skips any bytes
  between frames and rejects frames with a bad checksum.
- **macOS hides the SSID** from processes without Location Services on
  recent releases → the tool offers the networks the computer knows
  (`networksetup -listpreferredwirelessnetworks`) as a list to choose from.
- **Probing ports sends bytes to boards that are not ours** → the probe is
  one short line and one Improv frame, both ignored by firmware that does not
  speak Improv, and nothing else is sent to a port that does not answer.
- **Anyone with the USB cable can re-onboard the device** → physical access
  is the trust boundary, as it is for flashing. The onboard command on a
  device that is already connected is refused unless the tool sends its
  re-onboard flag, which it does only after the user agrees (or with
  `--reonboard` in a script).
- **The one-time password re-onboards the device** (`tedge_set_enroll_otp()`
  discards the certificate) → the tool generates a fresh password each time
  and registers it before sending it, so the device never holds a password
  the tenant does not know.
- **Improv may assign 0xE0 or 0xE1 one day** → the commands are in one
  table on each side and can move; the tool checks the device's tiny-edge
  protocol version in the info reply.
- **BLE costs RAM the C6 and WROOM may not have** → it is a separate option
  in a later phase, and measured per board before it is offered there.
- **A stop-start cycle of the client may not be clean on every port** → a
  task tests it on Zephyr and POSIX before the library relies on it.

## Migration Plan

New and optional: nothing to migrate. microcontroller-devices can move its
Improv BLE provisioner onto the library in a later change.

## Open Questions

- Should the Improv "provisioned" result carry a URL? Improv lets the
  device return one after Wi-Fi is set. The device does not know its
  Cumulocity managed object ID, so the tool prints the device link instead;
  the device could return the tenant's Device Management URL.
- Release packaging for the tool: GitHub release assets built by a matrix
  job, or GoReleaser. Decided when the release workflow is touched.
