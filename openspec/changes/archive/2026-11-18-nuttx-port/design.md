## Context

tiny-edge has three ports:

- **Zephyr** (`ports/zephyr/`): the first port and the most complete,
  including crash dumps.
- **FreeRTOS on ESP-IDF** (`ports/freertos/`): its own `os.h`, and the
  ESP-IDF services (net, kv, crypto, time, device, firmware, shell command,
  provisioning glue) in `ports/freertos/esp-idf/tiny_edge/`.
- **POSIX** (`ports/posix/`): for Linux and macOS hosts and tests, with no
  `firmware.h`, no shell command, and no crash dump.

`ports/common/` holds the BSD-socket and mbedTLS 4 code the POSIX and
ESP-IDF ports share.

The trial stage of this change (findings in `ports/nuttx/REPORT.md`)
built tiny-edge on NuttX 13.1 from the core, the POSIX port's sources, and
`ports/common/`, with four generic options in the POSIX port. On an
ESP32-C6 it was set up with `tedge-onboard`, connected, sent telemetry,
uploaded logs, and restarted from the cloud. It also found two defects in
NuttX's ESP USB serial driver and several start-up issues, all fixed in
`ports/nuttx/`.

What a supported port has, judged by the other two:

| | Zephyr | ESP-IDF | NuttX after this change |
|---|---|---|---|
| Feature options | Kconfig `CONFIG_TEDGE_*` | Kconfig `CONFIG_TEDGE_*` | Kconfig `CONFIG_TEDGE_*`, same names |
| Telemetry, health, restart, log upload, remote access, parameters, certificate renewal | Yes | Yes | Yes (POSIX port's code) |
| Firmware update | MCUboot | OTA partitions | MCUboot |
| Shell command | Zephyr shell | `esp_console` | NSH through `popen()` |
| Crash dumps | Yes | Not yet | If NuttX's core dump works (see below) |
| Set-up over USB, with Wi-Fi scan | Yes | Yes | Yes |
| CI | Board matrix | Three chips, every feature and a small set | Two chips, every feature and a small set |
| Released image | In microcontroller-devices | In microcontroller-devices | In microcontroller-devices |

## Goals / Non-Goals

**Goals:**

- Every feature of the other ports, selected with the same option names.
- Set-up, operation, and update of a device the same way as an ESP-IDF
  device, with the same tool.
- Reuse the POSIX port's sources and `ports/common/`, not copies.
- CI that keeps the port building, and documentation like the other
  ports'.

**Non-Goals:**

- Changes to `core/`, `provisioning/`, or the port interface.
- Improv over BLE, the gateway transport, or boards other than the ESP32
  chips.

## Decisions

### A nuttx-apps external app that compiles the POSIX port's sources

Kept from the trial. `ports/nuttx/` is an external app directory with
`Kconfig`, `Make.defs`, `Makefile`, and `CMakeLists.txt`. Its sources are
the core (`core/sources.cmake`), the provisioning library
(`provisioning/sources.cmake`), the POSIX port's `src/*.c`,
`ports/common/src/*.c`, mbedTLS 4.1, and `ports/nuttx/src/*.c`. Make's
source list is generated from the CMake lists at configure time, so both
builds compile the same files.

*Alternative:* copies of the POSIX files adapted to NuttX. They would
drift from the POSIX port, and the trial showed no need.

### The feature options are the other ports' options

The Kconfig menu uses `CONFIG_TEDGE_*` names and defaults from the ESP-IDF
component's Kconfig, so a feature is turned on or off the same way on every
port, and `tools/generate.cmake` maps them to the core's `TEDGE_*` macros.
The app's own settings (program name, state directory, interface, Wi-Fi
file) keep the `CONFIG_TINY_EDGE_*` prefix, which nuttx-apps uses for an
app's own options. Options for features this port lacks are absent, so a
build cannot select them.

### POSIX port options, chosen to fit any embedded POSIX system

Kept from the trial:

| Option | Why NuttX sets it |
|---|---|
| `TEDGE_POSIX_THREAD_STACKS` | NuttX's default thread stack is a few KB; threads get the core's sizes |
| `TEDGE_POSIX_MAC_IOCTL` | `getifaddrs()` lists no link-layer entries; the MAC comes from `SIOCGIFHWADDR` |
| `TEDGE_POSIX_NO_EXEC` and the `restart` field | A restart is a board reset, not `execv()` |

Each keeps today's behavior when it is off, and the POSIX tests run with
each on and off.

### Firmware update writes the MCUboot secondary slot through its MTD device

`ports/nuttx/src/tedge_port_fw.c` implements `firmware.h`:

- **Write:** open the secondary slot (`CONFIG_ESPRESSIF_OTA_SECONDARY_SLOT_DEVPATH`,
  `/dev/ota1`), erase it, and write the downloaded image in chunks.
- **Boot on trial:** `boot_set_pending(0)` from nuttx-apps' MCUboot
  library (`boot/mcuboot`), then the core's restart. MCUboot swaps the
  slots and boots the new image once.
- **Confirm:** `boot_set_confirmed()` when the core says the image has
  proven itself (connected, and the application's checks passed).
- **Rollback:** an image that resets before it confirms is swapped back by
  MCUboot at the next boot; the core reports the operation as failed.
- **Version:** the running image's version from its MCUboot header
  (`boot_read_bank_header()` on the primary slot), matching the version the
  build signs in.

The image the cloud offers is the signed application image, built with
`imgtool` as for Zephyr. NuttX's default slot (0x150000, 1.31 MB) is
smaller than the image with every feature (about 1.41 MB on the C6), so
the port's board configurations set a slot size that fits the 8 MB flash
of the DevKitC boards, with the LittleFS data partition after the scratch
area. The layout is recorded in the README and checked by the build
script, which fails when the image does not fit its slot.

*Alternative:* ESP-IDF's OTA format through NuttX's simple boot. NuttX
has no rollback for it, and the core's update flow needs one.

### Shell commands run NSH commands through `popen()`

`ports/nuttx/src/tedge_shell_cmd.c` provides the shell-command hook the
ESP-IDF port provides: it checks the command against
`CONFIG_TEDGE_SHELL_COMMAND_ALLOW_LIST`, runs it with `popen()` (nuttx-apps
`system/popen`, which runs NSH), reads its output up to
`CONFIG_TEDGE_SHELL_COMMAND_OUTPUT_BYTES`, and stops it after
`CONFIG_TEDGE_SHELL_COMMAND_TIMEOUT_S`. The console stays with Improv, so
NSH runs only for these commands and never reads the console.

### Crash dumps through NuttX's core dump, if it works on the ESP chips

NuttX can write a core dump to a block device after a crash
(`CONFIG_BOARD_COREDUMP_BLKDEV`). If that works on the ESP32-C6 and
ESP32-S3, `tedge_port_coredump_init()` offers the stored dump as a log
type and clears it once uploaded, as on Zephyr. If it does not work in
NuttX 13.1, the port leaves `TEDGE_CRASH_DUMP` out and the README says so;
this does not block the rest of the change.

### The application owns the network; the port offers ready-made pieces

As on the other ports, the application brings the network up. The port
offers `<tedge/nuttx.h>`: joining the stored Wi-Fi network with retries,
joining a new one and going back to the stored one when it fails, and
set-up over USB. The sample agent uses all of them. The agent sets the
clock from the network at every start (SNTP, with NuttX's NTP client as
well), because the ESP chips' clock keeps a wrong value through a reset.

### Set-up over USB on the console, with a Wi-Fi scan

Kept from the trial: Improv Serial on `/dev/console` in raw mode, frames
written with output processing off. Added: the `wifi_scan` hook through
`wapi`'s scan, so `tedge-onboard` lists the networks the device sees, as
it does for ESP-IDF; and retries when the device goes back to its stored
network after a failed join. The agent's console output is non-blocking,
so log lines are dropped rather than stalling the client when no program
reads the port.

### NuttX fixes are applied by the build script and sent upstream

The ESP USB serial driver's receive buffer (64 bytes, which cut
`tedge-onboard`'s onboard command) and its unbounded wait for a host
(which hung every restart when no program read the port) have no options.
`scripts/build.sh` patches both for the S3 and the C6 and fails if a patch
no longer applies. Both are offered to NuttX upstream; once a release
carries them, the pin moves and the patches go.

### The released image is the sample agent, built in microcontroller-devices

The agent in `ports/nuttx/samples/agent/` is the same device agent as the
ESP-IDF sample: the device measurement, set-up over USB, every feature.
thin-edge/microcontroller-devices builds it for the ESP32-C6 (first) with
the NuttX container, signs it, and releases it with the other images, with
the device type `thin-edge.io-nuttx` and a firmware entry in
`c8y-firmware.json`, as it does for `tedge-agent-idf`. That is a separate
pull request in that repository, after this change.

### Build in a container, with NuttX and nuttx-apps pinned

Kept from the trial: `ports/nuttx/docker/Dockerfile` with both toolchains,
and `scripts/build.sh` to clone the pinned NuttX, link the app, configure
a board, and build. The script cleans the build when the configuration
changes, because nuttx-apps does not rebuild an app's objects then. CI
runs the same script.

## Risks / Trade-offs

- **[MCUboot's swap and the slot layout on 8 MB flash.]** The default
  layout does not fit the image. Mitigation: a layout of the port's own in
  each board configuration, a size check in the build, and the update and
  rollback tested on the board before release.
- **[The ESP32-S3 is not on hand.]** It builds in CI, but board tests run
  on the C6 first. Mitigation: the S3 board tests are tasks of their own,
  and the README states which board each result comes from.
- **[RAM on the ESP32-C3.]** With no PSRAM, TLS and Wi-Fi share about
  400 KB. The C3 is added only if a build with every feature runs there;
  otherwise the README lists the features that fit.
- **[Wi-Fi after a software reset.]** Joins often fail at first. The
  agent retries without a limit; the README says so.
- **[Patches to NuttX.]** A new NuttX release may change the driver.
  Mitigation: the patches fail loudly, and the pin moves only with a
  rebuild and a board test.
- **[NuttX's NTP client never set the clock on the test network.]** The
  agent also sends its own SNTP request. If NuttX's client is later fixed,
  the agent's request does no harm.

## Open Questions

- The flash layout for the S3 and C6 boards with MCUboot (slot size, data
  partition size). Decided in the firmware-update task, from the measured
  image size.
- Whether NuttX 13.1's core dump to a block device works on the ESP chips.
