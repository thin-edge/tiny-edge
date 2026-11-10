## Why

Getting a new tiny-edge device onto Wi-Fi and into Cumulocity takes too many
steps today. Someone has to put the Wi-Fi credentials into the build or a
provisioner image, then either open the registration URL the device prints
or register the device beforehand and pass its one-time password in. The
Lab Provisioner (lab-ztp-provisioner) automates this for a fleet, but it is
a server to run and its own development effort, which is too much for one
board on a desk.

A developer with the board on a USB cable already has two things the device
needs: the computer is on the Wi-Fi network the device should join, and
go-c8y-cli (`c8y`) is logged in to the tenant the device should report to.
A small tool can hand both to the device in about a minute, with no browser
and nothing typed into a build.

## What Changes

- **A host tool, `tedge-onboard`**, in `tools/onboard/` (Go, one binary per
  platform), written for users who are not technical. Run with no
  arguments, it guides the user through a few plain-language steps: it
  finds the board by asking each USB port (offering a list when there are
  several), proposes the computer's current Wi-Fi network and takes its
  password from the OS credential store with the user's approval, helps the
  user pick or log in to a go-c8y-cli session, registers the device with
  `c8y deviceregistration register-ca` and a one-time password it
  generates, sends the Wi-Fi and the tenant and password to the device, and
  waits until the device shows as connected in Cumulocity. It never asks
  for a port, an ID or a URL; where it cannot decide, it offers choices.
  Flags remain for scripts.
- **A device-side provisioning library**, `provisioning/` at the repository
  root, beside the client and not in it. It speaks the Improv Wi-Fi RPC
  protocol, plus one tiny-edge command for the tenant and one-time password,
  and passes what it receives to the application (Wi-Fi) and to the client
  through the existing public API (`tedge_set_c8y_url()`,
  `tedge_set_enroll_otp()`).
- **Transports**: Improv Serial first, over the board's USB console. Improv
  BLE (a GATT service) later, on the same RPC layer, so the tool and a phone
  can onboard a device without a cable. The Improv RPC commands are the same
  on both transports.
- **Port glue** for the serial transport: the Zephyr port feeds the library
  from its console. Other ports add theirs when they add the library.
- **A sample** that uses the library: Zephyr's `samples/minimal` gains an
  onboarding overlay.

The library is optional and off by default. Nothing changes for an
application that does not enable it.

### Build options added

| Option | Default | What it adds |
|---|---|---|
| `TEDGE_PROVISIONING` | off | The Improv RPC layer and the tiny-edge command |
| `TEDGE_PROVISIONING_SERIAL` | on with `TEDGE_PROVISIONING` | Improv Serial over the console |
| `TEDGE_PROVISIONING_BLE` | off (phase 2) | Improv over a BLE GATT service |

Measured on Zephyr's `samples/minimal` with the minimal profile and the
onboarding overlay, against the same build with `TEDGE_PROVISIONING=n`
(2026-11-10):

| Board | Flash | Static RAM |
|---|---|---|
| ESP32-S3-DevKitC | +5.0 KB | +5.2 KB (4 KB of it the worker's stack) |
| ESP32-C6-DevKitC | +5.8 KB | +0.5 KB, plus the 4 KB stack in `.noinit` |

The ESP32-WROOM-32 is not measured: the minimal sample has no board file
for it. The overlay itself also turns on the shell and Wi-Fi credential
storage, which an application may already have; those are the
application's cost, not the library's. BLE costs what the Bluetooth stack
costs, which is why it is a separate option and a separate phase.

### Interface changes

- **Public `tedge_*` API**: two read-only getters,
  `tedge_get_device_id()` and `tedge_get_c8y_url()`, which the library
  needs for its info command and which any application can use.
  `tedge_stop()` now returns -EBUSY when the client thread does not end in
  time, instead of reporting success. The library has a public header of
  its own, `include/tedge/provisioning.h`, through which the application
  supplies the Wi-Fi and identify hooks and the port connects a transport.
- **Port interface**: none. The serial transport is fed by the port's glue
  code, outside the port interface.
- **Messages the device sends to Cumulocity**: none.
- **Messages on the console**: the device answers Improv frames on the
  console when the library is enabled. Improv frames start with `IMPROV` and
  a version byte, and the tool ignores any other output, so log lines and
  Improv frames can share the console.

### Where the change is

The library is portable C outside `core/`, under the same rules as `core/`
(no RTOS API). The serial glue is in `ports/zephyr/`. The tool is in
`tools/onboard/`. `core/` does not change.

## Non-goals

- Fleet provisioning. The Lab Provisioner stays the path for many devices
  at once, with its own trust model.
- Replacing the registration URL. A device without the library still prints
  it, and an operator can still open it.
- Provisioning over the network (SoftAP, a web page on the device).
- Bootstrap authentication. The tool onboards devices that use the
  Cumulocity CA. A bootstrap device needs only Wi-Fi, which the tool can
  still send.
- Device-specific settings beyond Wi-Fi and Cumulocity (parameters, remote
  access policy). Those are set from Cumulocity once the device is connected.
- Changing the existing Improv BLE provisioner image in
  microcontroller-devices. It can move to the library later.

## Capabilities

### New Capabilities

- `tedge-provisioning`: the device-side provisioning library: the Improv RPC
  commands it answers, the tiny-edge extension command, the transports, how
  it hands Wi-Fi to the application and onboarding data to the client, and
  what it refuses.
- `tedge-onboard-tool`: the host tool: how it finds the device, where it
  takes the Wi-Fi credentials from and with what consent, how it uses `c8y`
  to register the device, what it waits for, and how it reports failure.

### Modified Capabilities

None. `tedge-c8y-onboarding` already requires that the client stays unaware
of how the application obtained the one-time password and depends on no
provisioning transport. This change keeps that requirement: the library
sits outside the client and uses the public API.

## Impact

- New directories: `provisioning/` (library), `tools/onboard/` (Go module),
  `include/tedge/provisioning.h`.
- `ports/zephyr/`: Kconfig options, CMake for the library, console glue,
  and a sample overlay.
- `scripts/check-portability.py`: applies the core rules to `provisioning/`.
- CI: builds and tests the tool (Go), and the library's frame parser on the
  host.
- Dependencies: the tool needs go-c8y-cli on the PATH, logged in to the
  tenant. It uses a Go serial library, and in phase 2 a Go BLE library.
- Releases: the tool's binaries are attached to each tiny-edge release.
