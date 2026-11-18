# tiny-edge NuttX port

The thin-edge.io client on [Apache NuttX](https://nuttx.apache.org/): a
nuttx-apps application for the ESP32-C6 and the ESP32-S3. It is the same
client as on Zephyr, ESP-IDF, and POSIX: the same core, the same options,
and the same behavior in Cumulocity.

NuttX is a POSIX system, so the port builds on the POSIX port: it compiles
the POSIX port's sources and `ports/common/` unchanged, and adds only what
NuttX needs of its own. How that went, and what NuttX needed, is recorded
in `REPORT.md`.

| | |
|---|---|
| NuttX and nuttx-apps | `nuttx-13.1.0` (pinned in `scripts/build.sh`) |
| mbedTLS | 4.1.0, built by the app (nuttx-apps packages 3.6) |
| MCUboot | NuttX's, for the bootloader and the app-side library |
| Boards | `esp32c6-devkitc`, `esp32s3-devkit` |
| Toolchains | `xtensa-esp-elf` 14.2.0 (esp-14.2.0_20241119), `riscv-none-elf` 13.2.0-2 (xPack) |

## Layout

```
ports/nuttx/
├── Kconfig, Make.defs, Makefile   the nuttx-apps application
├── tools/generate.cmake           tedge_config.h and the Make source list
├── include/tedge/nuttx.h          Wi-Fi and set-up over USB, for applications
├── src/                           what NuttX needs of its own
│   ├── tedge_port_fw.c            firmware update through MCUboot
│   ├── tedge_nuttx_wifi.c         joining, storing, and scanning Wi-Fi (wapi)
│   └── tedge_provisioning_nuttx.c Improv Serial on the console
├── samples/agent/                 the device agent
├── configs/                       configuration fragments and flash layouts
├── scripts/build.sh               clone NuttX, configure a board, build
└── docker/                        the build container
```

The build takes its sources from five places:

| Layer | Directory | Used by |
|---|---|---|
| The client | `core/` | Every port |
| Set-up over USB | `provisioning/` | Every port |
| BSD sockets with mbedTLS 4, and the device key | `ports/common/` | POSIX, ESP-IDF, NuttX |
| Threads, settings, the device, the clock, shell commands | `ports/posix/src/` | POSIX, NuttX |
| Firmware update, Wi-Fi, the console | `ports/nuttx/src/` | NuttX |

## Use it in an application

The port is an external nuttx-apps directory. `scripts/build.sh` sets it up
in a NuttX tree; to do it by hand, add a directory to `apps/external/` whose
`Kconfig`, `Make.defs`, and `Makefile` include this port's:

```sh
mkdir -p apps/external/tiny_edge
echo 'source "/path/to/tiny-edge/ports/nuttx/Kconfig"' > apps/external/tiny_edge/Kconfig
echo 'include /path/to/tiny-edge/ports/nuttx/Make.defs' > apps/external/tiny_edge/Make.defs
echo 'include /path/to/tiny-edge/ports/nuttx/Makefile'  > apps/external/tiny_edge/Makefile
```

Then select `CONFIG_TINY_EDGE`. It needs TCP, IPv4, and DNS
(`CONFIG_NET_TCP`, `CONFIG_NET_IPv4`, `CONFIG_NETDB_DNSCLIENT`); the
fragment `configs/common.conf` lists what the agent turns on besides.

An application configures the POSIX port, then starts the client:

```c
#include <tedge/posix.h>
#include <tedge/tedge.h>

struct tedge_posix_config cfg = {
	.state_dir = "/data/tedge",       /* on a flash file system */
	.interface = "wlan0",
	.restart = board_reset,           /* boardctl(BOARDIOC_RESET, 0) */
};

tedge_posix_configure(&cfg);
tedge_init(NULL, &hooks);
tedge_start();
```

As on the other ports, the application owns the network: it joins Wi-Fi,
and the client waits for an IPv4 address. `<tedge/nuttx.h>` has ready-made
pieces for that, which the agent uses.

In your own NuttX tree, apply the changes `scripts/build.sh` makes to NuttX
13.1 first (see its comments, and findings 10 and 20 in `REPORT.md`). On
the ESP chips, without the USB serial driver changes, `tedge-onboard`'s
set-up command arrives cut and a restart from the cloud hangs when no
program is reading the USB port; without the MCUboot rule, the ESP32-S3's
bootloader does not build with the console on USB. They are not upstream
yet.

## Configuration

The options are in Kconfig, under "tiny-edge (thin-edge.io client)", with
the same `CONFIG_TEDGE_*` names and defaults as on Zephyr and ESP-IDF. The
core sees `CONFIG_TEDGE_HEALTH` as `TEDGE_HEALTH`. The agent's own settings
are `CONFIG_TINY_EDGE_*`:

| Option | Default | |
|---|---|---|
| `CONFIG_TINY_EDGE_STATE_DIR` | `/data/tedge` | Settings and the device key |
| `CONFIG_TINY_EDGE_INTERFACE` | `wlan0` | The network interface |
| `CONFIG_TINY_EDGE_WIFI_FILE` | `/data/wifi.conf` | The stored Wi-Fi network |
| `CONFIG_TINY_EDGE_FIRMWARE_NAME` | `tiny-edge-nuttx-agent` | The name in `c8y_Firmware` |
| `CONFIG_TINY_EDGE_FIRMWARE_VERSION` | tiny-edge's version | The version in `c8y_Firmware` (`build.sh --version`) |
| `CONFIG_TINY_EDGE_MEASUREMENT_INTERVAL_S` | `60` | The agent's `device` measurement |

What NuttX needs set, and why:

| NuttX setting | Why |
|---|---|
| A flash file system for the state directory (LittleFS) | The client keeps its settings and the device key in files |
| `CONFIG_DEV_URANDOM` with `CONFIG_DEV_URANDOM_ARCH` | Random numbers for TLS and the device key, from the chip's RNG |
| `CONFIG_BOARDCTL_RESET` | A restart from the cloud resets the board |
| `CONFIG_INIT_STACKSIZE=8192` when the agent is the first task | The default overflows in the agent's main loop |
| `CONFIG_PIPES` | Shell commands run through `popen()` |
| `CONFIG_BOOT_MCUBOOT` and a board that boots through MCUboot | Firmware update |
| `CONFIG_SERIAL_TERMIOS` | Set-up over USB switches the console to raw mode |

## How the port maps the interface

| Interface | NuttX |
|---|---|
| Threads, mutexes, queues, events | pthreads, with the stack size the core asks for (`TEDGE_POSIX_THREAD_STACKS`) |
| Network | BSD sockets and mbedTLS 4.1 (`ports/common/`) |
| Settings | A file per key under the state directory, on LittleFS |
| Device key | P-256, generated with PSA, kept as PEM in the state directory |
| Clock | SNTP when the clock is not valid (`CONFIG_TEDGE_SNTP`); the agent also runs NuttX's NTP client |
| Firmware update | MCUboot: the secondary slot through nuttx-apps' MCUboot library, swap with a scratch area, rollback when not confirmed |
| Restart | The application's `restart`, `boardctl(BOARDIOC_RESET)` |
| Shell commands (`c8y_Command`) | NSH commands through `popen()` |
| Logging | Standard error. With log upload, every line is also kept for upload |
| Device id | `tedge` and the MAC of the interface, from `SIOCGIFHWADDR` (`TEDGE_POSIX_MAC_IOCTL`) |
| What it runs on (`tedge_OS`, `c8y_Hardware`) | `nuttx` with its release, and the chip (`CONFIG_ARCH_CHIP`) as the model |
| Set-up (`tedge-onboard`) | Improv Serial on the USB serial console, and Wi-Fi with `wapi` |

## Shell commands

The cloud's shell (`c8y_Command`) runs NSH commands through `popen()`.
Nothing runs until `CONFIG_TEDGE_SHELL_COMMAND_ALLOW_LIST` names what may:
a command must begin with one of the listed prefixes, and anything that
could chain a second command is refused. `help` answers with the list. A
command that does not finish within `CONFIG_TEDGE_SHELL_COMMAND_TIMEOUT_S`
is reported as failed but keeps running, so keep commands that can block
off the list. The agent allows `free`, `uname`, `uptime`, and `ps`.

## Stack use

The agent image is built with `CONFIG_STACK_COLORATION`, and the agent then
offers a second log type, `stacks`: one line per thread with its stack
size, the most of it ever used, and the share, read from procfs when the
log is requested. Request it like any log (Device Management, Logs tab) to
check the headroom on a device in the field. `ps` cannot do this from the
cloud: its output is longer than the result Cumulocity gets.

## Set a device up over USB (tedge-onboard)

With `CONFIG_TEDGE_PROVISIONING=y`, [`tedge-onboard`](../../tools/onboard/README.md)
sets a device up from the computer it is plugged into: it gives it a Wi-Fi
network, registers it in Cumulocity, and passes it the tenant and a
one-time password. The port answers Improv Serial on the console, which is
the board's USB serial port, so the console cannot also run a shell.

```c
#include <tedge/nuttx.h>

tedge_init(NULL, &hooks);
tedge_provisioning_nuttx_init(NULL);   /* before joining Wi-Fi */
tedge_nuttx_wifi_join_stored();
tedge_start();
```

| Hook | The port's default |
|---|---|
| Wi-Fi | Joins the network with `wapi` and DHCP, and keeps it in `CONFIG_TINY_EDGE_WIFI_FILE`. When it cannot join, it joins the stored network again |
| Scan | The networks the station sees, each named once, strongest first |
| Identify | None: pass your own |
| Chip | `CONFIG_ARCH_CHIP` |

`tedge-onboard` resets the board when it opens the port, so start set-up
before anything slow, such as joining Wi-Fi, as the agent does.

The agent writes its log lines to the console without blocking: with no
program reading the port, lines are dropped instead of stopping the client.

## Firmware update

The image boots through MCUboot. A firmware update writes the new image to
the secondary slot, MCUboot swaps it in at the next reset, and the client
confirms it once it has connected to Cumulocity. An image that resets
before it confirms is swapped back, and the operation is reported as failed.

Both boards use the same flash layout, which fits 4 MB
(`configs/<board>.mcuboot.conf`):

| Offset | Size | |
|---|---|---|
| `0x000000` | 60 KB | MCUboot |
| `0x020000` | 1.5 MB | Primary slot: the running image |
| `0x1A0000` | 1.5 MB | Secondary slot: the new image |
| `0x320000` | 256 KB | Scratch, for the swap |
| `0x360000` | 640 KB | Data: LittleFS at `/data` |

## Build and flash

In the container from `docker/`, which has both toolchains, MCUboot's build
tools, and `imgtool`:

```sh
docker build -t tiny-edge-nuttx ports/nuttx/docker
docker run --rm -v "$PWD":/ws/tiny-edge tiny-edge-nuttx \
    /ws/tiny-edge/ports/nuttx/scripts/build.sh esp32c6-devkitc --agent
```

The script clones NuttX and nuttx-apps into `ports/nuttx/build-nuttx/`
once, patches the NuttX defects listed in `REPORT.md`, configures the
board's `wifi` configuration with the port's fragments on top, checks that
every option took, and builds. It writes to
`ports/nuttx/build-nuttx/out/<board>/`:

| File | |
|---|---|
| `mcuboot-<chip>.bin` | MCUboot, to flash at `0x0` |
| `nuttx.bin` | The image, padded and confirmed, to flash at `0x20000` |
| `nuttx-update.bin` | The same image for a firmware update from Cumulocity |

| Option | |
|---|---|
| `--agent` | The agent as the first task, with set-up over USB (`configs/agent.conf`) |
| `--version x.y.z` | The firmware version the agent reports and MCUboot's header carries |
| `--extra <fragment>` | Your own configuration on top (`configs/small.conf`: a reduced feature set) |
| `--cmake` | Build with NuttX's CMake build (`CMakeLists.txt`) instead of Make, in a worktree of the same NuttX release; the same files come out |
| `--simple-boot` | No MCUboot: an image to flash at `0x0`, for development |

### Which image for which board

| Board | Build | Notes |
|---|---|---|
| ESP32-C6-DevKitC | `build.sh esp32c6-devkitc --agent` | Every feature |
| ESP32-S3 with octal PSRAM (`N8R8`, `N16R8`, ...) | `build.sh esp32s3-devkit --agent --extra /ws/tiny-edge/ports/nuttx/configs/esp32s3-psram-octal.conf` | Every feature. The image does not start on a board without PSRAM |
| ESP32-S3 with quad PSRAM (`N8R2`, ...) | As above, with a copy of `configs/esp32s3-psram-octal.conf` that sets `CONFIG_ESP32S3_SPIRAM_MODE_QUAD=y` instead of `_MODE_OCT` | Not tested |
| ESP32-S3 without PSRAM (`N8`, ...) | `build.sh esp32s3-devkit --agent --extra /ws/tiny-edge/ports/nuttx/configs/small.conf` | The reduced set: no firmware update, shell command, log upload, or remote access, which need a second TLS session the internal RAM has no room for |
| ESP32-C3-DevKitM | `build.sh esp32c3-devkit --agent` | Builds; not run (see Status) |

The module's name says which an ESP32-S3 board has: the letters after the
flash size, as in `ESP32-S3-WROOM-1-N16R8` (16 MB flash, 8 MB octal PSRAM).
`esptool chip-id` reports the PSRAM's size ("Embedded PSRAM 8MB"); 8 MB
modules have octal PSRAM, 2 MB modules quad.

Flash a new board:

```sh
esptool --chip esp32c6 write-flash 0x0 mcuboot-esp32c6.bin 0x20000 nuttx.bin
```

Then run `tedge-onboard`. A board that ran an image without MCUboot keeps
nothing across the change: its data partition moves.

## Not in this port yet

- Crash dumps. NuttX 13.1 can write a core dump to a flash partition
  (`CONFIG_BOARD_COREDUMP_MTDDEV`), but it opens that partition during
  start-up, before any application runs, so the board code has to
  register it, and the ESP32-C6 and ESP32-S3 boards register only the
  MCUboot slots and the data partition. The 4 MB layout also has no room
  left: a dump partition would come out of the data partition, which
  erases the settings and the device key of every board updated to it.
  Adding it takes a NuttX board change and a new layout.
- Improv over BLE: set-up works over the USB serial console only.
- Set-up on a second serial port. On boards with two USB connectors (the
  ESP32-C6 and ESP32-S3 DevKitC), set-up works on the one labelled USB,
  the console. Serving it on UART0 as well, behind the connector labelled
  UART, was tried and dropped: opening UART0 while only the USB connector
  was plugged in stopped the ESP32-S3 during start-up (enabling UART0
  without opening it was fine), and with the console on USB nothing showed
  why. The ESP32-C3-DevKitM, whose only connector reaches UART0, has its
  console there instead.
- Boards other than the ESP32-C6 and ESP32-S3; the ESP32-C3 builds (see Status).

## Status

Builds for the ESP32-C6 and the ESP32-S3 (with octal PSRAM) with every
feature, for the ESP32-C6 with a reduced set, and for the ESP32-C6 with
NuttX's CMake build (all four in CI). The agent with every feature is
1.41 MB on the ESP32-C6 and 1.16 MB on the ESP32-S3, in a 1.5 MB slot.

Tested on an ESP32-C6 against a Cumulocity tenant, with no program reading
the USB port: set-up with `tedge-onboard`, certificate enrollment and
renewal, the MQTT connection, the twin fragments, measurements, the health
measurement, log upload, a restart from the cloud, shell commands,
parameters that persist across a restart, remote access, a firmware update
from Cumulocity and its rollback, an operation interrupted by a reset, the
reduced set, the CMake build, and a 30-minute run with telemetry and
tunnels (steady heap, at most 63 % of any thread's stack).

Tested on an ESP32-S3-DevKitC-1 N16R8 with its PSRAM: set-up from an
erased data partition, the connection and reports, log upload, parameters
across a restart, and a firmware update from Cumulocity. Without the PSRAM
fragment, log upload failed for lack of heap.

The ESP32-C3 (`esp32c3-devkit`) builds but has not run. Every feature
links (1.34 MB image; dram0 241,152 of 321,296 B), but that leaves about
80 KB of heap, where the agent on the ESP32-C6 used about 151 KB and peaked
at 232 KB. The reduced set leaves about 102 KB. It is not in CI.
