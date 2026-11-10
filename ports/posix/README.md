# tiny-edge POSIX port

The thin-edge.io client as a C library for a POSIX process: Linux and macOS
today, and QNX and VxWorks (RTP) later, which are POSIX too. It is the same
client as on a microcontroller: the same core, the same options, and the
same behavior in Cumulocity.

Use it to run the client on a gateway or a test host, to develop an
application before the hardware exists, or to run the core against a real
tenant in CI.

## Build

The port is a CMake project. It needs CMake 3.20, a C11 compiler (GCC or
Clang), and pthreads.

```sh
cmake -S ports/posix -B build -DTEDGE_C8Y_URL=example.cumulocity.com
cmake --build build
ctest --test-dir build
```

This builds the `tedge` library, the `tedge-minimal` sample, and the port's
tests.

### mbedTLS

The port uses mbedTLS 4.1 for TLS and for the device key, the same version
as the Zephyr port. CMake looks for an installed mbedTLS 4.1 (its CMake
package). Without one, it downloads the 4.1.0 release, checks its SHA-256,
and builds it with the rest. `-DTEDGE_MBEDTLS_FETCH=ON` always builds it.

The client uses TLS on several threads at once, so an installed mbedTLS must
be built with `MBEDTLS_THREADING_C` and `MBEDTLS_THREADING_PTHREAD`. The
build stops with an error if it is not.

### In your application

```cmake
add_subdirectory(path/to/tiny-edge/ports/posix tiny-edge)
target_link_libraries(my_app PRIVATE tedge::tedge)
```

Set the client's options before `add_subdirectory()`, or on the command
line.

## Options

The options are CMake cache variables with the names the core reads, so
`TEDGE_REMOTE_ACCESS` here is `CONFIG_TEDGE_REMOTE_ACCESS` on Zephyr. The
defaults are the Zephyr port's. [cmake/options.cmake](cmake/options.cmake)
lists every option with its description.

| Option | Default | |
|---|---|---|
| `TEDGE_C8Y_URL` | (empty) | The tenant. The application can also set it at run time |
| `TEDGE_AUTH` | `c8y-ca` | `c8y-ca`: a certificate from the Cumulocity CA. `bootstrap`: basic authentication |
| `TEDGE_C8Y_MQTT` | `service` | `service`: the MQTT Service endpoint. `core`: Core MQTT |
| `TEDGE_TELEMETRY`, `TEDGE_HEALTH`, `TEDGE_RESTART` | `ON` | |
| `TEDGE_LOG_UPLOAD`, `TEDGE_PARAMETERS`, `TEDGE_CERT_RENEWAL` | `ON` | |
| `TEDGE_REMOTE_ACCESS` | `ON` | With `TEDGE_REMOTE_ACCESS_TARGETS`: `lan`, `list`, or `local` |
| `TEDGE_LOG_LEVEL` | `3` | 1 error, 2 warning, 3 info, 4 debug |
| `TEDGE_HEAP_SIZE` | `16384` | The most the client allocates, as on a microcontroller |

The configuration ends up in `<build>/generated/tedge_config.h`.

Firmware update, the shell command, and crash dumps are not in this port
yet. Each needs a POSIX design of its own: an image format and a launcher
that can roll back, a command runner, and the system's core dumps. Turning
one on stops the build with a message.

## Running the sample

```sh
build/samples/minimal/tedge-minimal -s ~/.tedge-posix -c example.cumulocity.com
```

On the first run, the client generates the device key and prints a
registration URL. Open it to register the device in Cumulocity. The client
then enrolls, stores its certificate, and connects. Later runs connect at
once.

A device registered beforehand (with `c8y deviceregistration register-ca`
or a provisioning server) gets its one-time password with `-o`, and the
client prints no URL:

```sh
tedge-minimal -s ~/.tedge-posix -c example.cumulocity.com -d my-device -o <password>
```

| Flag | |
|---|---|
| `-s` | The state directory (default `.tedge-posix`) |
| `-c` | The tenant, when the build does not name it |
| `-i` | The network interface (default: the first that is up) |
| `-d` | The external ID (default: made from the interface's MAC address) |
| `-o` | A one-time password the device was registered with |

## What the application provides

A microcontroller image has its settings partition, its one network
interface, and its version built in. A process names them once, before
`tedge_init()`, in `<tedge/posix.h>`:

```c
#include <tedge/posix.h>
#include <tedge/tedge.h>

int main(int argc, char **argv)
{
	struct tedge_posix_config cfg = {
		.state_dir = "/var/lib/my-app/tedge",
		.firmware_name = "my-app",
		.firmware_version = "1.4.0",
		.argv = argv,
	};

	if (tedge_posix_configure(&cfg) != 0 || tedge_init(NULL, NULL) != 0 ||
	    tedge_start() != 0) {
		return 1;
	}
	/* ... the application's own work ... */
}
```

| Field | |
|---|---|
| `state_dir` | Where the client keeps its settings and the device key. Created with mode 0700 |
| `interface` | The interface the client uses. NULL: the first that is up and has an IPv4 address |
| `firmware_name`, `firmware_version` | What Cumulocity shows. NULL: the program's name and `0.0.0` |
| `argv` | `main()`'s `argv`. A restart starts the program again with it. NULL: a restart exits with status 75 (`TEDGE_POSIX_EXIT_RESTART`) for systemd or launchd to restart |

## How the port maps the interface

| Interface | POSIX port |
|---|---|
| `os.h` | pthreads. Each thread gets the system's default stack, since the core's stack sizes are a microcontroller's. Delayed work runs on one thread of the port's. The heap is `malloc()` with an allowance of `TEDGE_HEAP_SIZE` |
| `log.h` | One line per call on standard error, with a UTC timestamp. The level can change at run time |
| `kv.h` | One file per key under `<state_dir>/kv/`, replaced atomically (write, `fsync()`, `rename()`) |
| `net.h` | Non-blocking BSD sockets, mbedTLS for TLS 1.2, `getaddrinfo()`, and `getifaddrs()`. The network state is polled every 2 seconds |
| `crypto.h` | The device key as `<state_dir>/device-key.pem` (P-256, mode 0600), as thin-edge.io keeps its keys on Linux. Random numbers from `getentropy()` |
| `time.h` | The system clock. Setting it is the system's job (NTP), so the port never does |
| `device.h` | The MAC address of the client's interface. A restart is `execv()` of the same command line. There is no reset cause |

The DNS timeout is the system resolver's own (`resolv.conf`), because
`getaddrinfo()` cannot be cancelled.
