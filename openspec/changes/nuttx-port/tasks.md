## 1. Build environment (done in the trial)

- [x] 1.1 Pin NuttX and nuttx-apps (`nuttx-13.1.0`) and record it in `ports/nuttx/README.md`
- [x] 1.2 Add `ports/nuttx/docker/` with the Xtensa and RISC-V ESP toolchains
- [x] 1.3 Write `ports/nuttx/scripts/build.sh`: clone the pinned NuttX, link the app, configure a board, build, and check that every option took
- [x] 1.4 Patch NuttX's ESP USB serial driver in the build script (receive buffer, bounded wait), failing when a patch no longer applies
- [x] 1.5 Clean the build when the configuration changes

## 2. Connect and report (done in the trial, on the ESP32-C6)

- [x] 2.1 App skeleton: `Kconfig`, `Make.defs`, `Makefile`; Make source list generated from `core/sources.cmake` and `provisioning/sources.cmake`
- [x] 2.2 Build mbedTLS 4.1 in the app
- [x] 2.3 POSIX port options: `TEDGE_POSIX_THREAD_STACKS`, `TEDGE_POSIX_MAC_IOCTL`, `TEDGE_POSIX_NO_EXEC`, and the `restart` field; POSIX tests on Linux and macOS with each on and off
- [x] 2.4 Sample agent: stored Wi-Fi, the clock from the network at every start, the client, the `device` measurement; non-blocking console output
- [x] 2.5 On the C6: connect over mTLS; check `c8y_Agent`, `tedge_OS`, `c8y_Hardware`, and the measurement in Cumulocity
- [x] 2.6 On the C6: restart from the cloud and log upload, with no program reading the USB port

## 3. Set-up over USB on par with ESP-IDF

- [x] 3.1 Improv Serial on the console (`src/tedge_provisioning_nuttx.c`); store the received network on LittleFS
- [x] 3.2 On the C6: `tedge-onboard --reonboard` sets the device up and it connects
- [x] 3.3 Retry when going back to the stored network after a failed join, so a wrong password cannot leave the device offline
- [x] 3.4 Add the `wifi_scan` hook with `wapi`'s scan
- [ ] 3.5 On the C6, with a cleared data partition: `tedge-onboard` lists the board's networks, sets it up, and it connects; a wrong password brings it back to its stored network
- [x] 3.6 Check that log lines on the console do not break Improv frames during set-up (2026-11-17: four `tedge-onboard --reonboard` runs on the C6, three of them back to back, all connected; during each, the client logs to the same console while the frames are exchanged ("state: stopped", "discarding the certificate…", "provisioning: set up…", "awaiting-registration", "enrolled…"). Raising `log_level` to `dbg` for more traffic had no effect: the build's compile-time level is 3, and the parameter can only lower it)

## 4. Configuration on par with the other ports

- [x] 4.1 Align the Kconfig menu with the ESP-IDF component's `CONFIG_TEDGE_*` names, defaults, and help; keep `CONFIG_TINY_EDGE_*` for the app's own settings
- [x] 4.2 Add `CMakeLists.txt` for NuttX's CMake build, and check it on the C6 (2026-11-17: `ports/nuttx/CMakeLists.txt` with the same sources and flags as the Makefile, the source lists from `core/sources.cmake` and `provisioning/sources.cmake` with the `CONFIG_` prefix; `build.sh --cmake` builds in a worktree of the pinned NuttX (it refuses CMake in a tree configured for Make), keeps the board's full `.config` to start each build from, builds MCUboot with the `bootloader` target, and signs as before. On the C6 the CMake image (update 1,415,852 B, 628 B over Make's) connects, reports, and runs a shell command; a second run takes 33 s. CI builds it, and the reduced set now with `--agent`)
- [x] 4.3 Add a reduced configuration (no firmware update, shell command, log upload, or remote access) and check that it builds and advertises only what it has (2026-11-17: `configs/small.conf` with `--agent` failed the build script's check, because the agent's shell allow-list cannot apply with the shell off; the check now exempts the options of a feature a fragment turns off. On the C6 the reduced agent (update image 1,343,192 B, 72 KB under the full one) advertises only `c8y_Restart`, and a shell command or log request sent anyway fails with its reason. Not fixed here: `c8y_SupportedLogs` and the `remoteAccess` fragment from an earlier full image stay on the device, because the core sends neither when the feature is off; a `core/` fix for a separate pull request)

## 5. Firmware update through MCUboot

- [x] 5.1 Build NuttX's MCUboot bootloader and an MCUboot-format app for the C6; run `mcuboot_update_agent` on the board to confirm the swap, trial boot, and rollback
- [x] 5.2 Choose the slot and data partition layout for 8 MB flash; set it in the board configurations; make the build fail when the image does not fit its slot
- [x] 5.3 Add `ports/nuttx/src/tedge_port_fw.c`: write the secondary slot, mark it pending, confirm on the core's request, and report the running version
- [x] 5.4 Sign the image with `imgtool` in the build script, with the version the image reports
- [x] 5.5 Write the flash steps for a new board (bootloader, slot, data partition) into the README
- [x] 5.6 On the C6: install a newer image from Cumulocity; check the restart, confirmation, version, and operation status
- [x] 5.7 On the C6: install an image that cannot connect; check the rollback and the failed operation

## 6. Shell command and crash dumps

- [x] 6.1 Add `ports/nuttx/src/tedge_shell_cmd.c`: allow-list, `popen()`, output limit, timeout; enable `system/popen` in the configuration
- [x] 6.2 On the C6: an allowed command returns its output; a command not allowed fails with the reason
- [x] 6.3 Check NuttX 13.1's core dump to a block device on the C6; if it works, offer it as a log type through `tedge_port_coredump_init()`, else record it under "not in this port yet" (2026-11-17: recorded under "Not in this port yet". NuttX opens the dump partition (`CONFIG_BOARD_COREDUMP_MTDDEV`) in `nx_bringup()` before any application runs, so the board code must register it, and the ESP32 boards register only the MCUboot slots and storage; the 4 MB layout is also full, so a dump partition would come out of the data partition and wipe updated devices)

## 7. Device management on the board

- [x] 7.1 On the C6: change a parameter set, restart, and check that the values persist (2026-11-17, eu-latest device 9260855432: `tiny_edge.health_interval_s` 900 → 600, restart from the cloud, the board republished 600 from storage; set back to 900)
- [x] 7.2 On the C6: open a remote access tunnel (2026-11-17: passthrough to the LAN router 192.168.188.1:80 under the `lan` policy; three fetches through `c8y remoteaccess server`, each HTTP 200 with the same 7,659 B page as a direct fetch, 4.4–5.8 s each including tunnel set-up; Opened/Closed events with byte counts; sessions back to 0; config deleted)
- [x] 7.3 On the C6: an operation interrupted by a reset is reported as failed after the reboot (2026-11-17: c8y_Firmware with a 1.4 MB image, board reset over USB at 20 % downloaded; 9 s later the operation was FAILED "the device restarted while the operation was running"; the board stayed on 0.6.3)
- [x] 7.4 On the C6: a 30-minute session with telemetry and a tunnel, no program reading the port; check stack use and free heap (2026-11-17, 0.6.4 with `CONFIG_STACK_COLORATION` and the agent's new `stacks` log type, 07:05–07:35 UTC, nothing on the USB port: 52 of 52 tunnel fetches through the C6 (HTTP 200, 5.0 s average, 7.6 s max); 31 `device` measurements with no gap over 61 s and no reboot; client heap 16,135 B and 0 dropped messages throughout; `free` 96.6 → 96.1 → 96.4 KB free. Stacks at the end: highest 63 % (5,212 of 8,160 B), all others ≤ 36 %. Concern: heap `maxused` reached 232 of 248 KB, about 15 KB headroom at the peak, during tunnels)
- [x] 7.5 Certificate renewal: check it with a short renewal window, or record why not (2026-11-17: test image with `CONFIG_TEDGE_CERT_RENEW_BEFORE_DAYS=400`, `CONFIG_TEDGE_CERT_RENEW_CHECK_HOURS=1`; at boot "364 days left; renewing", renewed in 2.3 s, reconnected, twin `renewals` 1 and expiry 06:48:49 → 07:41:05; a restart afterwards reconnected with the renewed certificate; 0.6.4 put back)

## 8. Second chip

- [ ] 8.1 On an ESP32-S3: tasks 2.5, 3.5, 5.6, and 7.1 (when a board is available)
- [x] 8.2 ESP32-C3: build with every feature and record flash and RAM; add it to CI if it fits (2026-11-17, `esp32c3-devkit`, board files copied from the C6's: every feature links, update image 1,403,748 B, dram0 241,520 of 321,296 B, about 80 KB of heap; reduced set 1,331,516 B, dram0 219,008 B, about 102 KB of heap. The agent on the C6 used about 151 KB of heap and peaked at 232 KB, so the full set is not expected to run on a C3; not added to CI, since only the link fit is known)

## 9. CI and documentation

- [x] 9.1 Add `.github/workflows/nuttx.yml`: S3 and C6 with every feature, C6 reduced, in the port's container, on changes to `core/`, `provisioning/`, `ports/posix/`, `ports/common/`, or `ports/nuttx/`
- [x] 9.2 Rewrite `ports/nuttx/README.md` like the ESP-IDF port's: use in an application, options, interface mapping, set-up, the flash layout, status per board, not in the port yet
- [x] 9.3 Add NuttX to the top-level README's port table and port comparison
- [x] 9.4 Update `REPORT.md` with the final sizes and a short summary; keep it as the record of what NuttX needed (2026-11-17: summary, status per stage, findings 24–31, sizes for the S3, C6 and C3 agents, line counts)
- [x] 9.5 Fix the two format warnings in `core/src/tedge_core.c` in a separate pull request (`PRIu32`) (2026-11-17: thin-edge/tiny-edge#20; checked with `riscv-none-elf-gcc -Werror`. The download idle timeout from 5.6 went out separately too, as #21 with unit tests)
- [ ] 9.6 Offer the two USB serial driver fixes to NuttX upstream (deferred 2026-11-17 by the maintainer)
- [ ] 9.7 Confirm that `git diff main -- core/ provisioning/ include/tedge/port/` is empty (open until #20 and #21 merge and this branch is rebased on `main`, dropping its copy of the timeout change)

## 10. Release (thin-edge/microcontroller-devices, separate pull request)

Started once this change and its two `core/` pull requests have merged (decided 2026-11-17).

- [ ] 10.1 Build the NuttX agent for the ESP32-C6 in microcontroller-devices' release workflow, signed, with device type `thin-edge.io-nuttx`
- [ ] 10.2 Add it to the release assets and `c8y-firmware.json`
- [ ] 10.3 Flash a released image on the C6, set it up with `tedge-onboard`, and update it to the next release from Cumulocity
