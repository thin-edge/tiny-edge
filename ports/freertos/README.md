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

On the first boot the sample logs a registration URL. Open it to register
the device in Cumulocity. The key and certificate stay in NVS, so later
boots connect at once.

## Not in this port yet

- Cloud shell commands (`c8y_Command`): ESP-IDF's console would run them.
- Crash dumps: ESP-IDF's core dump partition would be offered as a log type.
- The provisioning library (Improv over serial or BLE).
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
changes (kept across a reboot), and a firmware update from the tenant's
firmware repository (the new image confirmed itself once connected). A
firmware rollback has not yet been tested on a board.

Each thread that runs a TLS handshake of its own (log upload, firmware
download, remote access) has a 12 KB stack: the log upload used 8.1 KB on
the ESP32-C6, more than with Zephyr's mbedTLS. In a build with
`CONFIG_TEDGE_LOG_LEVEL=4` (debug), each thread logs how much of its stack
it never used when it ends.
