## ADDED Requirements

### Requirement: tiny-edge builds as a NuttX application

`ports/nuttx/` SHALL build tiny-edge as an application that nuttx-apps
includes as an external directory, with NuttX's Make build and, where
NuttX offers one for the board, its CMake build. It SHALL compile the core,
the provisioning library, the POSIX port's sources, and `ports/common/`
from the repository, not copies of them.

#### Scenario: Build for a supported board

- **WHEN** nuttx-apps includes `ports/nuttx/` and `esp32s3-devkit` or
  `esp32c6-devkitc` is configured with the port's configuration
- **THEN** NuttX builds an image with the tiny-edge app, from the same core
  sources the Zephyr and FreeRTOS ports build

#### Scenario: CI builds it

- **WHEN** a pull request changes `core/`, `provisioning/`, `ports/posix/`,
  `ports/common/`, or `ports/nuttx/`
- **THEN** CI builds the app for the ESP32-S3 and the ESP32-C6 with every
  feature, and for one board with a reduced feature set, and fails the pull
  request if a build fails

#### Scenario: A changed configuration

- **WHEN** the build script builds with a configuration that differs from
  the last build's
- **THEN** it rebuilds every object, so no object built for another
  configuration is linked in

### Requirement: Features are selected with the other ports' options

Each feature SHALL have the `CONFIG_TEDGE_*` Kconfig option of the same
name and default as on Zephyr and ESP-IDF: telemetry, health, restart,
firmware update, shell command, log upload, remote access, parameters,
certificate renewal, and set-up over USB, and the authentication and MQTT
endpoint choices. The device SHALL advertise only the operations of the
features built in.

#### Scenario: A reduced build

- **WHEN** the app is built with firmware update, shell command, log
  upload, and remote access off
- **THEN** it builds, connects, and advertises neither firmware update,
  shell command, log upload, nor remote access

### Requirement: The core and other ports stay free of NuttX code

The port SHALL NOT change `core/`, `provisioning/`, or the port interface.
Code specific to NuttX SHALL be only in `ports/nuttx/`. A change to
`ports/posix/` SHALL be an option that any POSIX system can use, named for
what it does, and SHALL keep today's behavior when it is not set.

#### Scenario: A NuttX difference in the POSIX port

- **WHEN** the POSIX port uses a call that NuttX lacks or implements
  differently
- **THEN** the POSIX port gains an option for the alternative, the NuttX
  port sets it, and the POSIX tests on Linux and macOS pass with it on and
  off

### Requirement: The device connects to Cumulocity and reports itself

The device SHALL enroll with the Cumulocity CA or use bootstrap
credentials, connect over mTLS to the MQTT Service, and send the same
messages as on the other ports. `tedge_OS` SHALL report `nuttx` and its
release, and `c8y_Hardware` SHALL report the chip and the Wi-Fi MAC
address. Threads SHALL get the stack sizes the core asks for.

#### Scenario: First connection

- **WHEN** a set-up device starts with a network
- **THEN** it sets its clock from the network, connects to Cumulocity, its
  twin shows `c8y_Agent`, `tedge_OS`, and `c8y_Hardware`, and the `device`
  measurement arrives every interval

#### Scenario: A long session

- **WHEN** the device runs for 30 minutes with telemetry and a remote access
  tunnel, with no program on the computer reading its USB port
- **THEN** no thread overflows its stack, the free heap stays steady, and
  the device keeps answering operations

### Requirement: Device management works as on the other ports

The device SHALL restart from the cloud with a board reset, keep its
settings and key on LittleFS, apply and keep parameters, upload logs, open
remote access tunnels, renew its certificate, and report an operation
interrupted by a reset as failed after the reboot.

#### Scenario: Restart from the cloud

- **WHEN** Cumulocity sends a restart operation
- **THEN** the board resets, joins the network again, connects with the
  stored certificate, and reports the operation as successful

#### Scenario: Parameters persist

- **WHEN** a parameter set changes from Cumulocity and the board restarts
- **THEN** the board reports the changed values after the restart

### Requirement: Shell commands run NSH commands

With `CONFIG_TEDGE_SHELL_COMMAND`, the device SHALL run a `c8y_Command`
that its allow-list permits as an NSH command, return its output up to the
configured limit, and fail the operation when the command is not allowed
or runs longer than the timeout.

#### Scenario: An allowed command

- **WHEN** Cumulocity sends `c8y_Command` with `free`, which the allow-list
  permits
- **THEN** the operation succeeds with the command's output as its result

#### Scenario: A command not allowed

- **WHEN** Cumulocity sends a command the allow-list does not permit
- **THEN** the operation fails with a reason that says the command is not
  allowed, and nothing runs

### Requirement: Firmware updates go through MCUboot

The port SHALL implement `firmware.h` on NuttX's MCUboot support: write
the new image to the secondary slot, boot it on trial, confirm it once the
core says the image has proven itself, and report the running image's
version. A reset before confirmation SHALL bring the previous image back.
The build SHALL fail when the image does not fit its slot.

#### Scenario: A successful update

- **WHEN** Cumulocity installs a newer signed image on the device
- **THEN** the device writes it, restarts into it, connects, confirms it,
  and reports the new version and a successful operation

#### Scenario: An image that does not confirm

- **WHEN** the new image cannot connect within the core's confirmation time
- **THEN** the device restarts into the previous image and reports the
  operation as failed

### Requirement: tedge-onboard sets the device up over USB

The port SHALL run Improv Serial on the board's USB serial console, apply
the Wi-Fi network it receives and store it, list the networks the device
sees when asked, and go back to the stored network when a new one cannot be
joined, so `tedge-onboard` sets up a NuttX device with no change to the
tool. Log lines on the console SHALL NOT break Improv frames, and SHALL NOT
stall the device when no program reads the port.

#### Scenario: Set-up of a new device

- **WHEN** a user runs `tedge-onboard` with a NuttX board on USB that has a
  cleared data partition
- **THEN** the tool finds the board, lists the networks the board sees,
  gives it the chosen network, the tenant, and a one-time password, and the
  device connects with the device type its firmware reports

#### Scenario: A wrong password

- **WHEN** the tool gives the board a network it cannot join
- **THEN** the board reports that it could not join, and joins its stored
  network again

### Requirement: The port is documented and released like the others

`ports/nuttx/README.md` SHALL describe, as the other ports' READMEs do,
how to add the port to an application, its options, how each port
interface maps to NuttX, how to set a device up, its status per board, and
what is not in the port yet. The top-level README SHALL list the port. The
sample agent SHALL be released as a device image for the ESP32-C6 from
thin-edge/microcontroller-devices, with its own device type and a firmware
entry for Cumulocity.

#### Scenario: A user picks the NuttX image

- **WHEN** a user downloads the NuttX agent image from a
  microcontroller-devices release and flashes it
- **THEN** `tedge-onboard` sets the device up, and Cumulocity offers later
  releases of the same image as firmware updates for that device type
