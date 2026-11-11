# tiny-edge

A [thin-edge.io](https://thin-edge.io) device-management client for
microcontrollers. Add it to your firmware to connect the device to Cumulocity
and get remote access, firmware update, telemetry, log retrieval, and
parameters next to whatever the firmware already does.

The client is written once, in portable C, and runs on a real-time operating
system through a thin port. Each port follows the conventions of its RTOS,
so the client looks like a native library to the people who use that RTOS.

| Port | Status | How you add it |
|---|---|---|
| [Zephyr](ports/zephyr/README.md) | Verified on ESP32-C6, ESP32-S3, and ESP32 | A Zephyr module: a west project, Kconfig, profiles |
| [FreeRTOS](ports/freertos/README.md): ESP-IDF first | Builds for ESP32-C6, ESP32-S3, and ESP32; not yet run on a board | An ESP-IDF component with Kconfig; the FreeRTOS kernel layer is ready for other FreeRTOS platforms |
| Eclipse ThreadX | Planned | CMake library with a user configuration header |
| [POSIX](ports/posix/README.md): Linux and macOS hosts; QNX and VxWorks RTP later | Run against Cumulocity on macOS; builds and passes its tests on Linux and macOS | A CMake library with CMake options |

For example applications, see
[microcontroller-devices](https://github.com/thin-edge/microcontroller-devices):
Modbus TCP, OPC-UA, and SNMP servers with tiny-edge built in.

## Repository layout

```
tiny-edge/
├── include/tedge/
│   ├── tedge.h          the public API, the same on every RTOS
│   └── port/            the port interface: what each port implements
├── core/                the client: Cumulocity protocol, operations, parameters,
│                        telemetry buffering, and the decision logic;
│                        sources.cmake lists its files by feature for every port
├── ports/
│   ├── zephyr/          build glue, Kconfig, profiles, samples, tests, and the
│   │                    Zephyr-only sources
│   ├── freertos/        FreeRTOS kernel layer, and esp-idf/: the ESP-IDF
│   │                    component (tiny_edge) and its sample
│   ├── posix/           CMake library, options, sample, and tests
│   └── common/          not a port: BSD sockets with mbedTLS, and the device
│                        key, shared by the POSIX and ESP-IDF ports
├── provisioning/        optional library: set a device up over Improv Wi-Fi
│                        (include/tedge/provisioning.h)
├── tools/onboard/       tedge-onboard: set a device up from a computer
├── certs/               default trust anchors
├── cumulocity/          tenant setup: Smart Functions and DTM definitions
├── openspec/            specifications and change history
├── scripts/             repository checks
├── zephyr/module.yml    makes the repository a Zephyr module
└── VERSION
```

## How the code is split

**`core/`** holds everything that does not depend on the RTOS. It uses C,
the client's own headers, and the port interface, and speaks MQTT, HTTP,
and WebSocket itself. For parsing certificates and ASN.1 it uses mbedTLS as
a plain C library; anything that needs the device's key is the port's. It has no RTOS headers and
no Kconfig symbols. Features are switched with `TEDGE_*` macros from a
configuration header, so the core builds the same way everywhere.

**`ports/<rtos>/`** holds what does depend on the RTOS. A port is small and
follows the RTOS's own conventions for:

| Concern | Zephyr port | FreeRTOS port (ESP-IDF) | POSIX port | Other ports |
|---|---|---|---|---|
| Packaging | Zephyr module, west project | ESP-IDF component | CMake library | The RTOS's component or library format |
| Configuration | Kconfig `CONFIG_TEDGE_*`, mapped to `TEDGE_*` | Kconfig `CONFIG_TEDGE_*`, mapped to `TEDGE_*` | CMake options `TEDGE_*` | `tedge_user_config.h`, in the style of `FreeRTOSConfig.h` and `tx_user.h` |
| Threads and locks | `k_thread`, `k_mutex`, work queues | Static FreeRTOS tasks, semaphores, event groups, software timers | pthreads | The RTOS's task and mutex API |
| Network and TLS | Zephyr sockets with TLS credentials | lwIP's BSD sockets with mbedTLS (shared with POSIX) | BSD sockets with mbedTLS | The RTOS's sockets with its usual TLS stack (mbedTLS, NetX Secure, OpenSSL) |
| Persistent settings | Settings subsystem | NVS | A file per key | The RTOS's key-value store, or a file |
| Firmware update | MCUboot | OTA partitions with the bootloader's rollback | Not yet | MCUboot, or the platform's own OTA mechanism |
| Logging and shell | Zephyr logging and shell | `ESP_LOG`; no shell yet | Standard error | The RTOS's logging |

The port interface sits at the transport level: a port provides a TLS byte
stream, and the core speaks MQTT, HTTP, and WebSocket over it. One protocol
implementation then serves every port, and a new port needs no protocol code.

### The port interface

`include/tedge/port/` lists what a port implements, one header per concern.
Functions a port implements are prefixed `tedge_port_`.

| Header | What the port provides |
|---|---|
| `config.h` | `<tedge_config.h>`: the build configuration as `TEDGE_*` macros. Zephyr generates it from Kconfig. |
| `log.h` | `<tedge_port/log.h>`: the `TEDGE_LOG_*` macros on the RTOS's logging, and the run-time log level |
| `os.h` | `<tedge_port/os.h>`: the kernel object types. Mutexes, spinlocks, threads on static stacks, message queues, event flags, semaphores, delayed work, atomics, uptime, random numbers, and the client's own heap |
| `time.h` | The wall clock: whether it is set, setting it from the network, reading it |
| `device.h` | The MAC address, the reset and its cause, and crash dumps |
| `kv.h` | Persistent key-value storage: the tenant, credentials, parameter values, the state of an update |
| `net.h` | Whether the network is up, name lookup, TCP and TLS streams, UDP, and the TLS credentials |
| `crypto.h` | The device key (generated once, never handed out), a certificate request signed with it, and random bytes |
| `firmware.h` | The update slot: write a new image, boot it on trial, confirm it, and the running image's version |

The boundary is checked in CI. `scripts/check-portability.py` fails when
code in `core/`, `include/tedge/tedge.h`, or `include/tedge/port/` uses an
RTOS API, or when one port refers to another. `tests/host-compile/` compiles every
core file with the host's compiler and C library against a port that has
only types, which catches what the script's patterns miss.

## The public API

`include/tedge/tedge.h` is the API on every port: functions prefixed
`tedge_`. Which features are compiled in is a build-time choice, so an image
carries only what the board can afford. The Zephyr port documents the API
in full, with the integration contract and the messages the client
publishes: [ports/zephyr/README.md](ports/zephyr/README.md).

## Cumulocity tenant setup

Devices talk to the Cumulocity MQTT Service on thin-edge.io `te/` topics.
[`cumulocity/`](cumulocity/README.md) holds the Smart Functions that map
them, and the DTM definitions for the client's own parameter sets.

## Releases

Pull request titles are Conventional Commits. release-please keeps a release
pull request open with the next version and the changelog, and merging it
tags the release. Applications pin a release tag, not a branch.

## History

The client was developed as `tedge-zephyr` inside microcontroller-devices,
and moved here with its history. Commit messages that refer to a pull
request number refer to `thin-edge/microcontroller-devices`.

## License

Apache License 2.0. See [LICENSE](LICENSE).
