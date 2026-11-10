## ADDED Requirements

### Requirement: Provisioning is an optional library outside the client

tiny-edge SHALL provide device-side provisioning as a library separate from
the client, enabled with the build option `TEDGE_PROVISIONING` and off by
default. The library SHALL use only the public `tedge_*` API and the port
interface, and SHALL NOT use an RTOS API directly. The client SHALL NOT
depend on the library. A build without the option SHALL contain none of the
library's code.

#### Scenario: Build without the option

- **WHEN** an application is built without `TEDGE_PROVISIONING`
- **THEN** the image contains no provisioning code, and the client behaves as
  before

#### Scenario: Portability check

- **WHEN** `scripts/check-portability.py` runs
- **THEN** it applies the rules for `core/` to `provisioning/` as well

### Requirement: The library answers Improv Wi-Fi RPC commands

The library SHALL answer the Improv Wi-Fi RPC commands "send Wi-Fi
settings" (0x01) on every transport, "request current state" (0x02) and
"request device info" (0x03) on the serial transport, and "identify" (0x02)
on the BLE transport. It SHALL validate each RPC packet's length and
checksum, answer an invalid packet with the "invalid RPC" error (0x01), and
an unknown command with the "unknown RPC" error (0x02). The device info
SHALL report the firmware name and version, the chip, and the external ID as
the device name.

#### Scenario: Standard Improv client sends Wi-Fi settings

- **WHEN** an Improv client sends "send Wi-Fi settings" with an SSID and
  password
- **THEN** the library reports the "provisioning" state, calls the
  application's Wi-Fi hook with them, and reports "provisioned" once the
  network is up, or the "unable to connect" error (0x03) if it is not up
  within 30 seconds

#### Scenario: Corrupted packet

- **WHEN** a packet arrives whose checksum does not match
- **THEN** the library answers with the "invalid RPC" error and changes
  nothing

#### Scenario: Device info

- **WHEN** a serial client sends "request device info"
- **THEN** the library answers with the firmware name, firmware version,
  chip, and the device's external ID

### Requirement: The board reports the networks it can see

The library SHALL answer Improv's "request scanned Wi-Fi networks" command
(0x04) through the application's scan hook: one RPC result per network,
carrying the SSID, the signal strength in dBm and whether it needs a
password, strongest first, each SSID once and without hidden networks,
followed by a result with no strings. Without the hook, it SHALL answer
with the "unknown RPC" error.

#### Scenario: Two access points of one network

- **WHEN** the scan finds the same SSID on two access points and a hidden
  network
- **THEN** the SSID is reported once, with the stronger signal, and the
  hidden network is not reported

### Requirement: Joining Wi-Fi stays the application's job

The library SHALL hand Wi-Fi settings to the application through a hook
given in `tedge_provisioning_init()`, and SHALL NOT join a network or store
Wi-Fi credentials itself. It SHALL judge success by the network coming up
(`tedge_port_net_is_up()`). An application without the hook SHALL get the
"unable to connect" error for every Wi-Fi settings command.

#### Scenario: Application stores and joins the network

- **WHEN** the library calls the Wi-Fi hook with an SSID and password
- **THEN** the application joins the network and stores the credentials in
  its own way, and the library reports "provisioned" when the network is up

### Requirement: The Zephyr port provides default hooks

The Zephyr port SHALL provide `tedge_provisioning_zephyr_init()`, which
starts the library with the port's hooks: Wi-Fi through Zephyr's Wi-Fi
credentials store, the scan, identify on the devicetree's `led0` or
`led-strip`, and `CONFIG_SOC` as the chip. Each hook the application passes
SHALL replace the port's. The port's Wi-Fi hook SHALL store a network only
once the device has an address on it, and SHALL rejoin the previously
stored network when the new one cannot be joined.

#### Scenario: Application with its own network management

- **WHEN** an application passes its own Wi-Fi hook to
  `tedge_provisioning_zephyr_init()`
- **THEN** the library uses that hook, and the port's scan, identify and
  chip

#### Scenario: Wrong password with a network already stored

- **WHEN** the port's Wi-Fi hook cannot join the new network
- **THEN** the stored network is unchanged, and the device joins it again

### Requirement: The tiny-edge info command reports what onboarding needs

The library SHALL answer a tiny-edge info command (0xE0) with the
provisioning protocol version, the external ID, the authentication mode
(`c8y-ca` or `bootstrap`), the tenant host if one is set, and the client's
state. It SHALL NOT include any password or credential in the reply.

#### Scenario: Unregistered device

- **WHEN** a tool sends the info command to a device waiting for
  registration
- **THEN** the reply carries the external ID, `c8y-ca`, the tenant if set,
  and the state "awaiting-registration"

### Requirement: The tiny-edge onboard command hands the tenant and password to the client

The library SHALL accept a tiny-edge onboard command (0xE1) carrying a
tenant host, a one-time password, and a flags byte. It SHALL validate both
strings as the client's setters do, and SHALL apply them through
`tedge_set_c8y_url()` and `tedge_set_enroll_otp()`. When the client is
running, it SHALL stop it with `tedge_stop()` first and start it again with
`tedge_start()` afterwards. It SHALL refuse the command, with the "not
authorized" error (0x04), when the client is already connected and the
flags byte does not have its re-onboard bit set. It SHALL NOT log the
one-time password above debug level.

#### Scenario: Onboard a device waiting for registration

- **WHEN** a tool sends the onboard command to a device in the
  awaiting-registration state
- **THEN** the library sets the tenant and one-time password, restarts the
  client, and answers with an empty result; the client enrolls with that
  password

#### Scenario: Connected device without the re-onboard flag

- **WHEN** the onboard command arrives while the client is connected and
  the re-onboard bit is clear
- **THEN** the library answers with the "not authorized" error and the
  client keeps its certificate

#### Scenario: Bootstrap build

- **WHEN** the onboard command arrives on a build with bootstrap
  authentication
- **THEN** the library answers with the "unknown RPC" error, since there is
  no one-time password to set

### Requirement: The device can identify itself on request

The library SHALL accept a tiny-edge identify command (0xE2), so a tool
can tell several connected boards apart. It SHALL call the application's
identify hook when there is one (to blink an LED, for example) for about
10 seconds, and SHALL log a line naming the device in any case. It SHALL
answer the command with an empty result, with or without a hook. On BLE,
the Improv identify command (0x02) SHALL do the same.

#### Scenario: Two boards on one computer

- **WHEN** a tool sends the identify command to one of two boards
- **THEN** that board blinks (when the application has a hook) and logs that
  it was asked to identify itself, and the other board does nothing

### Requirement: Improv Serial on the console

With `TEDGE_PROVISIONING_SERIAL`, the library SHALL speak Improv Serial
(header `IMPROV`, version 1) on the console the port provides. It SHALL
ignore any bytes outside a valid frame, and SHALL write each of its own
frames to the console as one unit, so log output never splits a frame. On a
port whose console is a shell, the port SHALL provide a shell command,
`tedge improv`, that hands the console to the library until onboarding
finishes or no valid frame arrives for 60 seconds, then returns it to the
shell.

#### Scenario: Log output between frames

- **WHEN** the device logs lines while a tool exchanges frames with it
- **THEN** the tool receives every frame intact, with the log lines between
  them

#### Scenario: Shell console

- **WHEN** a tool sends `tedge improv` followed by Improv frames on a Zephyr
  console with the shell
- **THEN** the library answers the frames, and the shell prompt returns once
  onboarding finishes or after 60 seconds without a valid frame

### Requirement: Improv BLE on the same RPC layer

With `TEDGE_PROVISIONING_BLE`, the port SHALL expose the Improv Wi-Fi GATT
service and pass its RPC packets to the same RPC layer as the serial
transport. The commands and their results SHALL be the same on both
transports, apart from command 0x02.

#### Scenario: Onboard over BLE

- **WHEN** a BLE client sends the Wi-Fi settings and then the tiny-edge
  onboard command
- **THEN** the device joins the network and enrolls exactly as it would over
  serial
