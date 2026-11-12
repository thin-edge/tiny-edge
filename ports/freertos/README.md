# tiny-edge FreeRTOS port

The thin-edge.io client on FreeRTOS. ESP-IDF is the first platform: an
ESP-IDF component, `tiny_edge`, for ESP32 applications. It is the same
client as on Zephyr and POSIX: the same core, the same options, and the same
behavior in Cumulocity.

## Layout

```
ports/freertos/
├── include/tedge_port/os.h   kernel types for any FreeRTOS platform
├── src/tedge_port_os.c       kernel services for any FreeRTOS platform
└── esp-idf/
    ├── tiny_edge/            the ESP-IDF component
    │   ├── Kconfig           the client's options (CONFIG_TEDGE_*)
    │   ├── include/          ESP_LOG mapping
    │   └── src/              NVS, esp_netif, SNTP, OTA, device glue
    └── samples/minimal/      Wi-Fi, the client, and a measurement
```

The ESP-IDF build takes its sources from four places:

| Layer | Directory | Used by |
|---|---|---|
| The client | `core/` | Every port |
| BSD sockets with mbedTLS 4, and the device key | `ports/common/` | POSIX and ESP-IDF |
| FreeRTOS kernel services | `ports/freertos/src/` | Every FreeRTOS platform |
| ESP-IDF services | `ports/freertos/esp-idf/tiny_edge/src/` | ESP-IDF |

About 700 lines are ESP-IDF's own. Everything else is shared.

The kernel layer uses only the FreeRTOS API, so another FreeRTOS platform
(an STM32 with lwIP, for example) reuses it and `ports/common/`. Such a
platform then adds settings storage, the network interface queries, the
clock, and its firmware slots.

## Use it in an application

ESP-IDF 6.1 or later. Add the component with the component manager, in your
`main/idf_component.yml`:

```yaml
dependencies:
  tiny_edge:
    git: https://github.com/thin-edge/tiny-edge.git
    path: ports/freertos/esp-idf/tiny_edge
```

Then add `REQUIRES tiny_edge` (or `PRIV_REQUIRES`) to your component, and
start the client once the network is set up:

```c
#include <tedge/tedge.h>

tedge_init(NULL, &hooks);
tedge_start();
```

The application owns Wi-Fi or Ethernet, NVS, and the default event loop, as
in any ESP-IDF application. The client watches the default network
interface, and reconnects when it comes back.

## Configuration

The options are in Kconfig: `idf.py menuconfig`, then "tiny-edge
(thin-edge.io client)", or `CONFIG_TEDGE_*` lines in `sdkconfig.defaults`.
The names and defaults match the other ports. The core sees
`CONFIG_TEDGE_HEALTH` as `TEDGE_HEALTH`.

| ESP-IDF setting | Why |
|---|---|
| A partition table with two OTA app partitions | Firmware update writes the new image to the other one |
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` | An image that does not confirm itself is rolled back. The build stops without it when firmware update is on |
| `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=3072` | The firmware confirmation deadline runs on the timer task |

The component turns on the mbedTLS options it needs: threading, and CSR
writing for enrollment.

## How the port maps the interface

| Interface | ESP-IDF |
|---|---|
| Threads, mutexes, queues, events, semaphores | `xTaskCreateStatic`, `xSemaphoreCreate*Static`, `xQueueCreateStatic`, `xEventGroupCreateStatic`: nothing from the FreeRTOS heap |
| Delayed work | A one-shot FreeRTOS software timer |
| The client's heap | An allowance of `CONFIG_TEDGE_HEAP_SIZE` on `pvPortMalloc` |
| Network | lwIP's BSD sockets, mbedTLS 4.1 for TLS (`ports/common/`), and `esp_netif` and its IP events for the interface |
| Settings | NVS, namespace `tedge`. A key longer than NVS allows is stored under its hash, with the full key beside the value |
| Device key | P-256, generated with PSA, kept as PEM in NVS (`tedge/device-key`). Turn on NVS encryption on a production device |
| Clock | `esp_netif_sntp`, unless the clock is already set |
| Firmware update | `esp_ota_*` with the bootloader's rollback |
| Logging | `ESP_LOG`, tag `tedge`. With log upload, every line the image logs is also kept for upload, cut at 127 bytes |
| Device id | `tedge` and the MAC of the default interface |
| Provisioning (`tedge-onboard`) | Improv Serial on the UART and USB Serial/JTAG consoles, and Wi-Fi with `esp_wifi` |
| Shell commands (`c8y_Command`) | The application's `esp_console` commands, run with `esp_console_run()` on a thread of their own. What the command's task prints to stdout is the result |

## Shell commands

The cloud's shell (`c8y_Command`) runs the application's own console
commands, the ones it registers with `esp_console_cmd_register()`. Nothing
runs until `CONFIG_TEDGE_SHELL_COMMAND_ALLOW_LIST` names what may, for
example `"free,version"`: a command must begin with one of those, and
anything that could chain a second command is refused. `help` answers with
the list.

The result is what the command printed. While a command runs, stdout keeps
only what the command's task prints and passes everything else on to the
console. A command that does not return within
`CONFIG_TEDGE_SHELL_COMMAND_TIMEOUT_S` is reported as failed, but keeps its
thread until it returns, so keep commands that can block off the list.

The sample registers `free`, `version`, and `uptime`, and allows those three.

## Set a device up over USB (tedge-onboard)

With `CONFIG_TEDGE_PROVISIONING=y`, a device can be set up from a computer
it is plugged into: [`tedge-onboard`](../../tools/onboard/README.md) gives it
a Wi-Fi network, registers it in Cumulocity, and passes it the tenant and a
one-time password. The library (`include/tedge/provisioning.h`) answers the
Improv Wi-Fi protocol on the UART console and on the USB Serial/JTAG
console, so the tool finds the board on either USB socket. Log lines and
Improv frames share the console without mixing.

Start it after `tedge_init()`, with the port's ready-made hooks:

```c
#include <tedge/provisioning_esp_idf.h>

tedge_init(NULL, &hooks);
tedge_provisioning_esp_idf_init(NULL);
tedge_start();
```

| Hook | The port's default |
|---|---|
| Wi-Fi | Joins the network and keeps it in NVS. When the device gets no address within 30 seconds, it goes back to the previous network, or clears the stored one |
| Scan | The networks the station sees |
| Identify | None: pass your own, for example a status LED |
| Chip | `CONFIG_IDF_TARGET` |

An application passes the hooks it wants to replace:
`tedge_provisioning_esp_idf_init(&mine)`. Wi-Fi stays the application's:
it starts the station, and joins the stored network at boot when there is
one, as the sample does.

The library runs two threads: one reads the consoles (3 KB stack), and one
handles the requests (`CONFIG_TEDGE_PROVISIONING_STACK_SIZE`, 4 KB).

## Build the sample

With the ESP-IDF container (or a local ESP-IDF 6.1):

```sh
cd ports/freertos/esp-idf/samples/minimal
idf.py set-target esp32c6
idf.py menuconfig    # "tiny-edge sample": Wi-Fi; "tiny-edge": the tenant
idf.py build flash monitor
```

The sample keeps the Wi-Fi network on the device. An image built without
one, such as an image for a firmware update, uses the stored network, so it
carries no Wi-Fi password.

The sample has provisioning on. Flashed without a Wi-Fi network, it waits:
run `tedge-onboard` on the computer the board is plugged into, and the tool
gives it Wi-Fi and registers it in Cumulocity.

Built with a Wi-Fi network in menuconfig instead, the sample logs a
registration URL on the first boot. Open it to register the device in
Cumulocity. The key and certificate stay in NVS, so later boots connect at
once.

## Not in this port yet

- Crash dumps: ESP-IDF's core dump partition would be offered as a log type.
- Improv over BLE: provisioning works over the serial consoles only.
- Plain FreeRTOS builds outside ESP-IDF: the kernel layer is ready, but no
  platform layer or CMake glue exists yet.

## Status

Builds without warnings for the ESP32-C6, the ESP32-S3, and the ESP32, with
every feature and with a reduced set (bootstrap authentication, telemetry
only). The sample with every feature is 1.06 MB on the ESP32-C6, 61% of a
1.75 MB OTA slot.

Tested on an ESP32-C6 against a Cumulocity tenant: certificate enrollment
from the registration URL, the MQTT connection, measurements, the health
measurement, log upload, a remote access tunnel, an operation
interrupted by a reset (reported as failed after the reboot), parameter
changes (kept across a reboot), shell commands, onboarding with
`tedge-onboard` over the USB Serial/JTAG console (the network kept across a
reset), a firmware update from the
tenant's firmware repository (the new image confirmed itself once
connected), and a rollback (an image that never connected was reset after
its confirmation deadline, and the bootloader went back to the previous
one, which reported the update as failed).

Each thread that runs a TLS handshake of its own (log upload, firmware
download, remote access) has a 12 KB stack: the log upload used 8.1 KB on
the ESP32-C6, more than with Zephyr's mbedTLS. In a build with
`CONFIG_TEDGE_LOG_LEVEL=4` and `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` (ESP-IDF
leaves out debug lines otherwise), and the client's `log_level` parameter at
`dbg`, each thread logs how much of its stack it never used when it ends.
