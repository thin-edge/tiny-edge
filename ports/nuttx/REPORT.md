# NuttX trial: what was reused

The record of the trial in `openspec/changes/nuttx-port`, kept up to date
stage by stage. Sizes are measured, not estimated.

## Status

| Stage | ESP32-S3 | ESP32-C6 |
|---|---|---|
| Build: tiny-edge, mbedTLS 4.1, sample agent | Builds and links | Builds and links |
| 1. Connect over mTLS, send the `device` measurement | Not run (no board yet) | Works |
| 2. Restart, settings on LittleFS, parameters, remote access | Not run | Restart and log upload work; parameters and remote access not run |
| 3. Set-up with `tedge-onboard` | Not run | Works, `tedge-onboard` unchanged |
| 4. Firmware update through MCUboot | Builds (MCUboot and the app) | Builds; MCUboot's swap and rollback work on the board; an update from Cumulocity not run yet |

## Reuse by port interface

| Interface | On NuttX | Change |
|---|---|---|
| `os.h` | POSIX port, unchanged code | Option `TEDGE_POSIX_THREAD_STACKS`: the core's stack sizes |
| `log.h` | POSIX port, unchanged | None |
| `kv.h` | POSIX port, unchanged (files on LittleFS) | None |
| `net.h` | POSIX port and `ports/common`, unchanged | None |
| `crypto.h` | POSIX port and `ports/common`, unchanged | None |
| `time.h` | POSIX port, unchanged (NTP from the application) | None |
| `device.h` | POSIX port | Option `TEDGE_POSIX_MAC_IOCTL`; field `restart` and option `TEDGE_POSIX_NO_EXEC` |
| `firmware.h` | Not yet (stage 4) | |
| Provisioning transport | New: `src/tedge_provisioning_nuttx.c` (Improv Serial on the console) | None |

`core/`, `provisioning/`, and `include/tedge/port/` are unchanged.

## Findings

1. **The core, `ports/common`, and all of the POSIX port but one file
   compiled for NuttX unchanged.** With `make -k` over the whole tree, the
   only error was `<net/if_dl.h>` in `ports/posix/src/tedge_port_device.c`.
2. **`getifaddrs()` exists on NuttX but lists no link-layer entries.** The
   MAC address is in `ifa_data` of the IPv4 entry instead, which no other
   system does. The POSIX port gained `TEDGE_POSIX_MAC_IOCTL`, which reads
   it with `SIOCGIFHWADDR` (Linux has it too).
3. **The POSIX port ignored the core's stack sizes.** Its stack placeholder
   is one byte and every thread got the system default, which on NuttX is
   2 KB. `TEDGE_POSIX_THREAD_STACKS` records the size in the placeholder
   and passes it to `pthread_attr_setstacksize()`. The stack is not taken
   from static memory, because a thread that has just ended may still be
   running on it when its successor starts. The sizes are the ESP-IDF
   port's (8 KB for the client, 12 KB for threads that run TLS).
4. **No `exec()` on the C6's configuration.** `execv()` is only in NuttX
   with `CONFIG_LIBC_EXECFUNCS`, which the S3's `wifi` configuration has
   and the C6's does not, so the C6 failed to link. The POSIX port gained
   a `restart` field in `struct tedge_posix_config` (the sample uses
   `boardctl(BOARDIOC_RESET)`) and `TEDGE_POSIX_NO_EXEC`.
5. **`getentropy()`, `MSG_NOSIGNAL`, and `uname()` needed nothing.** NuttX
   has all three. `uname()` reports the architecture (`xtensa`,
   `risc-v`) as the machine, so the sample sets the chip as
   `hardware_model` through `struct tedge_identity`, which already allows
   it.
6. **mbedTLS 4.1 cannot come from nuttx-apps**, which packages 3.6.2 and
   whose Makefile builds a 3.x layout. The port builds 4.1 itself from the
   release tarball (checksum verified), with mbedTLS's Unix code paths
   (`-Dunix`, as nuttx-apps does for 3.6) and `/dev/urandom` from the
   hardware RNG (`CONFIG_DEV_URANDOM_ARCH`). The release ships its
   generated headers in `tf-psa-crypto/dispatch`, so no Python step is
   needed.
7. **Two format warnings in the core** (`core/src/tedge_core.c` lines 392
   and 448): `%u` with a `uint32_t`, which is `unsigned long` on these
   toolchains. A fix (`PRIu32`) belongs in a separate change to `core/`.
8. **48 KB of static RAM is the shared socket table** (`ports/common`,
   16 connections of about 3 KB). Not specific to NuttX, but it is two
   thirds of tiny-edge's static RAM here and worth sizing per port.
9. **NuttX 13.1 has no CMake build for the ESP32-S3**, only for the C6. The
   app has a Make build only so far; a `CMakeLists.txt` can be tested only
   on the C6.
10. **The ESP USB serial driver needs two fixes, both in NuttX.** It
    receives into a fixed 64-byte buffer and discards what does not fit, so
    `tedge-onboard`'s onboard command (about 90 bytes) arrived cut and was
    ignored. And kernel output waits for room in the port's FIFO with no
    limit: with no program on the computer reading the port, the FIFO never
    empties, so `board_reset()`'s "reboot status" line hung every restart
    from the cloud. Neither has an option, so `scripts/build.sh` patches the
    driver for both chips (a 512-byte receive buffer; a bounded wait that
    then drops output, as ESP-IDF's driver does). Both are worth sending
    upstream.
11. **Console writes from the application block too.** The same full FIFO
    blocked `printf()` and the client's log lines, which stopped the client
    a minute or several after it connected, as soon as a log line did not
    fit. The sample sets stdout and stderr to `O_NONBLOCK`, so log lines are
    dropped instead. Set-up over USB writes its frames through its own,
    blocking descriptor, while `tedge-onboard` is reading.
12. **Improv frames need the console in raw mode, and the switch must be
    `TCSANOW`.** NuttX's console echoes, collects lines, and maps CR and LF.
    `TCSADRAIN` crashed: the ESP USB serial driver has no `txempty`
    operation, and draining calls it.
13. **nuttx-apps does not rebuild an application when the configuration
    changes.** Objects built under an earlier configuration were linked in
    with another `struct stat` layout, so the POSIX port read every file's
    size wrong and the client lost its tenant at each restart.
    `scripts/build.sh` now cleans when the configuration differs from the
    last build's.
14. **The clock keeps counting through a reset, from a wrong value.** After
    a reset the C6's clock read years such as 403000, which passed the
    sample's "is the clock set" check, and TLS then failed with "certificate
    validity has expired". The POSIX port now counts a clock after 2100 as
    not set too, and sets it with SNTP when `TEDGE_SNTP` is on, as ESP-IDF
    does; the NuttX port turns it on.
15. **NuttX's NTP client never set the clock on the test network.** It asks
    only after a ping to the gateway answers within 100 ms, and it
    collected no samples even with the gateway answering. The client's own
    SNTP request (`TEDGE_SNTP`) works. Turning Wi-Fi power save off
    (`wapi_set_power_save()`) is not supported by the C6's driver.
16. **After a join times out, the ESP Wi-Fi driver keeps trying in the
    background**, and refuses every later join and every scan at once until
    it stops. The port now disconnects after a failed join, and before a
    scan that was refused. The sample also keeps trying the stored network,
    with a growing wait, until it joins or set-up over USB gives it one.
17. **The application's first task needs a larger stack.** The sample is
    `CONFIG_INIT_ENTRYPOINT` with `CONFIG_INIT_STACKSIZE=8192`; the default
    is too small for the client's start-up.

18. **NuttX's default MCUboot slot (1.31 MB) is too small** for the image
    with every feature (1.41 MB on the C6). The port sets its own layout for
    4 MB of flash: two 1.5 MB slots, the scratch area, and a 640 KB data
    partition. The S3 names the same settings `CONFIG_ESP32S3_*`, the C6
    `CONFIG_ESPRESSIF_*`, so each board has its own fragment.
19. **NuttX signs the MCUboot image with a fixed version** (1.0.0 on the
    C6, 0 on the S3, with no option on the S3), padded and confirmed. A
    firmware update must be neither, so `scripts/build.sh` signs both
    images itself with `imgtool`, with the version given to `--version`.
20. **The ESP32-S3's MCUboot does not build with the USB serial console.**
    Its build rules name the console UART only for a UART console, so the
    bootloader fails to compile. The C6's rules handle it; `scripts/build.sh`
    adds the same line for the S3. Worth sending upstream with the USB
    serial driver fixes.
21. **Two features moved into the POSIX port as generic options**, because
    `popen()` and SNTP are POSIX: the shell command (`TEDGE_SHELL_COMMAND`,
    through `popen()`) and setting the clock (`TEDGE_SNTP`). Both are off on
    Linux and macOS by default and have their own tests.

22. **A zeroed `pthread_mutex_t` is not a mutex on NuttX.** The POSIX
    port's thread join locked the mutex of a thread that had never started,
    which glibc and macOS accept and NuttX waits on for ever. The first
    shell command from the cloud hung the client. A join of a thread that
    never started now returns at once, as on FreeRTOS, with a test.
23. **NuttX's `pclose()` returns 0 or -1, not the command's status**, and
    without `CONFIG_SCHED_CHILD_STATUS` it returns -1 (`ECHILD`) for a
    command that has already finished. The shell command option now
    selects child status, and the POSIX port treats `ECHILD` as "the
    status is not available", as POSIX defines it. A command's exit code
    is not available on NuttX; a command that fails to run is.

## Sizes

Images built from each board's `wifi` configuration, with and without
tiny-edge (all features on except firmware update, shell command, and core
dump; provisioning off).

| Board | Stock `wifi` image | With tiny-edge | Difference |
|---|---|---|---|
| ESP32-S3 (`esp32s3-devkit`) | 800,504 B | 1,155,508 B | +355,004 B |
| ESP32-C6 (`esp32c6-devkitc`) | 985,700 B | 1,410,060 B | +424,360 B |

tiny-edge's own static RAM (`.bss` and `.data`) on the S3 is about 72 KB,
of which 48 KB is the socket table (finding 8). Free heap after a TLS
connection is measured on the board in stage 1.

## Files

| | Lines | What |
|---|---|---|
| Reused unchanged | | `core/`, `ports/common/`, `ports/posix/src/` apart from two files |
| Changed in `ports/posix/` | | `os.h`, `tedge_port_os.c`, `tedge_port_device.c`, `posix.h`, `CMakeLists.txt` |
| New in `ports/nuttx/` | | Build glue, configurations, the sample agent |

The line counts are filled in at the end of the trial.
