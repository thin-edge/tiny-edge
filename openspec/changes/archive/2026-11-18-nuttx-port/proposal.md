## Why

tiny-edge has two supported ports for microcontrollers, Zephyr and
FreeRTOS on ESP-IDF, and a POSIX port for Linux and macOS. Apache NuttX is
a POSIX RTOS that runs on the same ESP32 chips, and teams that build on
NuttX need the same client the other ports offer.

A first stage of this change was a trial: tiny-edge built as a NuttX
application from the core, the POSIX port's sources, and `ports/common/`,
with only small generic options added to the POSIX port. On an ESP32-C6
the device was set up with `tedge-onboard`, connected over mTLS, sent
telemetry, uploaded logs, and restarted from the cloud. The trial showed
that the reuse works (`ports/nuttx/REPORT.md`). This change now makes NuttX
a supported port, on par with the Zephyr and ESP-IDF ports: every feature
those ports offer, the same set-up experience, CI builds, documentation,
and a released device image.

## What Changes

- **A supported NuttX port, `ports/nuttx/`**: a nuttx-apps external
  application, built with NuttX's Make and CMake builds. It compiles the
  core, the POSIX port's sources, and `ports/common/` unchanged, and adds
  only what NuttX needs of its own.
- **Every feature the other ports offer**, each with its own
  `CONFIG_TEDGE_*` option under the same name as on Zephyr and ESP-IDF:
  telemetry, the health measurement, restart, firmware update, shell
  command, log upload, remote access, parameters, and certificate renewal,
  with both authentication methods (Cumulocity CA and bootstrap) and both
  MQTT endpoints.
- **Firmware update through MCUboot**: NuttX's MCUboot support for the ESP
  chips. The new image is written to the secondary slot, booted on trial,
  confirmed once connected, and rolled back if it never confirms.
- **Shell command (`c8y_Command`)**: NSH commands run through NuttX's
  `popen()`, with an allow-list, a timeout, and an output limit, as on
  ESP-IDF.
- **Crash dumps**, if NuttX's core dump to flash works on the ESP chips:
  offered as a log type, as on Zephyr. If it does not, the README lists it
  under "not in this port yet", as the ESP-IDF port does.
- **Set-up over USB on par with ESP-IDF**: Improv Serial on the console;
  the Wi-Fi scan that shows the networks the device sees; going back to
  the stored network, with retries, when a new one fails.
- **The sample agent becomes a released image**: the same device agent as
  the ESP-IDF one, for the ESP32-C6 first, with a device type of its own,
  released from thin-edge/microcontroller-devices with the other images.
- **Fixes to NuttX the port depends on**, applied by the build script and
  offered upstream: the ESP USB serial driver's 64-byte receive buffer, and
  its unbounded wait when no host reads the port.
- **Generic options in the POSIX port**, kept from the trial: thread stack
  sizes from the core, the MAC address through `SIOCGIFHWADDR`, a restart
  callback, and no `exec()`.
- **CI**: the app built for the ESP32-S3 and ESP32-C6 with every feature,
  and once with a reduced set, as for ESP-IDF.
- **Documentation**: `ports/nuttx/README.md` in the style of the other
  ports' READMEs, and NuttX in the top-level README's port table.

## Non-goals

- No change to `core/`, `provisioning/`, or the port interface. A need for
  one is proposed separately.
- No NuttX-specific code outside `ports/nuttx/`. The POSIX port gains
  generic options only.
- No Improv over BLE. Neither ESP-IDF nor this port has it.
- No boards other than the ESP32 chips: the ESP32-S3 and ESP32-C6 first;
  the ESP32-C3 if the TLS memory fits.
- No gateway transport until it is done on the other ports
  (`openspec/changes/gateway-transport`).

## Capabilities

### New Capabilities

- `tedge-nuttx-port`: tiny-edge as a NuttX application: how it is built and
  configured, what each port interface maps to on NuttX, firmware update
  through MCUboot, shell commands, set-up over USB, the released image, and
  the limits on what the port may change outside `ports/nuttx/`.

### Modified Capabilities

None. The port implements the existing requirements of the client
(`device-management-features`, `tedge-firmware-update`, and the others) on
one more RTOS. The POSIX port's new options keep today's behavior when
they are not set.

## Impact

- **Where:** `ports/nuttx/`, small additions to `ports/posix/`, a CI
  workflow, and the top-level README. Nothing in `core/` or
  `provisioning/`.
- **Public `tedge_*` API:** unchanged.
- **Port interface (`include/tedge/port/`):** unchanged.
- **POSIX port API (`tedge/posix.h`):** an optional `restart` field in
  `struct tedge_posix_config`.
- **Messages the device sends:** unchanged. `tedge_OS` reports `nuttx` and
  its release; `c8y_Hardware` reports the chip and the MAC address.
- **Dependencies:** NuttX and nuttx-apps pinned at `nuttx-13.1.0`,
  mbedTLS 4.1 built by the app, MCUboot from NuttX, and the Espressif
  toolchains, in `ports/nuttx/docker/`.
- **Other repository:** thin-edge/microcontroller-devices gains the NuttX
  agent image in its release, in a separate pull request.
- **Boards:** an ESP32-C6-DevKitC (on hand) and an ESP32-S3-DevKitC.
