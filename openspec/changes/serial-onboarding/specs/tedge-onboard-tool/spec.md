## ADDED Requirements

### Requirement: The tool guides a non-technical user from start to finish

`tedge-onboard` SHALL need no arguments. Run on its own, it SHALL find the
device, the Wi-Fi network and the Cumulocity tenant by itself, and SHALL ask
the user only where it cannot decide, by offering a list of choices with the
likely one selected. It SHALL NOT ask the user to type a port, an ID or a
URL. Every message SHALL be in plain language, without terms such as
"serial port", "external ID", "one-time password" or "RPC", and every
problem SHALL say what the user can do next. Each step SHALL show progress
while it runs and a clear result when it ends.

#### Scenario: First use with one board

- **WHEN** a user with one board on USB runs `tedge-onboard` with no
  arguments
- **THEN** the tool finds the board, proposes the computer's Wi-Fi network
  and the user's tenant, asks the user to confirm once, onboards the device,
  and ends with the device's name and a link to open it in Cumulocity

#### Scenario: A problem the user can fix

- **WHEN** a step fails for a reason the user can fix (the board is not
  plugged in, Cumulocity is not logged in, the Wi-Fi password is wrong)
- **THEN** the tool says what went wrong and what to do in one or two plain
  sentences, and offers to try again rather than exiting

#### Scenario: Started by double-clicking

- **WHEN** the user starts the tool by double-clicking it, so it runs in a
  terminal window that closes when it exits
- **THEN** the tool waits for the user to press Enter before exiting, so the
  result stays readable

### Requirement: The board is found without the user naming it

The tool SHALL look at every serial port whose USB vendor ID belongs to a
supported USB-serial chip (Espressif `303A`, WCH `1A86`, Silicon Labs
`10C4`, FTDI `0403`), open each with DTR and RTS low so no board resets,
and ask each for its tiny-edge info. It SHALL use the one board that
answers. When several answer, it SHALL list them by what they are (for
example "ESP32-S3 running tedge-minimal 0.3.0, not yet in Cumulocity") and
let the user choose, with an option to make a board identify itself (blink
or log) when the firmware supports it. When none is found, it SHALL tell
the user to plug the board in, and keep looking until a board appears or
the user cancels. `--port` SHALL remain available for scripts.

#### Scenario: Two boards connected

- **WHEN** two boards with tiny-edge firmware are on USB
- **THEN** the tool lists both by model, firmware and Cumulocity status, and
  onboards the one the user picks

#### Scenario: Board plugged in after the start

- **WHEN** no board is connected when the tool starts
- **THEN** the tool asks the user to plug one in and continues by itself
  once it appears

#### Scenario: Board with other firmware

- **WHEN** a board answers no tiny-edge info request within 5 seconds, or
  answers that the command is unknown
- **THEN** the tool says the board's firmware cannot be set up this way and
  points to the firmware download, and sends it nothing

#### Scenario: Newer firmware than the tool

- **WHEN** the board's provisioning protocol version is newer than the
  tool's
- **THEN** the tool asks the user to download the latest tool, and sends
  nothing

### Requirement: Wi-Fi is taken from the computer, with consent

The tool SHALL propose the Wi-Fi network the computer is connected to, and
SHALL let the user choose another from the networks the computer knows or
type one. It SHALL take the password from the operating system's credential
store (the macOS Keychain, NetworkManager, or the Windows WLAN profiles),
which may ask the user to allow it. When the password is not available, it
SHALL ask for it with hidden input. It SHALL NOT print or log the password.
When the board reports that it could not join the network, the tool SHALL
say so and offer to choose the network or enter the password again. A user
who does not want to change the board's Wi-Fi SHALL be able to skip the
step. `--wifi-ssid`, `--wifi-password-stdin` and `--skip-wifi` SHALL remain
available for scripts.

#### Scenario: macOS with the Keychain

- **WHEN** the tool runs on macOS connected to Wi-Fi
- **THEN** it proposes that network, macOS asks the user to allow access to
  its password, and the tool sends it only after the user confirms

#### Scenario: The OS hides the current network

- **WHEN** the operating system does not reveal the current network's name
- **THEN** the tool lists the networks the computer knows for the user to
  choose from

#### Scenario: Wrong password

- **WHEN** the board reports that it could not connect to the network
- **THEN** the tool says the network or password was not accepted and asks
  again, without leaving the device half set up

### Requirement: Cumulocity is reached through go-c8y-cli, set up with the user

The tool SHALL use the `c8y` command (go-c8y-cli) with the user's session
for every call to Cumulocity, and SHALL hold no Cumulocity credentials
itself. When `c8y` is missing, it SHALL explain how to install it for the
user's operating system. When there is no active session, or more than one
to choose from, it SHALL let the user pick a session by tenant name or start
go-c8y-cli's own login, in the same terminal, and continue afterwards. It
SHALL show the tenant it will use and include it in the one confirmation.

#### Scenario: Not logged in

- **WHEN** `c8y` has no working session
- **THEN** the tool offers the user's saved sessions or a new login, runs
  go-c8y-cli's interactive login, and continues with that tenant

#### Scenario: go-c8y-cli missing

- **WHEN** `c8y` is not on the PATH
- **THEN** the tool explains how to install go-c8y-cli on this operating
  system, and offers to check again

### Requirement: The device gets a name and a fresh registration

The tool SHALL propose a device name for Cumulocity, defaulting to the
board's own ID, which the user may change. It SHALL generate a one-time
password of 24 characters from `[A-Za-z0-9]` with the operating system's
random source for every run, register it with
`c8y deviceregistration register-ca` for the board's ID and the chosen
name, and only then send it to the board. It SHALL NOT print the password.
When the board's ID already belongs to a device in the tenant, or the board
reports that it is connected, the tool SHALL say so by name and ask whether
to set the board up again (which gives it a new certificate), defaulting to
no. `--reonboard` and `--yes` SHALL answer these questions for scripts.

#### Scenario: Device already in Cumulocity

- **WHEN** the board's ID already belongs to a device in the tenant
- **THEN** the tool names that device and asks whether to set the board up
  again, and changes nothing unless the user agrees

#### Scenario: Setting a board up again

- **WHEN** the user agrees to set up a board that is already registered
- **THEN** the tool registers a new password, sends it with the re-onboard
  flag, and the device enrolls again with a new certificate

### Requirement: The tool confirms the device is connected

After sending the tenant and password, the tool SHALL wait until the board
reports that it is connected and Cumulocity shows the device as available,
for up to 3 minutes. It SHALL then show the device name and its Cumulocity
link, and offer to set up another board. It SHALL exit with status 0 only
when every device it set up is connected.

#### Scenario: Device connects

- **WHEN** the device enrolls and connects
- **THEN** the tool shows the device's name and link and asks whether to set
  up another board

#### Scenario: Device does not connect in time

- **WHEN** the device has not connected 3 minutes after the tenant and
  password were sent
- **THEN** the tool shows the last state the board reported, explains the
  likely causes in plain words, and offers to try again

### Requirement: Scripts can run the tool without questions

With `--yes`, the tool SHALL ask no questions: it SHALL use the detected or
given values, fail with a non-zero status and a message where it would have
asked, and print one line per step. `--json` SHALL print the result (device
name, ID, link, status) as JSON.

#### Scenario: Unattended run

- **WHEN** a script runs `tedge-onboard --yes --port <port> --skip-wifi`
- **THEN** the tool onboards the board without prompting, and exits with a
  non-zero status instead of prompting when something is missing
