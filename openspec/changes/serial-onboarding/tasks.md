## 1. Client prerequisites

- [x] 1.1 Verify that `tedge_stop()` followed by `tedge_set_c8y_url()`, `tedge_set_enroll_otp()` and `tedge_start()` is clean while the client waits for registration, on the POSIX port (sample against a tenant) and on Zephyr native_sim; fix the core if a thread, socket or event survives the stop
- [x] 1.2 Add a host test of that stop-start cycle to the POSIX port tests, so the library can rely on it

## 2. Provisioning library (portable, `provisioning/`)

- [x] 2.1 Public header `include/tedge/provisioning.h`: init with the Wi-Fi and identify hooks, the transport's write callback, feeding received bytes, and the current Improv state
- [x] 2.2 RPC layer: packet validation (length, checksum), Improv commands 0x01 to 0x03, errors 0x01 to 0x04, result packets with length-prefixed strings
- [x] 2.3 The Wi-Fi flow: provisioning state, the application's hook, success when `tedge_port_net_is_up()`, "unable to connect" after 30 s
- [x] 2.4 tiny-edge info (0xE0), onboard (0xE1) and identify (0xE2, with the application's hook), with the protocol version, the re-onboard flag, stop-set-start of the client, and no password in logs above debug
- [x] 2.5 Improv Serial framing: a byte-at-a-time parser that skips non-frame bytes, and a frame writer that hands one whole frame to the transport
- [x] 2.6 Host unit tests of the parser and the RPC layer (a C test in the POSIX port's tests, built by CMake and run in CI): every command, corrupted and split frames, log bytes between frames, oversized strings
- [x] 2.7 Extend `scripts/check-portability.py` to apply the core rules to `provisioning/`, and `tests/host-compile/run.sh` to compile it

## 3. Zephyr glue

- [ ] 3.1 Kconfig `TEDGE_PROVISIONING`, `TEDGE_PROVISIONING_SERIAL` (and `TEDGE_PROVISIONING_BLE` declared but depending on `TEDGE_EXPERIMENTAL_FEATURES` until phase 2); CMake builds the library through `provisioning/sources.cmake`
- [ ] 3.2 Console transport: the `tedge improv` shell command using the shell bypass, the 60 s idle timeout, frame writes under the shell's output lock; the shell-less path reading the console UART
- [ ] 3.3 `samples/minimal`: an `overlay-onboarding.conf` and a Wi-Fi hook that joins and stores the network (Wi-Fi credentials subsystem)
- [ ] 3.4 Kconfig case for the new options in `tests/kconfig/cases`, and a twister build of the overlay for the ESP32-S3 and ESP32-C6
- [ ] 3.5 Measure flash and RAM with and without the library on the ESP32-S3-DevKitC, ESP32-C6 and ESP32-WROOM-32 sample builds, and record them in proposal.md

## 4. Host tool (`tools/onboard/`)

- [ ] 4.1 Go module and the guided shell: steps with a spinner and a one-line result, choices with `charmbracelet/huh` (and its accessible mode), retry offers, "press Enter to close" when started outside a terminal, plain-language message catalogue
- [ ] 4.2 Script mode: `--yes`, `--json`, `--port`, `--wifi-ssid`, `--wifi-password-stdin`, `--skip-wifi`, `--reonboard`, `--timeout`, exit codes
- [ ] 4.3 Improv Serial client: frame codec, requests with timeouts, skipping log output; unit tests against recorded byte streams
- [ ] 4.4 Board discovery: list ports by USB vendor ID, open with DTR and RTS low, send `tedge improv` and the info request, describe each board that answers; wait for a board to be plugged in; the identify choice
- [ ] 4.5 Wi-Fi: current network and known networks on macOS (ipconfig, networksetup, Keychain), Linux (nmcli), Windows (netsh); hidden password prompt; re-ask when the board cannot join; unit tests with a fake command runner
- [ ] 4.6 go-c8y-cli: detect and explain installation, list sessions by tenant, run the interactive login in the same terminal, tenant host, existing-device check, `register-ca` with the chosen name and a generated password, waiting for the identity and availability; unit tests with a fake runner
- [ ] 4.7 The end-to-end flow tying 4.3 to 4.6, with the device's name and link at the end and "set up another board"
- [ ] 4.8 README for the tool written for a non-technical reader: download, run, what it reads from the computer and why, what to do when a step fails
- [ ] 4.9 Walk through the flow with someone unfamiliar with it (or a scripted fresh-user check), and fix every question that needed explaining

## 5. CI and release

- [ ] 5.1 Workflow: `go vet`, `go test`, and builds of the tool for Linux, macOS and Windows (amd64 and arm64)
- [x] 5.2 Build and run the library's host tests in the posix workflow
- [ ] 5.3 Attach the tool's binaries to tiny-edge releases

## 6. End-to-end verification

- [ ] 6.1 Flash the Zephyr sample with the onboarding overlay to the ESP32-S3-DevKitC on this computer, run `tedge-onboard`, and confirm the device joins Wi-Fi, enrolls, and connects in Cumulocity
- [ ] 6.2 Run it again without `--reonboard` (refused), and with it (new certificate)
- [ ] 6.3 Check that ESP Web Tools or another standard Improv Serial client can at least set Wi-Fi on a shell-less build
- [ ] 6.4 Remove the test device from the tenant

## 7. Phase 2: BLE (a separate pull request)

- [ ] 7.1 Zephyr: the Improv GATT service and advertising, feeding the same RPC layer; measure RAM on each board
- [ ] 7.2 Tool: `--ble` transport with `tinygo.org/x/bluetooth`
- [ ] 7.3 End-to-end over BLE on the ESP32-S3
